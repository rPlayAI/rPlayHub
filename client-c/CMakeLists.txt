cmake_minimum_required(VERSION 3.20)
project(rplay-gui LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RPLAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(DEPS_DIR "${RPLAY_ROOT}/deps")

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${DEPS_DIR}/imgui
    ${DEPS_DIR}/imgui/backends
    ${DEPS_DIR}/json
)

# Source files
set(SOURCES
    rplay-gui.cpp
    stream-core.c
    audio.c
    ui/window_effects.cc
    ui/display_window.cc
    ui/twin_view.cc
    ui/fold_view.cc
    ui/png_decode.cc
    ${DEPS_DIR}/imgui/imgui.cpp
    ${DEPS_DIR}/imgui/imgui_draw.cpp
    ${DEPS_DIR}/imgui/imgui_tables.cpp
    ${DEPS_DIR}/imgui/imgui_widgets.cpp
    ${DEPS_DIR}/imgui/backends/imgui_impl_sdl2.cpp
    ${DEPS_DIR}/imgui/backends/imgui_impl_sdlrenderer2.cpp
)

add_executable(rplay-gui ${SOURCES})

if(MSVC)
    target_compile_definitions(rplay-gui PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    # SDL2
    target_include_directories(rplay-gui PRIVATE ${DEPS_DIR}/sdl2/include)
    target_link_directories(rplay-gui PRIVATE ${DEPS_DIR}/sdl2/lib/x64)
    target_link_libraries(rplay-gui PRIVATE SDL2main SDL2)

    # FFmpeg (RVRA patched)
    target_include_directories(rplay-gui PRIVATE ${DEPS_DIR}/ffmpeg-dist/include)
    target_link_directories(rplay-gui PRIVATE ${DEPS_DIR}/ffmpeg-dist/bin)
    target_link_libraries(rplay-gui PRIVATE
        avcodec
        avutil
        swscale
        swresample
    )

    # Windows system libraries
    target_link_libraries(rplay-gui PRIVATE
        ws2_32
        shell32
        comdlg32
        user32
        gdi32
        ole32
        advapi32
    )

    # Copy runtime DLLs to output directory post-build
    add_custom_command(TARGET rplay-gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DEPS_DIR}/sdl2/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:rplay-gui>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${DEPS_DIR}/ffmpeg-dist/bin"
            "$<TARGET_FILE_DIR:rplay-gui>"
    )
endif()
