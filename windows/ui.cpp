#include "ui.h"
#include "KnoxBindTool.h"
#include "config.h"
#include "terminal.h"
#include "input.h"
#include <commctrl.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstring>

#pragma comment(lib, "comctl32.lib")

#define IDC_NAV_CONFIG      1001
#define IDC_NAV_TERMINAL    1002
#define IDC_CONFIG_LABEL    1003
#define IDC_CONFIG_SAVE     1004
#define IDC_CONFIG_RELOAD   1005
#define IDC_CONFIG_STATUS   1006
#define IDC_CONFIG_EDIT     1007
#define IDC_TERM_TABS       1008
#define IDC_TERM_OUTPUT     1009
#define IDC_TERM_INPUT      1010
#define IDC_TERM_SEND       1011
#define IDC_TERM_KILL       1012
#define IDC_TERM_EMPTY      1013
#define IDC_RIGHT_TEXT      1014

static const int TIMER_REFRESH = 1;
static const int REFRESH_MS = 100;
static const wchar_t* WND_CLASS = L"KnoxBindToolMain";

static HWND hwnd_main = nullptr;
static HWND hwnd_nav_config = nullptr;
static HWND hwnd_nav_terminal = nullptr;
static HWND hwnd_config_label = nullptr;
static HWND hwnd_config_save = nullptr;
static HWND hwnd_config_reload = nullptr;
static HWND hwnd_config_status = nullptr;
static HWND hwnd_config_edit = nullptr;
static HWND hwnd_term_tabs = nullptr;
static HWND hwnd_term_output = nullptr;
static HWND hwnd_term_input = nullptr;
static HWND hwnd_term_send = nullptr;
static HWND hwnd_term_kill = nullptr;
static HWND hwnd_term_empty = nullptr;
static HWND hwnd_right_text = nullptr;

static HINSTANCE app_instance = nullptr;
static HICON logo_icon = nullptr;
static HBRUSH left_panel_brush = nullptr;
static WNDPROC orig_input_proc = nullptr;
static HFONT mono_font = nullptr;

static std::string selected_nav = "config";
static int selected_process_id = 0;
static std::string save_status;
static bool should_close = false;
static int last_tab_count = 0;
static size_t last_output_len = 0;

static void load_config_into_editor();
static void save_config();
static void send_terminal_input();
static void layout_controls(int cx, int cy);
static void update_visibility();
static void update_right_panel();
static void update_terminal_tabs();
static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK input_subclass(HWND, UINT, WPARAM, LPARAM);

static void load_config_into_editor(){
    std::ifstream in(get_config_path());
    if(!in){
        save_status = "couldn't open config";
        SetWindowTextA(hwnd_config_status, save_status.c_str());
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    std::string display;
    for(char c : s){
        if(c == '\n') display += "\r\n";
        else if(c != '\r') display += c;
    }
    SetWindowTextA(hwnd_config_edit, display.c_str());
    save_status = "loaded";
    SetWindowTextA(hwnd_config_status, save_status.c_str());
}

static void save_config(){
    int len = GetWindowTextLengthA(hwnd_config_edit);
    std::string buf(len + 1, '\0');
    GetWindowTextA(hwnd_config_edit, buf.data(), len + 1);
    buf.resize(len);

    std::string clean;
    for(char c : buf){
        if(c != '\r') clean += c;
    }

    try {
        auto reparsed = nlohmann::json::parse(clean);
        std::ofstream out(get_config_path());
        if(!out){
            save_status = "couldn't write config";
            SetWindowTextA(hwnd_config_status, save_status.c_str());
            return;
        }
        out << clean;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            data = reparsed;
        }
        save_status = "saved";
    } catch(const std::exception& e){
        save_status = std::string("parse error: ") + e.what();
    }
    SetWindowTextA(hwnd_config_status, save_status.c_str());
}

static void send_terminal_input(){
    char buf[1024] = {};
    GetWindowTextA(hwnd_term_input, buf, sizeof(buf));
    if(buf[0] == '\0') return;
    std::string s = std::string(buf) + "\n";
    send_to_process(selected_process_id, s);
    SetWindowTextA(hwnd_term_input, "");
    SetFocus(hwnd_term_input);
}

static void update_visibility(){
    bool config_mode = (selected_nav == "config");
    int cs = config_mode ? SW_SHOW : SW_HIDE;
    int ts = config_mode ? SW_HIDE : SW_SHOW;

    ShowWindow(hwnd_config_label, cs);
    ShowWindow(hwnd_config_save, cs);
    ShowWindow(hwnd_config_reload, cs);
    ShowWindow(hwnd_config_status, cs);
    ShowWindow(hwnd_config_edit, cs);

    ShowWindow(hwnd_term_tabs, ts);
    ShowWindow(hwnd_term_output, ts);
    ShowWindow(hwnd_term_input, ts);
    ShowWindow(hwnd_term_send, ts);
    ShowWindow(hwnd_term_kill, ts);
    ShowWindow(hwnd_term_empty, ts);

    last_output_len = 0;
}

static void update_right_panel(){
    std::string text;
    if(selected_nav == "config"){
        text = "Keyboard Mode: ";
        text += is_keyboard_mode_active() ? "ON" : "OFF";
        text += "\r\n\r\n";
        text += "Tools\r\n-----\r\n";
        std::lock_guard<std::mutex> lock(data_mutex);
        if(data.contains("Tools")){
            for(auto& [name, value] : data["Tools"].items()){
                std::string tool = value.value("tool", std::string("?"));
                text += name + "  -  " + tool + "\r\n";
                if(value.contains("keybinds")){
                    for(auto& kb : value["keybinds"])
                        text += "  * " + kb.get<std::string>() + "\r\n";
                }
                bool clip = value.value("requires_clipboard", false);
                text += "  clipboard: ";
                text += clip ? "yes" : "no";
                text += "\r\n\r\n";
            }
        }
        text += "Keyboard Mode Actions\r\n---------------------\r\n";
        if(data.contains("Keyboard_mode") && data["Keyboard_mode"].contains("keykey")){
            for(auto& [action, keys] : data["Keyboard_mode"]["keykey"].items()){
                text += "  " + action + ": ";
                for(size_t i = 0; i < keys.size(); i++){
                    if(i > 0) text += " + ";
                    text += keys[i].get<std::string>();
                }
                text += "\r\n";
            }
        }
        text += "\r\nPayload Mode\r\n------------\r\n";
        if(data.contains("Payload_mode")){
            for(auto& [name, value] : data["Payload_mode"].items()){
                std::string file = value.value("file", std::string("?"));
                text += name + "  -  " + file;
                int idx = get_payload_index(file);
                text += " (line " + std::to_string(idx) + ")\r\n";
                if(value.contains("keybinds")){
                    for(auto& kb : value["keybinds"])
                        text += "  * " + kb.get<std::string>() + "\r\n";
                }
                text += "\r\n";
            }
        }
    } else {
        text = "Processes\r\n---------\r\n";
        auto procs = list_processes();
        if(procs.empty()){
            text += "none\r\n";
        } else {
            int running = 0;
            for(auto& p : procs) if(p.running) running++;
            text += "Running: " + std::to_string(running);
            text += " / Total: " + std::to_string((int)procs.size()) + "\r\n";
        }
        text += "\r\nKeyboard Mode: ";
        text += is_keyboard_mode_active() ? "ON" : "OFF";
        text += "\r\n";
    }
    SetWindowTextA(hwnd_right_text, text.c_str());
}

static void update_terminal_tabs(){
    auto procs = list_processes();
    int count = (int)procs.size();

    if(count != last_tab_count){
        TabCtrl_DeleteAllItems(hwnd_term_tabs);
        for(int i = 0; i < count; i++){
            std::string label = procs[i].tab_name;
            if(!procs[i].running)
                label += " (exit " + std::to_string(procs[i].exit_code) + ")";
            TCITEMA item = {};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<char*>(label.c_str());
            SendMessageA(hwnd_term_tabs, TCM_INSERTITEMA, i, (LPARAM)&item);
        }
        last_tab_count = count;
        if(count > 0 && selected_process_id == 0)
            selected_process_id = procs[0].id;
    }

    if(count == 0){
        ShowWindow(hwnd_term_empty, SW_SHOW);
        ShowWindow(hwnd_term_output, SW_HIDE);
        ShowWindow(hwnd_term_input, SW_HIDE);
        ShowWindow(hwnd_term_send, SW_HIDE);
        ShowWindow(hwnd_term_kill, SW_HIDE);
    } else if(selected_nav == "terminal"){
        ShowWindow(hwnd_term_empty, SW_HIDE);
        ShowWindow(hwnd_term_output, SW_SHOW);
        ShowWindow(hwnd_term_kill, SW_SHOW);

        bool proc_running = false;
        for(auto& p : procs)
            if(p.id == selected_process_id) proc_running = p.running;

        ShowWindow(hwnd_term_input, proc_running ? SW_SHOW : SW_HIDE);
        ShowWindow(hwnd_term_send, proc_running ? SW_SHOW : SW_HIDE);
    }
}

static void refresh_terminal_output(){
    if(selected_nav != "terminal" || selected_process_id == 0) return;
    std::string output = get_process_output(selected_process_id);
    if(output.size() == last_output_len) return;
    last_output_len = output.size();

    std::string display;
    display.reserve(output.size());
    for(char c : output){
        if(c == '\n') display += "\r\n";
        else if(c != '\r') display += c;
    }

    SendMessage(hwnd_term_output, WM_SETREDRAW, FALSE, 0);
    SetWindowTextA(hwnd_term_output, display.c_str());
    int line_count = (int)SendMessage(hwnd_term_output, EM_GETLINECOUNT, 0, 0);
    SendMessage(hwnd_term_output, EM_LINESCROLL, 0, line_count);
    SendMessage(hwnd_term_output, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwnd_term_output, nullptr, FALSE);
}

static void layout_controls(int cx, int cy){
    const int left_w = 180;
    const int right_w = 320;
    const int gap = 8;
    int middle_x = left_w + gap;
    int middle_w = cx - left_w - right_w - 2 * gap;
    if(middle_w < 120) middle_w = 120;
    int right_x = middle_x + middle_w + gap;

    MoveWindow(hwnd_nav_config, 10, 80, left_w - 20, 30, TRUE);
    MoveWindow(hwnd_nav_terminal, 10, 118, left_w - 20, 30, TRUE);

    int y = 10;
    MoveWindow(hwnd_config_label, middle_x, y, 200, 20, TRUE);
    y += 25;
    MoveWindow(hwnd_config_save, middle_x, y, 60, 25, TRUE);
    MoveWindow(hwnd_config_reload, middle_x + 65, y, 60, 25, TRUE);
    MoveWindow(hwnd_config_status, middle_x + 135, y, middle_w - 135, 25, TRUE);
    y += 30;
    MoveWindow(hwnd_config_edit, middle_x, y, middle_w, cy - y - 10, TRUE);

    MoveWindow(hwnd_term_tabs, middle_x, 10, middle_w, 28, TRUE);
    int term_top = 42;
    int input_h = 25;
    int output_h = cy - term_top - input_h - 20;
    if(output_h < 50) output_h = 50;
    MoveWindow(hwnd_term_kill, middle_x + middle_w - 60, term_top, 60, 25, TRUE);
    MoveWindow(hwnd_term_output, middle_x, term_top + 30, middle_w, output_h, TRUE);
    MoveWindow(hwnd_term_empty, middle_x, term_top + 30, middle_w, output_h, TRUE);
    int input_y = term_top + 30 + output_h + 5;
    MoveWindow(hwnd_term_input, middle_x, input_y, middle_w - 65, input_h, TRUE);
    MoveWindow(hwnd_term_send, middle_x + middle_w - 60, input_y, 60, input_h, TRUE);

    MoveWindow(hwnd_right_text, right_x, 10, cx - right_x - 10, cy - 20, TRUE);
}

static LRESULT CALLBACK input_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    if(msg == WM_KEYDOWN && wp == VK_RETURN){
        send_terminal_input();
        return 0;
    }
    return CallWindowProc(orig_input_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK wnd_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_CREATE: {
        hwnd_nav_config = CreateWindowA("BUTTON", "Config",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)IDC_NAV_CONFIG, app_instance, nullptr);
        hwnd_nav_terminal = CreateWindowA("BUTTON", "Terminal",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)IDC_NAV_TERMINAL, app_instance, nullptr);

        hwnd_config_label = CreateWindowA("STATIC", "config/knox.json",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hWnd, (HMENU)IDC_CONFIG_LABEL, app_instance, nullptr);
        hwnd_config_save = CreateWindowA("BUTTON", "Save",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)IDC_CONFIG_SAVE, app_instance, nullptr);
        hwnd_config_reload = CreateWindowA("BUTTON", "Reload",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)IDC_CONFIG_RELOAD, app_instance, nullptr);
        hwnd_config_status = CreateWindowA("STATIC", "",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hWnd, (HMENU)IDC_CONFIG_STATUS, app_instance, nullptr);
        hwnd_config_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hWnd, (HMENU)IDC_CONFIG_EDIT, app_instance, nullptr);

        hwnd_term_tabs = CreateWindowA(WC_TABCONTROLA, "",
            WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hWnd, (HMENU)IDC_TERM_TABS, app_instance, nullptr);
        hwnd_term_output = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
            0, 0, 0, 0, hWnd, (HMENU)IDC_TERM_OUTPUT, app_instance, nullptr);
        hwnd_term_input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | ES_AUTOHSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_TERM_INPUT, app_instance, nullptr);
        hwnd_term_send = CreateWindowA("BUTTON", "Send",
            WS_CHILD | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)IDC_TERM_SEND, app_instance, nullptr);
        hwnd_term_kill = CreateWindowA("BUTTON", "Kill",
            WS_CHILD | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)IDC_TERM_KILL, app_instance, nullptr);
        hwnd_term_empty = CreateWindowA("STATIC",
            "No processes yet. Fire a hotkey to launch one.",
            WS_CHILD,
            0, 0, 0, 0, hWnd, (HMENU)IDC_TERM_EMPTY, app_instance, nullptr);

        hwnd_right_text = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hWnd, (HMENU)IDC_RIGHT_TEXT, app_instance, nullptr);

        if(mono_font){
            SendMessage(hwnd_config_edit, WM_SETFONT, (WPARAM)mono_font, TRUE);
            SendMessage(hwnd_term_output, WM_SETFONT, (WPARAM)mono_font, TRUE);
            SendMessage(hwnd_term_input, WM_SETFONT, (WPARAM)mono_font, TRUE);
            SendMessage(hwnd_right_text, WM_SETFONT, (WPARAM)mono_font, TRUE);
        }

        orig_input_proc = (WNDPROC)SetWindowLongPtr(hwnd_term_input,
            GWLP_WNDPROC, (LONG_PTR)input_subclass);

        SetTimer(hWnd, TIMER_REFRESH, REFRESH_MS, nullptr);
        update_visibility();
        load_config_into_editor();
        update_right_panel();
        return 0;
    }
    case WM_SIZE: {
        int cx = LOWORD(lp);
        int cy = HIWORD(lp);
        if(cx > 0 && cy > 0) layout_controls(cx, cy);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        RECT left_rc = {0, 0, 180, rc.bottom};
        FillRect(hdc, &left_rc, left_panel_brush);
        if(logo_icon)
            DrawIconEx(hdc, 58, 16, logo_icon, 48, 48, 0, nullptr, DI_NORMAL);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        switch(LOWORD(wp)){
        case IDC_NAV_CONFIG:
            selected_nav = "config";
            update_visibility();
            update_right_panel();
            last_output_len = 0;
            break;
        case IDC_NAV_TERMINAL:
            selected_nav = "terminal";
            update_visibility();
            update_right_panel();
            update_terminal_tabs();
            last_output_len = 0;
            break;
        case IDC_CONFIG_SAVE:
            save_config();
            update_right_panel();
            break;
        case IDC_CONFIG_RELOAD:
            load_config_into_editor();
            update_right_panel();
            break;
        case IDC_TERM_SEND:
            send_terminal_input();
            break;
        case IDC_TERM_KILL:
            if(selected_process_id > 0)
                kill_process(selected_process_id);
            break;
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if(nm->idFrom == IDC_TERM_TABS && nm->code == TCN_SELCHANGE){
            int sel = TabCtrl_GetCurSel(hwnd_term_tabs);
            auto procs = list_processes();
            if(sel >= 0 && sel < (int)procs.size()){
                selected_process_id = procs[sel].id;
                last_output_len = 0;
                refresh_terminal_output();
            }
        }
        return 0;
    }
    case WM_TIMER:
        if(wp == TIMER_REFRESH){
            if(selected_nav == "terminal"){
                update_terminal_tabs();
                refresh_terminal_output();
            }
            update_right_panel();
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hWnd, TIMER_REFRESH);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

bool ui_init(HINSTANCE instance, int show_cmd){
    app_instance = instance;

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    left_panel_brush = CreateSolidBrush(RGB(50, 50, 55));
    mono_font = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

    logo_icon = LoadIcon(instance, MAKEINTRESOURCE(IDI_KNOXBINDTOOL));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = logo_icon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WND_CLASS;
    wc.hIconSm = LoadIcon(instance, MAKEINTRESOURCE(IDI_SMALL));
    if(!RegisterClassExW(&wc)) return false;

    hwnd_main = CreateWindowExW(0, WND_CLASS, L"Knox Tools",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, nullptr, instance, nullptr);
    if(!hwnd_main) return false;

    ShowWindow(hwnd_main, show_cmd);
    UpdateWindow(hwnd_main);
    return true;
}

bool ui_should_close(){
    return should_close;
}

void ui_render_frame(){
    MSG msg;
    if(GetMessage(&msg, nullptr, 0, 0) > 0){
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    } else {
        should_close = true;
    }
}

void ui_shutdown(){
    if(mono_font){ DeleteObject(mono_font); mono_font = nullptr; }
    if(left_panel_brush){ DeleteObject(left_panel_brush); left_panel_brush = nullptr; }
    if(hwnd_main){ DestroyWindow(hwnd_main); hwnd_main = nullptr; }
    UnregisterClassW(WND_CLASS, app_instance);
}

bool ui_has_edit_focus(){
    HWND focus = GetFocus();
    return focus == hwnd_config_edit || focus == hwnd_term_input;
}
