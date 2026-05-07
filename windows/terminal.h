#pragma once
#include <string>
#include <vector>

struct process_info {
    int id;
    std::string tab_name;
    bool running;
    int exit_code;
};

void launch_process(const std::string& tool, const std::vector<std::string>& argv);
std::vector<process_info> list_processes();
std::string get_process_output(int id);
void send_to_process(int id, const std::string& data);
void kill_process(int id);
void shutdown_processes();
