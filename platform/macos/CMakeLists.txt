add_library(saccade_platform_macos STATIC
    accessibility_provider.cpp
    agent_socket.mm
    artifact_verifier.mm
    coreml_model.mm
    coreml_provider.mm
    display_topology.mm
    glyph_atlas.mm
    global_hotkeys.cpp
    keyboard.cpp
    input_monitor.mm
    input_executor.mm
    surface_qualifier.mm)
target_include_directories(saccade_platform_macos
    PUBLIC "${PROJECT_SOURCE_DIR}"
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
target_compile_options(saccade_platform_macos PRIVATE -fobjc-arc)
target_link_libraries(saccade_platform_macos
    PUBLIC saccade_core
    PRIVATE
        saccade_build_settings
        Threads::Threads
        "-framework ApplicationServices"
        "-framework AppKit"
        "-framework CoreML"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework Security")
target_link_libraries(saccade_platform_macos PRIVATE
    "-framework Carbon"
    "-framework CoreVideo"
    "-framework IOKit")

if(TARGET saccade_backend_metal)
    add_library(saccade_platform_macos_capture STATIC
        coreml_image_bridge.cpp
        scene_capture.cpp
        screen_capture.mm)
    target_include_directories(saccade_platform_macos_capture
        PUBLIC "${PROJECT_SOURCE_DIR}"
        PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(saccade_platform_macos_capture PRIVATE -fobjc-arc)
    target_link_libraries(saccade_platform_macos_capture
        PUBLIC
            saccade_platform_macos
            saccade_backend_metal
        PRIVATE
            saccade_build_settings
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework Foundation"
            "-framework IOSurface"
            "-framework Metal"
            "-framework ScreenCaptureKit")

    add_library(saccade_platform_macos_overlay STATIC overlay_surface.mm)
    target_include_directories(saccade_platform_macos_overlay
        PUBLIC "${PROJECT_SOURCE_DIR}"
        PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(saccade_platform_macos_overlay PRIVATE -fobjc-arc)
    target_link_libraries(saccade_platform_macos_overlay
        PUBLIC
            saccade_platform_macos
            saccade_backend_metal
        PRIVATE
            saccade_build_settings
            "-framework AppKit"
            "-framework CoreGraphics"
            "-framework Metal"
            "-framework QuartzCore")

    add_library(saccade_platform_macos_pipeline STATIC desktop_pipeline.mm)
    target_include_directories(saccade_platform_macos_pipeline
        PUBLIC "${PROJECT_SOURCE_DIR}"
        PRIVATE
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(saccade_platform_macos_pipeline PRIVATE -fobjc-arc)
    target_link_libraries(saccade_platform_macos_pipeline
        PUBLIC
            saccade_platform_macos_capture
            saccade_platform_macos_overlay
        PRIVATE
            saccade_build_settings
            "-framework AppKit"
            "-framework CoreGraphics"
            "-framework Metal")
endif()
