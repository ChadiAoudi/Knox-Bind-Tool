#include "input.h"
#include "config.h"
#include "terminal.h"
#include "framework.h"
#include <set>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <fstream>
#include <string>
#include <cstring>

static std::thread input_thread;
static std::atomic<bool> stop_flag{false};
static std::atomic<bool> ui_wants_keyboard{false};
static DWORD input_thread_id = 0;

static std::mutex init_mutex;
static std::condition_variable init_cv;
static bool init_done = false;
static bool init_ok = false;

static std::atomic<bool> kb_mode{false};
static int ctrl_tap_count = 0;
static ULONGLONG last_ctrl_release = 0;
static bool ctrl_solo = true;
static const ULONGLONG CTRL_TAP_WINDOW = 800;

static std::unordered_map<std::string, int> payload_indices;
static std::mutex payload_mutex;

struct sloptimus {
    std::string tool;
    bool requires_clipboard;
    std::vector<std::string> args;
};

static std::set<uint32_t> pressed_keys;
static std::set<uint32_t> max_pressed;
static HHOOK microslopt = nullptr;

static std::string read_clipboard(){
    if(!OpenClipboard(nullptr)) return {};
    HANDLE h = GetClipboardData(CF_TEXT);
    if(!h){ CloseClipboard(); return {}; }
    const char* text = (const char*)GlobalLock(h);
    if(!text){ CloseClipboard(); return {}; }
    std::string result(text);
    GlobalUnlock(h);
    CloseClipboard();
    return result;
}

static void write_clipboard(const std::string& text){
    if(!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if(hg){
        char* dst = (char*)GlobalLock(hg);
        memcpy(dst, text.c_str(), text.size() + 1);
        GlobalUnlock(hg);
        SetClipboardData(CF_TEXT, hg);
    }
    CloseClipboard();
}

static std::string get_payload_path(const std::string& rel){
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string exe_dir(buf, len);
    size_t pos = exe_dir.find_last_of("\\/");
    if(pos != std::string::npos) exe_dir.resize(pos);
    std::string path = rel;
    for(auto& c : path) if(c == '/') c = '\\';
    if(!path.empty() && path[0] == '\\') path = path.substr(1);
    std::string full = exe_dir + "\\" + path;
    if(GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES)
        return full;
    return path;
}

static std::string read_payload_line(const std::string& file){
    std::string path = get_payload_path(file);
    std::ifstream f(path);
    if(!f.is_open()) return {};
    std::vector<std::string> lines;
    std::string line;
    while(std::getline(f, line)){
        while(!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if(!line.empty())
            lines.push_back(line);
    }
    if(lines.empty()) return {};
    std::lock_guard<std::mutex> lock(payload_mutex);
    int& idx = payload_indices[file];
    if(idx >= (int)lines.size()) idx = 0;
    std::string result = lines[idx];
    idx++;
    return result;
}

struct monitor_rect {
    HMONITOR hmon;
    RECT work;
};

static BOOL CALLBACK enum_monitors_proc(HMONITOR hmon, HDC, LPRECT, LPARAM lp){
    auto* monitors = (std::vector<monitor_rect>*)lp;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hmon, &mi);
    monitors->push_back({hmon, mi.rcWork});
    return TRUE;
}

static void action_move_to_next_monitor(){
    HWND fg = GetForegroundWindow();
    if(!fg) return;
    RECT wr;
    GetWindowRect(fg, &wr);
    HMONITOR cur = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    std::vector<monitor_rect> monitors;
    EnumDisplayMonitors(nullptr, nullptr, enum_monitors_proc, (LPARAM)&monitors);
    if(monitors.size() < 2) return;
    int cur_idx = 0;
    for(int i = 0; i < (int)monitors.size(); i++){
        if(monitors[i].hmon == cur){ cur_idx = i; break; }
    }
    int next_idx = (cur_idx + 1) % (int)monitors.size();
    RECT& from = monitors[cur_idx].work;
    RECT& to = monitors[next_idx].work;
    int rel_x = wr.left - from.left;
    int rel_y = wr.top - from.top;
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;
    SetWindowPos(fg, nullptr, to.left + rel_x, to.top + rel_y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void action_next_window(){
    HWND fg = GetForegroundWindow();
    HWND next = nullptr;
    HWND h = fg;
    while((h = GetWindow(h, GW_HWNDNEXT)) != nullptr){
        if(h == fg) break;
        if(!IsWindowVisible(h)) continue;
        if(GetWindowTextLengthA(h) == 0) continue;
        LONG ex = GetWindowLong(h, GWL_EXSTYLE);
        if(ex & WS_EX_TOOLWINDOW) continue;
        LONG style = GetWindowLong(h, GWL_STYLE);
        if(style & WS_CHILD) continue;
        next = h;
        break;
    }
    if(!next){
        h = GetTopWindow(nullptr);
        while(h && h != fg){
            if(IsWindowVisible(h) && GetWindowTextLengthA(h) > 0){
                LONG ex = GetWindowLong(h, GWL_EXSTYLE);
                LONG style = GetWindowLong(h, GWL_STYLE);
                if(!(ex & WS_EX_TOOLWINDOW) && !(style & WS_CHILD)){
                    next = h;
                    break;
                }
            }
            h = GetWindow(h, GW_HWNDNEXT);
        }
    }
    if(!next) return;
    DWORD fg_tid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD tgt_tid = GetWindowThreadProcessId(next, nullptr);
    if(fg_tid != tgt_tid) AttachThreadInput(fg_tid, tgt_tid, TRUE);
    SetForegroundWindow(next);
    if(fg_tid != tgt_tid) AttachThreadInput(fg_tid, tgt_tid, FALSE);
}

static void action_move_window_left(){
    HWND fg = GetForegroundWindow();
    if(!fg) return;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);
    RECT& r = mi.rcWork;
    int w = (r.right - r.left) / 2;
    int h = r.bottom - r.top;
    ShowWindow(fg, SW_RESTORE);
    SetWindowPos(fg, nullptr, r.left, r.top, w, h, SWP_NOZORDER);
}

static void action_move_window_right(){
    HWND fg = GetForegroundWindow();
    if(!fg) return;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(mon, &mi);
    RECT& r = mi.rcWork;
    int w = (r.right - r.left) / 2;
    int h = r.bottom - r.top;
    ShowWindow(fg, SW_RESTORE);
    SetWindowPos(fg, nullptr, r.left + w, r.top, w, h, SWP_NOZORDER);
}

static void action_maximize(){
    HWND fg = GetForegroundWindow();
    if(fg) ShowWindow(fg, SW_MAXIMIZE);
}

static void action_minimize(){
    HWND fg = GetForegroundWindow();
    if(fg) ShowWindow(fg, SW_MINIMIZE);
}

static void execute_kb_action(const std::string& action){
    if(action == "move_to_next_monitor") action_move_to_next_monitor();
    else if(action == "next_window") action_next_window();
    else if(action == "move_window_left") action_move_window_left();
    else if(action == "move_window_right") action_move_window_right();
    else if(action == "maximize_window") action_maximize();
    else if(action == "minimize_window") action_minimize();
}

static void process_combo(){
    if(ui_wants_keyboard.load()){
        max_pressed.clear();
        return;
    }





    
    std::string matched_kb_action;
    std::vector<sloptimus> tool_matches;
    struct payload_hit { std::string file; };
    std::vector<payload_hit> payload_hits;
    bool in_kb_mode = kb_mode.load();

    {
        std::lock_guard<std::mutex> lock(data_mutex);

        if(in_kb_mode){
            if(data.contains("Keyboard_mode") && data["Keyboard_mode"].contains("keykey")){
                for(auto& [action, keys] : data["Keyboard_mode"]["keykey"].items()){
                    if(!keys.is_array() || keys.empty()) continue;
                    if(max_pressed.size() != keys.size()) continue;
                    bool all_match = true;
                    for(const auto& kb : keys){
                        auto it = KEY_NAMES.find(kb.get<std::string>());
                        if(it == KEY_NAMES.end() || !max_pressed.count(it->second)){
                            all_match = false;
                            break;
                        }
                    }
                    if(all_match){
                        matched_kb_action = action;
                        break;
                    }
                }
            }
        } else {
            if(data.contains("Tools")){
                for(auto& [name, value] : data["Tools"].items()){
                    if(!value.contains("keybinds") || !value["keybinds"].is_array()) continue;
                    const auto& keybinds = value["keybinds"];
                    if(keybinds.empty()) continue;
                    if(max_pressed.size() != keybinds.size()) continue;
                    bool all_match = true;
                    for(const auto& kb : keybinds){
                        auto it = KEY_NAMES.find(kb.get<std::string>());
                        if(it == KEY_NAMES.end() || !max_pressed.count(it->second)){
                            all_match = false;
                            break;
                        }
                    }
                    if(all_match){
                        sloptimus m;
                        m.tool = value.value("tool", std::string("?"));
                        m.requires_clipboard = value.value("requires_clipboard", false);
                        if(value.contains("args") && value["args"].is_array()){
                            for(const auto& a : value["args"])
                                m.args.push_back(a.get<std::string>());
                        }
                        tool_matches.push_back(std::move(m));
                    }
                }
            }
        }

        if(data.contains("Payload_mode")){
            for(auto& [name, value] : data["Payload_mode"].items()){
                if(!value.contains("keybinds") || !value["keybinds"].is_array()) continue;
                if(!value.contains("file")) continue;
                const auto& keybinds = value["keybinds"];
                if(keybinds.empty()) continue;
                if(max_pressed.size() != keybinds.size()) continue;
                bool all_match = true;
                for(const auto& kb : keybinds){
                    auto it = KEY_NAMES.find(kb.get<std::string>());
                    if(it == KEY_NAMES.end() || !max_pressed.count(it->second)){
                        all_match = false;
                        break;
                    }
                }
                if(all_match)
                    payload_hits.push_back({value["file"].get<std::string>()});
            }
        }
    }





    if(!matched_kb_action.empty())
        execute_kb_action(matched_kb_action);

    for(auto& m : tool_matches){
        std::string target;
        if(m.requires_clipboard){
            target = read_clipboard();
            while(!target.empty() && (target.back() == '\n' || target.back() == '\r'))
                target.pop_back();
        }
        std::vector<std::string> sub_args;
        bool used_target = false;
        for(auto s : m.args){
            for(size_t pos; (pos = s.find("{TARGET}")) != std::string::npos; ){
                s.replace(pos, 8, target);
                used_target = true;
            }
            sub_args.push_back(std::move(s));
        }
        if(!target.empty() && !used_target)
            sub_args.push_back(target);
        launch_process(m.tool, sub_args);
    }

    for(auto& p : payload_hits){
        std::string line = read_payload_line(p.file);
        if(!line.empty())
            write_clipboard(line);
    }

    max_pressed.clear();
}

static LRESULT CALLBACK keyboard_proc(int code, WPARAM wp, LPARAM lp){
    if(code == HC_ACTION){
        auto* kb = (KBDLLHOOKSTRUCT*)lp;
        uint32_t vk = (uint32_t)kb->vkCode;
        if(wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN){
            pressed_keys.insert(vk);
            max_pressed.insert(vk);
            if(vk == VK_LCONTROL){
                ctrl_solo = true;
            } else {
                ctrl_solo = false;
                ctrl_tap_count = 0;
            }
        } else if(wp == WM_KEYUP || wp == WM_SYSKEYUP){
            pressed_keys.erase(vk);
            if(vk == VK_LCONTROL && ctrl_solo){
                ULONGLONG now = GetTickCount64();
                if(now - last_ctrl_release > CTRL_TAP_WINDOW)
                    ctrl_tap_count = 0;
                ctrl_tap_count++;
                last_ctrl_release = now;
                if(ctrl_tap_count >= 3){
                    ctrl_tap_count = 0;
                    PostThreadMessage(input_thread_id, WM_APP + 2, 0, 0);
                }
            }
        }
        if(pressed_keys.empty() && !max_pressed.empty()){
            PostThreadMessage(input_thread_id, WM_APP + 1, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}



static void thread_main(){
    input_thread_id = GetCurrentThreadId();
    HMODULE hmod = GetModuleHandle(nullptr);
    microslopt = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, hmod, 0);

    {
        std::lock_guard<std::mutex> lock(init_mutex);
        init_done = true;
        init_ok = (microslopt != nullptr);
    }
    init_cv.notify_one();

    if(!microslopt) return;

    MSG msg;
    while(GetMessage(&msg, nullptr, 0, 0) > 0){
        if(msg.message == WM_APP + 1)
            process_combo();
        else if(msg.message == WM_APP + 2)
            kb_mode.store(!kb_mode.load());
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(microslopt);
    microslopt = nullptr;
}





bool start_input_thread(){
    stop_flag.store(false);
    init_done = false;
    init_ok = false;
    input_thread = std::thread(thread_main);
    std::unique_lock<std::mutex> lock(init_mutex);
    init_cv.wait(lock, []{ return init_done; });
    return init_ok;
}

void stop_input_thread(){
    stop_flag.store(true);
    if(input_thread_id) PostThreadMessage(input_thread_id, WM_QUIT, 0, 0);
    if(input_thread.joinable()) input_thread.join();
}

void set_ui_wants_keyboard(bool wants){
    ui_wants_keyboard.store(wants);
}



bool is_keyboard_mode_active(){
    return kb_mode.load();
}

int get_payload_index(const std::string& file){
    std::lock_guard<std::mutex> lock(payload_mutex);
    auto it = payload_indices.find(file);
    if(it == payload_indices.end()) return 0;
    return it->second;
}
