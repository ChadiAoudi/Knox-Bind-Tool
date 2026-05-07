#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <imgui.h>
#include "config.h"
#include "input.h"
#include "ui.h"
#include "terminal.h"


std::string reclipboard(){
    const char* w = getenv("WAYLAND_DISPLAY");
    const char* d = getenv("DISPLAY");
    const char* r = getenv("XDG_RUNTIME_DIR");
    fprintf(stderr, "clippy:  WAYLAND_DISPLAY=%s DISPLAY=%s XDG_RUNTIME_DIR=%s\n",
            w?w:"(null)", d?d:"(null)", r?r:"(null)");
    const char* cmd = w ? "wl-paste --no-newline"
                    : d ? "xclip -selection clipboard -o"
                        : nullptr;
    if(!cmd) { fprintf(stderr, "clippy:  no display env\n"); return {}; }
    std::unique_ptr<FILE, int(*)(FILE*)> p(popen(cmd, "r"), pclose);
    if(!p) { perror("clippy:  popen"); return {}; }
    std::string out;
    std::array<char, 4096> buf;
    while(size_t n = fread(buf.data(), 1, buf.size(), p.get())) out.append(buf.data(), n);
    fprintf(stderr, "clippy:  got %zu bytes\n", out.size());
    return out;
}
void launch_it( const std::string& program, const std::string& target){
    if(target.empty() || target.find_first_of(" \t\n;|&`$<>\\\"'") != std::string::npos){
        fprintf(stderr, "Invalid target: %s\n", target.c_str());
        return;
    }
    const char* term = getenv("TERMINAL");
    if(!term){
        for(auto t:{"foot", "kitty", "alacritty", "xterm", "gnome-terminal"}){
            if(access((std::string("/usr/bin/") + t).c_str(), X_OK) == 0) { term = t; break; }
        }
    }
    if(!term){
        fprintf(stderr, "No terminal found\n");
        return;
    }
    pid_t pid = fork();
    if(pid < 0){
        fprintf(stderr, "Failed to fork: %s\n", strerror(errno));
        return;
    }
    if(pid == 0){
        execl(term, term, "-e", program.c_str(), target.c_str(), (char*)nullptr);
        _exit(127);
    }
}
int main(){
    if(!config_init()){
        fprintf(stderr, "config: failed to load\n");
        return 1;
    }
    if(!start_input_thread()){
        fprintf(stderr, "input: failed to start\n");
        return 1;
    }
    if(!ui_init()){
        fprintf(stderr, "ui: failed to init\n");
        stop_input_thread();
        return 1;
    }
    while(!ui_should_close()){
        set_ui_wants_keyboard(ImGui::GetIO().WantCaptureKeyboard);
        ui_render_frame();
    }
    stop_input_thread();
    shutdown_processes();
    ui_shutdown();
    return 0;
}
