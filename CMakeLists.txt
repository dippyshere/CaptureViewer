cmake_minimum_required(VERSION 3.20)
project(PCKVM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SERIAL_STREAMER_DEBUG "Enable verbose TLV packet logging" OFF)

add_executable(pckvm
    src/main.cpp
    src/Application.cpp
    src/DirectShowCapture.cpp
    src/D3DRenderer.cpp
    src/Settings.cpp
    src/DeviceEnumeration.cpp
    
    src/InputCapture.cpp
    
    src/AudioPlayback.cpp
    src/OverlayUI.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx12.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
)

target_include_directories(pckvm
    PRIVATE
        include
        third_party/imgui
        third_party/imgui/backends
)

if(MSVC)
    target_compile_options(pckvm PRIVATE /permissive- /Zc:__cplusplus /MP)
    target_compile_definitions(pckvm PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(SERIAL_STREAMER_DEBUG)
    target_compile_definitions(pckvm PRIVATE SERIAL_STREAMER_DEBUG=1)
else()
    target_compile_definitions(pckvm PRIVATE SERIAL_STREAMER_DEBUG=0)
endif()

target_link_libraries(pckvm
    PRIVATE
        d3d12
        dxgi
        d3dcompiler
        quartz
        strmiids
        ole32
        oleaut32
        setupapi
        propsys
        mmdevapi
)

set_target_properties(pckvm PROPERTIES WIN32_EXECUTABLE TRUE)
