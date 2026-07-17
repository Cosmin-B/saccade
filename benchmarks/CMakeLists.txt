add_executable(saccade_benchmark_overlay_packet overlay_packet.cpp)
target_include_directories(saccade_benchmark_overlay_packet PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_benchmark_overlay_packet PRIVATE
    saccade_core
    saccade_build_settings)

add_executable(saccade_benchmark_coordinate_transform coordinate_transform.cpp)
target_include_directories(saccade_benchmark_coordinate_transform PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_benchmark_coordinate_transform PRIVATE
    saccade_core
    saccade_build_settings)

add_executable(saccade_benchmark_interaction interaction.cpp)
target_include_directories(saccade_benchmark_interaction PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_benchmark_interaction PRIVATE
    saccade_core
    saccade_build_settings)

add_executable(saccade_benchmark_overlay_compose overlay_compose.cpp)
target_include_directories(saccade_benchmark_overlay_compose PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_benchmark_overlay_compose PRIVATE
    saccade_core
    saccade_build_settings)

add_executable(saccade_benchmark_scene_fusion scene_fusion.cpp)
target_include_directories(saccade_benchmark_scene_fusion PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_benchmark_scene_fusion PRIVATE
    saccade_core
    saccade_build_settings)

if(TARGET saccade_platform_macos)
    add_executable(saccade_benchmark_macos_display_stimulus
        macos_display_stimulus.mm)
    target_include_directories(saccade_benchmark_macos_display_stimulus PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_macos_display_stimulus PRIVATE
        saccade_core
        saccade_build_settings
        "-framework AppKit"
        "-framework QuartzCore")
    target_compile_options(saccade_benchmark_macos_display_stimulus PRIVATE
        -fobjc-arc)

    add_executable(saccade_benchmark_macos_surface_qualifier
        macos_surface_qualifier.mm)
    target_include_directories(saccade_benchmark_macos_surface_qualifier PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_macos_surface_qualifier PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings)
    target_compile_options(saccade_benchmark_macos_surface_qualifier PRIVATE
        -fobjc-arc)

    add_executable(saccade_benchmark_macos_coreml_model macos_coreml_model.mm)
    target_include_directories(saccade_benchmark_macos_coreml_model PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_macos_coreml_model PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings
        "-framework CoreML"
        "-framework CoreVideo"
        "-framework Foundation")
    target_compile_options(saccade_benchmark_macos_coreml_model PRIVATE -fobjc-arc)

    add_executable(saccade_benchmark_macos_full_scope macos_full_scope.mm)
    target_include_directories(saccade_benchmark_macos_full_scope PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_macos_full_scope PRIVATE
        saccade_platform_macos_capture
        saccade_build_settings
        "-framework CoreGraphics"
        "-framework Metal")
    target_compile_options(saccade_benchmark_macos_full_scope PRIVATE -fobjc-arc)
    add_dependencies(saccade_benchmark_macos_full_scope saccade_overlay_metallib)

    if(TARGET saccade_platform_macos_overlay)
        add_executable(saccade_benchmark_macos_overlay_presentation
            macos_overlay_presentation.mm)
        target_include_directories(saccade_benchmark_macos_overlay_presentation PRIVATE
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(saccade_benchmark_macos_overlay_presentation PRIVATE
            saccade_platform_macos_overlay
            saccade_build_settings
            "-framework AppKit")
        target_compile_options(saccade_benchmark_macos_overlay_presentation PRIVATE -fobjc-arc)
        add_dependencies(saccade_benchmark_macos_overlay_presentation saccade_overlay_metallib)
    endif()

    add_executable(saccade_benchmark_macos_input macos_input.cpp)
    target_include_directories(saccade_benchmark_macos_input PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_macos_input PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings
        "-framework Carbon"
        "-framework CoreGraphics"
        "-framework IOKit")
endif()

if(WIN32 AND TARGET saccade_platform_windows)
    add_executable(saccade_benchmark_windows_capture windows_capture.cpp)
    target_include_directories(saccade_benchmark_windows_capture PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_windows_capture PRIVATE
        saccade_platform_windows
        saccade_build_settings
        d3d11
        gdi32)

    add_executable(saccade_benchmark_windows_preprocess windows_preprocess.cpp)
    target_include_directories(saccade_benchmark_windows_preprocess PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_windows_preprocess PRIVATE
        saccade_platform_windows
        saccade_backend_d3d12
        saccade_build_settings
        d3d11
        d3d12)
    add_dependencies(saccade_benchmark_windows_preprocess
        saccade_d3d12_shaders)

    if(TARGET saccade_backend_d3d12)
        if(SACCADE_WINDOWS_ML_AVAILABLE)
            set(saccade_windows_ort_runtime_files
                "${WINML_BINARY_DIR}/onnxruntime.dll")
            if(NOT "${WINML_ONNXRUNTIME_PROVIDERS_SHARED_DLL}" STREQUAL "")
                list(APPEND saccade_windows_ort_runtime_files
                    "${WINML_ONNXRUNTIME_PROVIDERS_SHARED_DLL}")
            endif()
            set(saccade_windows_directml_runtime_files
                ${saccade_windows_ort_runtime_files}
                "${WINML_DIRECTML_DLL}")

            add_executable(saccade_benchmark_windows_cpu_model
                windows_cpu_model.cpp)
            target_include_directories(saccade_benchmark_windows_cpu_model PRIVATE
                "${WINML_INCLUDE_DIR}/winml")
            target_link_libraries(saccade_benchmark_windows_cpu_model PRIVATE
                saccade_build_settings
                psapi)
            foreach(runtime_file IN LISTS saccade_windows_ort_runtime_files)
                add_custom_command(TARGET saccade_benchmark_windows_cpu_model POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${runtime_file}"
                        $<TARGET_FILE_DIR:saccade_benchmark_windows_cpu_model>
                    VERBATIM)
            endforeach()
        endif()

        add_executable(saccade_benchmark_windows_directml_model
            windows_directml_model.cpp)
        target_include_directories(saccade_benchmark_windows_directml_model PRIVATE
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(saccade_benchmark_windows_directml_model PRIVATE
            saccade_backend_d3d12
            saccade_build_settings
            d3d12
            psapi)
        if(SACCADE_WINDOWS_ML_AVAILABLE)
            foreach(runtime_file IN LISTS saccade_windows_directml_runtime_files)
                add_custom_command(TARGET saccade_benchmark_windows_directml_model POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${runtime_file}"
                        $<TARGET_FILE_DIR:saccade_benchmark_windows_directml_model>
                    VERBATIM)
            endforeach()
        endif()

        if(SACCADE_WINDOWS_ML_AVAILABLE)
            add_executable(saccade_benchmark_windows_d3d12_memory_soak
                windows_d3d12_memory_soak.cpp)
            target_include_directories(saccade_benchmark_windows_d3d12_memory_soak PRIVATE
                "${PROJECT_SOURCE_DIR}"
                "${PROJECT_SOURCE_DIR}/include"
                "${PROJECT_SOURCE_DIR}/src")
            target_link_libraries(saccade_benchmark_windows_d3d12_memory_soak PRIVATE
                saccade_backend_d3d12
                saccade_build_settings
                d3d12
                dxgi
                psapi)
            add_dependencies(saccade_benchmark_windows_d3d12_memory_soak
                saccade_d3d12_shaders)
            foreach(runtime_file IN LISTS saccade_windows_directml_runtime_files)
                add_custom_command(TARGET saccade_benchmark_windows_d3d12_memory_soak POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${runtime_file}"
                        "$<TARGET_FILE_DIR:saccade_benchmark_windows_d3d12_memory_soak>"
                    VERBATIM)
            endforeach()
        endif()

        add_executable(saccade_benchmark_windows_d3d12_capture
            windows_d3d12_capture.cpp)
        target_include_directories(saccade_benchmark_windows_d3d12_capture PRIVATE
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(saccade_benchmark_windows_d3d12_capture PRIVATE
            saccade_platform_windows
            saccade_backend_d3d12
            saccade_build_settings
            d3d11
            d3d12)

        if(SACCADE_WINDOWS_ML_AVAILABLE)
            add_executable(saccade_benchmark_windows_full_scope
                windows_full_scope.cpp)
            target_include_directories(saccade_benchmark_windows_full_scope PRIVATE
                "${PROJECT_SOURCE_DIR}"
                "${PROJECT_SOURCE_DIR}/include"
                "${PROJECT_SOURCE_DIR}/src")
            target_link_libraries(saccade_benchmark_windows_full_scope PRIVATE
                saccade_platform_windows
                saccade_backend_d3d12
                saccade_core
                saccade_build_settings)
            add_dependencies(saccade_benchmark_windows_full_scope
                saccade_d3d12_shaders)
            foreach(runtime_file IN LISTS saccade_windows_directml_runtime_files)
                add_custom_command(TARGET saccade_benchmark_windows_full_scope POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${runtime_file}"
                        "$<TARGET_FILE_DIR:saccade_benchmark_windows_full_scope>"
                    VERBATIM)
            endforeach()
        endif()
    endif()

    add_executable(saccade_windows_interactive_launch
        windows_interactive_launch.cpp)
    target_include_directories(saccade_windows_interactive_launch PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_windows_interactive_launch PRIVATE
        saccade_build_settings
        advapi32)
endif()

if(TARGET saccade_backend_metal)
    add_executable(saccade_benchmark_metal_preprocess_resolution
        metal_preprocess_resolution.mm)
    target_include_directories(saccade_benchmark_metal_preprocess_resolution PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_metal_preprocess_resolution PRIVATE
        saccade_backend_metal
        saccade_core
        saccade_build_settings
        "-framework Metal")
    target_compile_options(saccade_benchmark_metal_preprocess_resolution PRIVATE
        -fobjc-arc)
    add_dependencies(saccade_benchmark_metal_preprocess_resolution
        saccade_overlay_metallib)

    add_executable(saccade_benchmark_metal_target_postprocess
        metal_target_postprocess.mm)
    target_include_directories(saccade_benchmark_metal_target_postprocess PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_metal_target_postprocess PRIVATE
        saccade_backend_metal
        saccade_core
        saccade_build_settings
        "-framework Metal")
    target_compile_options(saccade_benchmark_metal_target_postprocess PRIVATE
        -fobjc-arc)
    add_dependencies(saccade_benchmark_metal_target_postprocess
        saccade_overlay_metallib)

    add_executable(saccade_benchmark_screen_capture screen_capture.mm)
    target_include_directories(saccade_benchmark_screen_capture PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_screen_capture PRIVATE
        saccade_platform_macos_capture
        saccade_backend_metal
        saccade_build_settings
        "-framework CoreGraphics"
        "-framework Metal")
    target_compile_options(saccade_benchmark_screen_capture PRIVATE -fobjc-arc)
    add_dependencies(saccade_benchmark_screen_capture saccade_overlay_metallib)

    add_executable(saccade_benchmark_metal_overlay metal_overlay.cpp)
    target_include_directories(saccade_benchmark_metal_overlay PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_metal_overlay PRIVATE
        saccade_backend_metal
        saccade_core
        saccade_build_settings)
    add_dependencies(saccade_benchmark_metal_overlay
        saccade_overlay_metallib)

    add_executable(saccade_benchmark_metal_renderer
        metal_renderer.mm)
    target_include_directories(saccade_benchmark_metal_renderer PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_benchmark_metal_renderer PRIVATE
        saccade_backend_metal
        saccade_core
        saccade_build_settings
        "-framework Metal")
    target_compile_options(saccade_benchmark_metal_renderer PRIVATE
        -fobjc-arc)
    add_dependencies(saccade_benchmark_metal_renderer
        saccade_overlay_metallib)
endif()
