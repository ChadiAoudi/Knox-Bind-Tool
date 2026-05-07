#include "input.h"
#include "config.h"
#include "terminal.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <linux/input.h>
#include <libudev.h>
#include <poll.h>
#include <set>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <unordered_map>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdlib>

extern std::string reclipboard();

static int open_res(const char *path, int flags, void *userdata){
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}
static void close_res(int fd, void *userdata){
    close(fd);
}

const struct libinput_interface interface = {
    .open_restricted = open_res,
    .close_restricted = close_res,
};

static std::thread input_thread;
static std::atomic<bool> stop_flag{false};
static std::atomic<bool> ui_wants_keyboard{false};
static std::atomic<bool> kb_mode{false};

static int ctrl_tap_count = 0;
static std::chrono::steady_clock::time_point last_ctrl_release;
static bool ctrl_solo = true;
static const int64_t CTRL_TAP_WINDOW_MS = 800;

static std::unordered_map<std::string, int> payload_indices;
static std::mutex payload_mutex;

struct combo_match {
    std::string tool;
    bool requires_clipboard;
    std::vector<std::string> args;
};

static void write_clipboard(const std::string& text){
    const char* w = getenv("WAYLAND_DISPLAY");
    const char* d = getenv("DISPLAY");
    const char* cmd = w ? "wl-copy" : d ? "xclip -selection clipboard" : nullptr;
    if(!cmd) return;
    FILE* p = popen(cmd, "w");
    if(!p) return;
    fwrite(text.c_str(), 1, text.size(), p);
    pclose(p);
}

static std::string get_payload_path(const std::string& rel){
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if(len > 0){
        buf[len] = '\0';
        std::string exe_dir(buf, len);
        size_t pos = exe_dir.find_last_of('/');
        if(pos != std::string::npos) exe_dir.resize(pos);
        std::string path = rel;
        if(!path.empty() && path[0] == '/') path = path.substr(1);
        std::string full = exe_dir + "/" + path;
        if(access(full.c_str(), F_OK) == 0) return full;
    }
    std::string path = rel;
    if(!path.empty() && path[0] == '/') path = path.substr(1);
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

static std::string detect_wm(){
    if(getenv("HYPRLAND_INSTANCE_SIGNATURE")) return "hyprland";
    if(getenv("SWAYSOCK")) return "sway";
    if(getenv("DISPLAY")) return "x11";
    return "unknown";
}

struct x11_monitor {
    int x, y, w, h;
};

static std::vector<x11_monitor> get_x11_monitors(){
    std::vector<x11_monitor> monitors;
    FILE* f = popen("xrandr --query 2>/dev/null", "r");
    if(!f) return monitors;
    char buf[512];
    while(fgets(buf, sizeof(buf), f)){
        if(!strstr(buf, " connected")) continue;
        const char* p = buf;
        int mw, mh, mx, my;
        while(*p){
            if(sscanf(p, "%dx%d+%d+%d", &mw, &mh, &mx, &my) == 4){
                monitors.push_back({mx, my, mw, mh});
                break;
            }
            p++;
        }
    }
    pclose(f);
    return monitors;
}

struct x11_win_geom {
    long id;
    int x, y, w, h;
};

static x11_win_geom get_x11_active_window(){
    x11_win_geom g = {};
    FILE* f = popen("xdotool getactivewindow 2>/dev/null", "r");
    if(!f) return g;
    char buf[64];
    if(fgets(buf, sizeof(buf), f))
        g.id = strtol(buf, nullptr, 10);
    pclose(f);
    if(g.id == 0) return g;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xdotool getwindowgeometry --shell %ld 2>/dev/null", g.id);
    f = popen(cmd, "r");
    if(!f) return g;
    while(fgets(buf, sizeof(buf), f)){
        if(strncmp(buf, "X=", 2) == 0) g.x = atoi(buf + 2);
        else if(strncmp(buf, "Y=", 2) == 0) g.y = atoi(buf + 2);
        else if(strncmp(buf, "WIDTH=", 6) == 0) g.w = atoi(buf + 6);
        else if(strncmp(buf, "HEIGHT=", 7) == 0) g.h = atoi(buf + 7);
    }
    pclose(f);
    return g;
}

static void action_move_to_next_monitor(){
    std::string wm = detect_wm();
    if(wm == "hyprland"){
        system("hyprctl dispatch movewindow mon:+1 2>/dev/null");
    } else if(wm == "sway"){
        system("swaymsg move container to output right 2>/dev/null");
    } else if(wm == "x11"){
        auto win = get_x11_active_window();
        if(win.id == 0) return;
        auto monitors = get_x11_monitors();
        if(monitors.size() < 2) return;
        int cx = win.x + win.w / 2;
        int cy = win.y + win.h / 2;
        int cur_idx = 0;
        for(int i = 0; i < (int)monitors.size(); i++){
            if(cx >= monitors[i].x && cx < monitors[i].x + monitors[i].w &&
               cy >= monitors[i].y && cy < monitors[i].y + monitors[i].h){
                cur_idx = i;
                break;
            }
        }
        int next_idx = (cur_idx + 1) % (int)monitors.size();
        int new_x = monitors[next_idx].x + (win.x - monitors[cur_idx].x);
        int new_y = monitors[next_idx].y + (win.y - monitors[cur_idx].y);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "xdotool windowmove %ld %d %d 2>/dev/null", win.id, new_x, new_y);
        system(cmd);
    }
}

static void action_next_window(){
    std::string wm = detect_wm();
    if(wm == "hyprland"){
        system("hyprctl dispatch cyclenext 2>/dev/null");
    } else if(wm == "sway"){
        system("swaymsg focus next 2>/dev/null");
    } else if(wm == "x11"){
        system("xdotool key --clearmodifiers alt+Tab 2>/dev/null");
    }
}

static void action_move_window_left(){
    std::string wm = detect_wm();
    if(wm == "hyprland"){
        system("hyprctl dispatch movewindow l 2>/dev/null");
    } else if(wm == "sway"){
        system("swaymsg move left 2>/dev/null");
    } else if(wm == "x11"){
        auto win = get_x11_active_window();
        if(win.id == 0) return;
        auto monitors = get_x11_monitors();
        if(monitors.empty()) return;
        int cx = win.x + win.w / 2;
        int cy = win.y + win.h / 2;
        int mi = 0;
        for(int i = 0; i < (int)monitors.size(); i++){
            if(cx >= monitors[i].x && cx < monitors[i].x + monitors[i].w &&
               cy >= monitors[i].y && cy < monitors[i].y + monitors[i].h){
                mi = i;
                break;
            }
        }
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
            "wmctrl -i -r %ld -b remove,maximized_vert,maximized_horz 2>/dev/null; "
            "wmctrl -i -r %ld -e 0,%d,%d,%d,%d 2>/dev/null",
            win.id, win.id,
            monitors[mi].x, monitors[mi].y,
            monitors[mi].w / 2, monitors[mi].h);
        system(cmd);
    }
}

static void action_move_window_right(){
    std::string wm = detect_wm();
    if(wm == "hyprland"){
        system("hyprctl dispatch movewindow r 2>/dev/null");
    } else if(wm == "sway"){
        system("swaymsg move right 2>/dev/null");
    } else if(wm == "x11"){
        auto win = get_x11_active_window();
        if(win.id == 0) return;
        auto monitors = get_x11_monitors();
        if(monitors.empty()) return;
        int cx = win.x + win.w / 2;
        int cy = win.y + win.h / 2;
        int mi = 0;
        for(int i = 0; i < (int)monitors.size(); i++){
            if(cx >= monitors[i].x && cx < monitors[i].x + monitors[i].w &&
               cy >= monitors[i].y && cy < monitors[i].y + monitors[i].h){
                mi = i;
                break;
            }
        }
        int half_w = monitors[mi].w / 2;
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
            "wmctrl -i -r %ld -b remove,maximized_vert,maximized_horz 2>/dev/null; "
            "wmctrl -i -r %ld -e 0,%d,%d,%d,%d 2>/dev/null",
            win.id, win.id,
            monitors[mi].x + half_w, monitors[mi].y,
            half_w, monitors[mi].h);
        system(cmd);
    }
}

static void action_maximize(){
    std::string wm = detect_wm();
    if(wm == "hyprland"){
        system("hyprctl dispatch fullscreen 1 2>/dev/null");
    } else if(wm == "sway"){
        system("swaymsg fullscreen toggle 2>/dev/null");
    } else if(wm == "x11"){
        system("wmctrl -r :ACTIVE: -b add,maximized_vert,maximized_horz 2>/dev/null");
    }
}

static void action_minimize(){
    std::string wm = detect_wm();
    if(wm == "hyprland"){
        system("hyprctl dispatch movetoworkspacesilent special 2>/dev/null");
    } else if(wm == "sway"){
        system("swaymsg move scratchpad 2>/dev/null");
    } else if(wm == "x11"){
        system("xdotool getactivewindow windowminimize 2>/dev/null");
    }
}

static void execute_kb_action(const std::string& action){
    if(action == "move_to_next_monitor") action_move_to_next_monitor();
    else if(action == "next_window") action_next_window();
    else if(action == "move_window_left") action_move_window_left();
    else if(action == "move_window_right") action_move_window_right();
    else if(action == "maximize_window") action_maximize();
    else if(action == "minimize_window") action_minimize();
}

static void process_combo(std::set<uint32_t>& max_pressed){
    if(ui_wants_keyboard.load()){
        max_pressed.clear();
        return;
    }

    std::string matched_kb_action;
    std::vector<combo_match> tool_matches;
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
                        combo_match m;
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
            target = reclipboard();
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

static void thread_main(){
    struct udev *udev = udev_new();
    struct libinput *li = libinput_udev_create_context(&interface, NULL, udev);
    if(!li){
        fprintf(stderr, "libinput: failed to create context\n");
        return;
    }
    if(libinput_udev_assign_seat(li, "seat0") != 0){
        fprintf(stderr, "libinput: failed to assign seat\n");
        libinput_unref(li);
        udev_unref(udev);
        return;
    }
    libinput_dispatch(li);

    std::set<uint32_t> pressed_keys;
    std::set<uint32_t> max_pressed;
    struct pollfd pfd;
    pfd.fd = libinput_get_fd(li);
    pfd.events = POLLIN;

    while(!stop_flag.load()){
        int ret = poll(&pfd, 1, 100);
        if(ret < 0){
            if(errno == EINTR) continue;
            break;
        }
        if(ret == 0) continue;
        libinput_dispatch(li);
        struct libinput_event *event;
        while((event = libinput_get_event(li))){
            if(libinput_event_get_type(event) == LIBINPUT_EVENT_KEYBOARD_KEY){
                struct libinput_event_keyboard *key_event = libinput_event_get_keyboard_event(event);
                uint32_t key = libinput_event_keyboard_get_key(key_event);
                enum libinput_key_state state = libinput_event_keyboard_get_key_state(key_event);
                if(state == LIBINPUT_KEY_STATE_PRESSED){
                    pressed_keys.insert(key);
                    max_pressed.insert(key);
                    if(key == KEY_LEFTCTRL){
                        ctrl_solo = true;
                    } else {
                        ctrl_solo = false;
                        ctrl_tap_count = 0;
                    }
                } else {
                    pressed_keys.erase(key);
                    if(key == KEY_LEFTCTRL && ctrl_solo){
                        auto now = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ctrl_release).count();
                        if(elapsed > CTRL_TAP_WINDOW_MS)
                            ctrl_tap_count = 0;
                        ctrl_tap_count++;
                        last_ctrl_release = now;
                        if(ctrl_tap_count >= 3){
                            ctrl_tap_count = 0;
                            kb_mode.store(!kb_mode.load());
                        }
                    }
                }
                if(pressed_keys.empty() && !max_pressed.empty())
                    process_combo(max_pressed);
            }
            libinput_event_destroy(event);
        }
    }

    libinput_unref(li);
    udev_unref(udev);
}

bool start_input_thread(){
    stop_flag.store(false);
    input_thread = std::thread(thread_main);
    return true;
}

void stop_input_thread(){
    stop_flag.store(true);
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
