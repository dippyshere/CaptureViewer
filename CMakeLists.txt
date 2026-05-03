cmake_minimum_required(VERSION 3.20)
project(PCKVM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_executable(viewer
    src/main.cpp
    src/Application.cpp
    src/DirectShowCapture.cpp
    src/D3DRenderer.cpp
    src/Settings.cpp
    src/DeviceEnumeration.cpp    
    src/AudioPlayback.cpp
    src/OverlayUI.cpp
    res/udsgr_std_003.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx12.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/widgets/imgui_toggle.cpp
    third_party/imgui/widgets/imgui_toggle_palette.cpp
    third_party/imgui/widgets/imgui_toggle_presets.cpp
    third_party/imgui/widgets/imgui_toggle_renderer.cpp
    res/pckvm.ico
    res/pckvm.rc
)

target_include_directories(viewer
    PRIVATE
        include
        third_party/imgui
        third_party/imgui/backends
)

if(MSVC)
    target_compile_options(viewer PRIVATE
        /permissive-
        /Zc:__cplusplus
        /MP
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/GL>
    )
    target_link_options(viewer PRIVATE $<$<CONFIG:Release>:/LTCG>)
    target_compile_definitions(viewer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

target_link_libraries(viewer
    PRIVATE
        d3d12
        d3dcompiler
        dxgi
        strmiids
)

set_target_properties(viewer PROPERTIES WIN32_EXECUTABLE TRUE)
