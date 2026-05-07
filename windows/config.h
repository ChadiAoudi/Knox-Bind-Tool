#pragma once
#include <unordered_map>
#include <cstdint>
#include <string>
#include <mutex>
#include <nlohmann/json.hpp>

extern nlohmann::json data;
extern std::mutex data_mutex;
extern const std::unordered_map<std::string, uint32_t> KEY_NAMES;

std::string get_config_path();
bool config_init();
