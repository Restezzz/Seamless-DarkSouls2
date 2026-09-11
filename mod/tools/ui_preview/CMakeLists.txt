cmake_minimum_required(VERSION 3.20)
project(ui_preview CXX)

# Offscreen preview of the overlay: the real UI sources, the real ImGui, and
# stubs for everything that would talk to the game.
set(CMAKE_CXX_STANDARD 20)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(ui_preview
    preview_main.cpp
    preview_stubs.cpp
    ${ROOT}/src/ui/overlay.cpp
    ${ROOT}/src/ui/overlay_hud.cpp
    ${ROOT}/src/ui/ui_settings.cpp
    ${ROOT}/src/ui/ui_theme.cpp
    ${ROOT}/src/ui/ui_widgets.cpp
    ${ROOT}/third_party/imgui/imgui.cpp
    ${ROOT}/third_party/imgui/imgui_draw.cpp
    ${ROOT}/third_party/imgui/imgui_tables.cpp
    ${ROOT}/third_party/imgui/imgui_widgets.cpp
    ${ROOT}/third_party/imgui/backends/imgui_impl_dx11.cpp
)

target_include_directories(ui_preview PRIVATE
    ${ROOT}/include
    ${ROOT}/third_party/imgui
    ${ROOT}/third_party/imgui/backends
    ${ROOT}/third_party/minhook/include
)

target_compile_options(ui_preview PRIVATE /utf-8)
target_link_libraries(ui_preview PRIVATE d3d11 dxgi d3dcompiler ws2_32)
