#pragma once
#include <string>

bool start_input_thread();
void stop_input_thread();
void set_ui_wants_keyboard(bool wants);
bool is_keyboard_mode_active();
int get_payload_index(const std::string& file);
