#include "ui.h"
#include "config.h"
#include "terminal.h"
#include "input.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

static GLFWwindow* window = nullptr;
static std::string selected_nav = "config";
static std::vector<char> json_buffer;
static std::string save_status;
static int selected_process_id = 0;
static char input_buffer[1024] = {0};

static const size_t JSON_BUFFER_SIZE = 65536;

static GLuint logo_tex = 0;
static int logo_w = 0, logo_h = 0;

static std::string find_logo_path(){
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string exe_dir = ".";
    if(len > 0){
        buf[len] = '\0';
        exe_dir = std::string(buf, len);
        size_t pos = exe_dir.find_last_of('/');
        if(pos != std::string::npos) exe_dir.resize(pos);
    }
    std::string paths[] = {
        exe_dir + "/src/logo.png",
        exe_dir + "/../src/logo.png",
    };
    for(auto& p : paths){
        if(access(p.c_str(), F_OK) == 0) return p;
    }
    return "src/logo.png";
}

static void load_logo(){
    std::string logo_path = find_logo_path();
    int channels;
    unsigned char* pixels = stbi_load(logo_path.c_str(), &logo_w, &logo_h, &channels, 4);
    if(!pixels){
        fprintf(stderr, "logo: stbi_load failed: %s\n", stbi_failure_reason());
        return;
    }
    GLFWimage icon{logo_w, logo_h, pixels};
    glfwSetWindowIcon(window, 1, &icon);
    glGenTextures(1, &logo_tex);
    glBindTexture(GL_TEXTURE_2D, logo_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logo_w, logo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
}

static void load_json_into_buffer(){
    json_buffer.assign(JSON_BUFFER_SIZE, 0);
    std::ifstream in(get_config_path());
    if(!in){ save_status = "couldn't open config"; return; }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if(s.size() >= JSON_BUFFER_SIZE){ save_status = "config too large for buffer"; return; }
    std::memcpy(json_buffer.data(), s.data(), s.size());
    save_status = "loaded";
}

static void save_buffer_to_json(){
    try {
        auto reparsed = nlohmann::json::parse(json_buffer.data());
        std::ofstream out(get_config_path());
        if(!out){ save_status = "couldn't write config"; return; }
        out << json_buffer.data();
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            data = reparsed;
        }
        save_status = "saved";
    } catch(const std::exception& e){
        save_status = std::string("parse error: ") + e.what();
    }
}

static void render_left_nav(){
    if(logo_tex){
        float pane_w = ImGui::GetContentRegionAvail().x;
        float img_w = pane_w - 24.0f;
        if(img_w > 140.0f) img_w = 140.0f;
        float img_h = img_w * (float)logo_h / (float)logo_w;
        float pad_x = (pane_w - img_w) * 0.5f;
        if(pad_x > 0) ImGui::Indent(pad_x);
        ImGui::Image((ImTextureID)(intptr_t)logo_tex, ImVec2(img_w, img_h));
        if(pad_x > 0) ImGui::Unindent(pad_x);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }
    if(ImGui::Selectable("Config", selected_nav == "config")) selected_nav = "config";
    if(ImGui::Selectable("Terminal", selected_nav == "terminal")) selected_nav = "terminal";
}

static void render_terminal_middle(){
    auto procs = list_processes();
    if(procs.empty()){
        ImGui::TextWrapped("No processes yet. Fire a hotkey to launch one.");
        return;
    }
    if(ImGui::BeginTabBar("##processes", ImGuiTabBarFlags_FittingPolicyScroll)){
        for(auto& p : procs){
            std::string label = p.tab_name;
            if(!p.running) label += " (exit " + std::to_string(p.exit_code) + ")";
            if(ImGui::BeginTabItem(label.c_str())){
                if(selected_process_id != p.id){
                    selected_process_id = p.id;
                    input_buffer[0] = 0;
                }
                if(p.running){
                    if(ImGui::SmallButton("Kill")) kill_process(p.id);
                    ImGui::SameLine();
                    ImGui::TextDisabled("running");
                } else {
                    ImGui::TextDisabled("exited (%d)", p.exit_code);
                }
                ImGui::Separator();
                float input_row_h = p.running ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
                ImGui::BeginChild("##output", ImVec2(-1, -input_row_h), true,
                    ImGuiWindowFlags_HorizontalScrollbar);
                std::string output = get_process_output(p.id);
                ImGui::TextUnformatted(output.c_str());
                if(ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                    ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                if(p.running){
                    bool send_now = false;
                    ImGui::SetNextItemWidth(-72.0f);
                    if(ImGui::InputText("##input", input_buffer, sizeof(input_buffer),
                        ImGuiInputTextFlags_EnterReturnsTrue)){
                        send_now = true;
                    }
                    ImGui::SameLine();
                    if(ImGui::Button("Send", ImVec2(-1, 0))) send_now = true;
                    if(send_now){
                        std::string s = std::string(input_buffer) + "\n";
                        send_to_process(p.id, s);
                        input_buffer[0] = 0;
                        ImGui::SetKeyboardFocusHere(-1);
                    }
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

static void render_middle_pane(){
    if(selected_nav == "config"){
        ImGui::Text("config/knox.json");
        ImGui::Separator();
        if(ImGui::Button("Save")) save_buffer_to_json();
        ImGui::SameLine();
        if(ImGui::Button("Reload")) load_json_into_buffer();
        if(!save_status.empty()){
            ImGui::SameLine();
            ImGui::TextDisabled("%s", save_status.c_str());
        }
        ImGui::InputTextMultiline("##json_editor",
            json_buffer.data(), json_buffer.size(),
            ImVec2(-1, -1));
    } else if(selected_nav == "terminal"){
        render_terminal_middle();
    }
}

static void render_right_pane(){
    if(selected_nav == "config"){
        ImGui::Text("Keyboard Mode: %s", is_keyboard_mode_active() ? "ON" : "OFF");
        ImGui::Spacing();
        ImGui::Text("Tools");
        ImGui::Separator();
        std::lock_guard<std::mutex> lock(data_mutex);
        if(data.contains("Tools")){
            for(auto &[name, value] : data["Tools"].items()){
                std::string tool = value.value("tool", std::string("?"));
                ImGui::Text("%s  -  %s", name.c_str(), tool.c_str());
                ImGui::Indent();
                if(value.contains("keybinds")){
                    for(auto &kb : value["keybinds"])
                        ImGui::BulletText("%s", kb.get<std::string>().c_str());
                }
                bool clip = value.value("requires_clipboard", false);
                ImGui::TextDisabled("clipboard: %s", clip ? "yes" : "no");
                ImGui::Unindent();
                ImGui::Spacing();
            }
        }
        ImGui::Spacing();
        ImGui::Text("Keyboard Mode Actions");
        ImGui::Separator();
        if(data.contains("Keyboard_mode") && data["Keyboard_mode"].contains("keykey")){
            for(auto& [action, keys] : data["Keyboard_mode"]["keykey"].items()){
                std::string combo;
                for(size_t i = 0; i < keys.size(); i++){
                    if(i > 0) combo += " + ";
                    combo += keys[i].get<std::string>();
                }
                ImGui::Text("  %s: %s", action.c_str(), combo.c_str());
            }
        }
        ImGui::Spacing();
        ImGui::Text("Payload Mode");
        ImGui::Separator();
        if(data.contains("Payload_mode")){
            for(auto& [name, value] : data["Payload_mode"].items()){
                std::string file = value.value("file", std::string("?"));
                int idx = get_payload_index(file);
                ImGui::Text("%s  -  %s (line %d)", name.c_str(), file.c_str(), idx);
                ImGui::Indent();
                if(value.contains("keybinds")){
                    for(auto& kb : value["keybinds"])
                        ImGui::BulletText("%s", kb.get<std::string>().c_str());
                }
                ImGui::Unindent();
                ImGui::Spacing();
            }
        }
    } else if(selected_nav == "terminal"){
        ImGui::Text("Processes");
        ImGui::Separator();
        auto procs = list_processes();
        if(procs.empty()){
            ImGui::TextDisabled("none");
        } else {
            int running_count = 0;
            for(auto& p : procs) if(p.running) running_count++;
            ImGui::Text("Running: %d / Total: %d", running_count, (int)procs.size());
        }
        ImGui::Spacing();
        ImGui::Text("Keyboard Mode: %s", is_keyboard_mode_active() ? "ON" : "OFF");
    }
}

static void render_main_window(){
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##knox_main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    float left_w = 180.0f;
    float right_w = 320.0f;
    float gap = 8.0f;
    float middle_w = ImGui::GetContentRegionAvail().x - left_w - right_w - 2.0f * gap;
    if(middle_w < 120.0f) middle_w = 120.0f;

    ImGui::BeginChild("##nav", ImVec2(left_w, 0), true);
    render_left_nav();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##middle", ImVec2(middle_w, 0), true);
    render_middle_pane();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##right", ImVec2(0, 0), true);
    render_right_pane();
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleVar(2);
}

bool ui_init(){
    if(!glfwInit()){
        fprintf(stderr, "glfw init failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    window = glfwCreateWindow(1280, 800, "Knox Tools", nullptr, nullptr);
    if(!window){
        fprintf(stderr, "glfw window create failed\n");
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    load_logo();
    load_json_into_buffer();
    return true;
}

bool ui_should_close(){
    return glfwWindowShouldClose(window);
}

void ui_render_frame(){
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    render_main_window();

    ImGui::Render();
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
}

void ui_shutdown(){
    if(logo_tex){ glDeleteTextures(1, &logo_tex); logo_tex = 0; }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if(window) glfwDestroyWindow(window);
    glfwTerminate();
}
