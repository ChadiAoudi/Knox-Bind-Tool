#pragma once
#include <libinput.h>
#include <string>

extern const struct libinput_interface interface;

bool start_input_thread();
void stop_input_thread();
void set_ui_wants_keyboard(bool wants);
bool is_keyboard_mode_active();
int get_payload_index(const std::string& file);
