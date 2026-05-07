#include "config.h"
#include "framework.h"
#include <fstream>

nlohmann::json data;
std::mutex data_mutex;

std::string get_config_path(){
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string exe_dir(buf, len);
    size_t pos = exe_dir.find_last_of("\\/");
    if(pos != std::string::npos) exe_dir.resize(pos);
    std::string path = exe_dir + "\\config\\knox.json";
    if(GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return path;
    return "config\\knox.json";
}

bool config_init(){
    std::ifstream f(get_config_path());
    if(!f.is_open()) return false;
    try {
        data = nlohmann::json::parse(f);
    } catch(...){
        return false;
    }
    return true;
}

const std::unordered_map<std::string, uint32_t> KEY_NAMES = {
    {"KEY_A", 'A'}, {"KEY_B", 'B'}, {"KEY_C", 'C'}, {"KEY_D", 'D'},
    {"KEY_E", 'E'}, {"KEY_F", 'F'}, {"KEY_G", 'G'}, {"KEY_H", 'H'},
    {"KEY_I", 'I'}, {"KEY_J", 'J'}, {"KEY_K", 'K'}, {"KEY_L", 'L'},
    {"KEY_M", 'M'}, {"KEY_N", 'N'}, {"KEY_O", 'O'}, {"KEY_P", 'P'},
    {"KEY_Q", 'Q'}, {"KEY_R", 'R'}, {"KEY_S", 'S'}, {"KEY_T", 'T'},
    {"KEY_U", 'U'}, {"KEY_V", 'V'}, {"KEY_W", 'W'}, {"KEY_X", 'X'},
    {"KEY_Y", 'Y'}, {"KEY_Z", 'Z'},
    {"KEY_0", '0'}, {"KEY_1", '1'}, {"KEY_2", '2'}, {"KEY_3", '3'},
    {"KEY_4", '4'}, {"KEY_5", '5'}, {"KEY_6", '6'}, {"KEY_7", '7'},
    {"KEY_8", '8'}, {"KEY_9", '9'},
    {"KEY_LEFTCTRL", VK_LCONTROL}, {"KEY_RIGHTCTRL", VK_RCONTROL},
    {"KEY_LEFTSHIFT", VK_LSHIFT}, {"KEY_RIGHTSHIFT", VK_RSHIFT},
    {"KEY_LEFTALT", VK_LMENU}, {"KEY_RIGHTALT", VK_RMENU},
    {"KEY_LEFTMETA", VK_LWIN}, {"KEY_RIGHTMETA", VK_RWIN},
    {"KEY_SPACE", VK_SPACE}, {"KEY_ENTER", VK_RETURN},
    {"KEY_TAB", VK_TAB}, {"KEY_ESC", VK_ESCAPE},
};
