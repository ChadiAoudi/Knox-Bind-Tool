cmake_minimum_required(VERSION 3.20)
project(KnoxTool LANGUAGES CXX)
set(PRJNM ${PROJECT_NAME})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/lib/imgui)

find_package(OpenGL REQUIRED)
find_package(glfw3  REQUIRED)
find_package(Threads REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV  REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(INPUT REQUIRED IMPORTED_TARGET libinput)
find_package(nlohmann_json REQUIRED)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(imgui PUBLIC
    glfw
    OpenGL::GL
)

add_executable(${PRJNM}
    src/main.cpp
    src/config.cpp
    src/input.cpp
    src/ui.cpp
    src/terminal.cpp
)
target_link_libraries(${PRJNM} PRIVATE
    imgui
    PkgConfig::UDEV
    PkgConfig::INPUT
    nlohmann_json::nlohmann_json
    Threads::Threads
)
target_compile_options(${PRJNM} PRIVATE -Wall -Wextra -Wpedantic)
