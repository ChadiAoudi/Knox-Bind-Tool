#pragma once
#include "framework.h"

bool ui_init(HINSTANCE instance, int show_cmd);
bool ui_should_close();
void ui_render_frame();
void ui_shutdown();
bool ui_has_edit_focus();
