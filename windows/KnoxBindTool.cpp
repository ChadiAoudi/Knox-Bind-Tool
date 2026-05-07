#include "framework.h"
#include "KnoxBindTool.h"
#include "config.h"
#include "input.h"
#include "ui.h"
#include "terminal.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    if(!config_init()){
        MessageBoxA(nullptr, "Failed to load config\\knox.json", "Knox Tools", MB_OK | MB_ICONERROR);
        return 1;
    }
    if(!start_input_thread()){
        MessageBoxA(nullptr, "Failed to start input thread", "Knox Tools", MB_OK | MB_ICONERROR);
        return 1;
    }
    if(!ui_init(hInstance, nCmdShow)){
        stop_input_thread();
        return 1;
    }

    while(!ui_should_close()){
        set_ui_wants_keyboard(ui_has_edit_focus());
        ui_render_frame();
    }

    stop_input_thread();
    shutdown_processes();
    ui_shutdown();
    return 0;
}
