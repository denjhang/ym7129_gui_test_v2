cmake_minimum_required(VERSION 3.15)
project(YM7129_Drum_Pad CXX C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ImGui sources
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_win32.cpp
    ${IMGUI_DIR}/imgui_impl_dx11.cpp
)

# App sources
add_executable(drum_gui WIN32
    drum_gui.cpp
    ym7129.c
    spfm_lite.c
    error_lite.c
    ${IMGUI_SOURCES}
)

target_include_directories(drum_gui PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${IMGUI_DIR}
)

# Link FTD2XX + DX11
target_link_libraries(drum_gui PRIVATE
    ${CMAKE_SOURCE_DIR}/libftd2xx.a
    ws2_32
    d3d11
    dxgi
    d3dcompiler
    dwmapi
)

# Static runtime so the exe runs without libgcc_s_seh-1.dll / libstdc++-6.dll
if(MINGW)
    target_link_options(drum_gui PRIVATE -static)
endif()

# For MSVC: copy DLL to output
if(EXISTS ${CMAKE_SOURCE_DIR}/ftd2xx64.dll)
    add_custom_command(TARGET drum_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/ftd2xx64.dll $<TARGET_FILE_DIR:drum_gui>
    )
endif()
