#include "config.h"
#include <fstream>
#include <linux/input.h>
#include <unistd.h>
#include <climits>

nlohmann::json data;
std::mutex data_mutex;

std::string get_config_path(){
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if(len > 0){
        buf[len] = '\0';
        std::string exe_dir(buf, len);
        size_t pos = exe_dir.find_last_of('/');
        if(pos != std::string::npos) exe_dir.resize(pos);
        std::string path = exe_dir + "/config/knox.json";
        if(access(path.c_str(), F_OK) == 0)
            return path;
    }
    return "config/knox.json";
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
    {"KEY_A", KEY_A}, {"KEY_B", KEY_B}, {"KEY_C", KEY_C}, {"KEY_D", KEY_D},
    {"KEY_E", KEY_E}, {"KEY_F", KEY_F}, {"KEY_G", KEY_G}, {"KEY_H", KEY_H},
    {"KEY_I", KEY_I}, {"KEY_J", KEY_J}, {"KEY_K", KEY_K}, {"KEY_L", KEY_L},
    {"KEY_M", KEY_M}, {"KEY_N", KEY_N}, {"KEY_O", KEY_O}, {"KEY_P", KEY_P},
    {"KEY_Q", KEY_Q}, {"KEY_R", KEY_R}, {"KEY_S", KEY_S}, {"KEY_T", KEY_T},
    {"KEY_U", KEY_U}, {"KEY_V", KEY_V}, {"KEY_W", KEY_W}, {"KEY_X", KEY_X},
    {"KEY_Y", KEY_Y}, {"KEY_Z", KEY_Z},
    {"KEY_0", KEY_0}, {"KEY_1", KEY_1}, {"KEY_2", KEY_2}, {"KEY_3", KEY_3},
    {"KEY_4", KEY_4}, {"KEY_5", KEY_5}, {"KEY_6", KEY_6}, {"KEY_7", KEY_7},
    {"KEY_8", KEY_8}, {"KEY_9", KEY_9},
    {"KEY_LEFTCTRL", KEY_LEFTCTRL}, {"KEY_RIGHTCTRL", KEY_RIGHTCTRL},
    {"KEY_LEFTSHIFT", KEY_LEFTSHIFT}, {"KEY_RIGHTSHIFT", KEY_RIGHTSHIFT},
    {"KEY_LEFTALT", KEY_LEFTALT}, {"KEY_RIGHTALT", KEY_RIGHTALT},
    {"KEY_LEFTMETA", KEY_LEFTMETA}, {"KEY_RIGHTMETA", KEY_RIGHTMETA},
    {"KEY_SPACE", KEY_SPACE}, {"KEY_ENTER", KEY_ENTER},
    {"KEY_TAB", KEY_TAB}, {"KEY_ESC", KEY_ESC},
};
