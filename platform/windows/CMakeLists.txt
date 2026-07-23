add_library(saccade_platform_windows STATIC
    action_point_qualifier.cpp
    accessibility_provider.cpp
    agent_pipe.cpp
    artifact_verifier.cpp
    d3d12_capture_transfer.cpp
    desktop_pipeline.cpp
    display_topology.cpp
    glyph_atlas.cpp
    global_hotkeys.cpp
    input_executor.cpp
    keyboard.cpp
    neural_bridge.cpp
    operating_system.cpp
    overlay_surface.cpp
    runtime_scheduling.cpp
    scene_capture.cpp
    screen_capture.cpp)
target_include_directories(saccade_platform_windows
    PUBLIC "${PROJECT_SOURCE_DIR}"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_platform_windows
    PUBLIC saccade_core
    PRIVATE
        saccade_build_settings
        advapi32
        avrt
        d3d11
        d3d12
        dcomp
        dwmapi
        dxgi
        gdi32
        runtimeobject
        ole32
        oleaut32
        uiautomationcore
        user32
        windowsapp)
target_link_libraries(saccade_platform_windows PRIVATE bcrypt)
if(TARGET saccade_backend_d3d12)
    target_link_libraries(saccade_platform_windows
        PUBLIC saccade_backend_d3d12)
endif()
