add_executable(saccade_build_smoke smoke/build_smoke.cpp)
target_link_libraries(saccade_build_smoke PRIVATE saccade_build_settings)
add_test(NAME smoke.build COMMAND saccade_build_smoke)

add_executable(saccade_header_c abi/header_c.c)
target_include_directories(saccade_header_c PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_header_c PRIVATE saccade_core saccade_build_settings)
add_test(NAME abi.header_c COMMAND saccade_header_c)

add_executable(saccade_header_cpp abi/header_cpp.cpp)
target_include_directories(saccade_header_cpp PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_header_cpp PRIVATE saccade_core saccade_build_settings)
add_test(NAME abi.header_cpp COMMAND saccade_header_cpp)

add_executable(saccade_overlay_header_c abi/overlay_header_c.c)
target_include_directories(saccade_overlay_header_c PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_overlay_header_c PRIVATE saccade_build_settings)
add_test(NAME abi.overlay.header_c COMMAND saccade_overlay_header_c)

add_executable(saccade_overlay_header_cpp abi/overlay_header_cpp.cpp)
target_include_directories(saccade_overlay_header_cpp PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_overlay_header_cpp PRIVATE saccade_build_settings)
add_test(NAME abi.overlay.header_cpp COMMAND saccade_overlay_header_cpp)

add_executable(saccade_agent_header_c abi/agent_header_c.c)
target_include_directories(saccade_agent_header_c PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_agent_header_c PRIVATE saccade_build_settings)
add_test(NAME abi.agent.header_c COMMAND saccade_agent_header_c)

add_executable(saccade_agent_header_cpp abi/agent_header_cpp.cpp)
target_include_directories(saccade_agent_header_cpp PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_agent_header_cpp PRIVATE saccade_build_settings)
add_test(NAME abi.agent.header_cpp COMMAND saccade_agent_header_cpp)

add_executable(saccade_agent_service agent/service.cpp)
target_include_directories(saccade_agent_service PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_agent_service PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME agent.service COMMAND saccade_agent_service)

add_executable(saccade_protocol_fuzz security/protocol_fuzz.cpp)
target_include_directories(saccade_protocol_fuzz PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_protocol_fuzz PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME security.protocol_fuzz COMMAND saccade_protocol_fuzz)

add_executable(saccade_abi_layout abi/layout.c)
target_include_directories(saccade_abi_layout PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_abi_layout PRIVATE saccade_build_settings)
add_test(NAME abi.layout COMMAND saccade_abi_layout)

add_executable(saccade_typed_facade_c abi/typed_facade_c.c)
target_include_directories(saccade_typed_facade_c PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_typed_facade_c PRIVATE saccade_build_settings)
add_test(NAME abi.typed_facade_c COMMAND saccade_typed_facade_c)

add_executable(saccade_typed_facade_cpp abi/typed_facade_cpp.cpp)
target_include_directories(saccade_typed_facade_cpp PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_typed_facade_cpp PRIVATE saccade_build_settings)
add_test(NAME abi.typed_facade_cpp COMMAND saccade_typed_facade_cpp)

set(typed_facade_script "${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_reject.cmake")
set(typed_facade_common_arguments
    "-DC_COMPILER=${CMAKE_C_COMPILER}"
    "-DC_COMPILER_ID=${CMAKE_C_COMPILER_ID}"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
    "-DINCLUDE_DIR=${PROJECT_SOURCE_DIR}/include"
    "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}")

add_test(
    NAME abi.typed_facade.reject
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=reject
        ${typed_facade_common_arguments}
        "-DUNSUPPORTED_C_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_unsupported.c"
        "-DUNSUPPORTED_CXX_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_unsupported.cpp"
        -P "${typed_facade_script}")

add_test(
    NAME abi.typed_facade.static_dispatch
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=preprocess
        ${typed_facade_common_arguments}
        "-DSUPPORTED_C_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_c.c"
        "-DSUPPORTED_CXX_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/abi/typed_facade_cpp.cpp"
        -P "${typed_facade_script}")

add_executable(saccade_backend_header_c abi/backend_header_c.c)
target_include_directories(saccade_backend_header_c PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_backend_header_c PRIVATE saccade_build_settings)
add_test(NAME abi.backend.header_c COMMAND saccade_backend_header_c)

add_executable(saccade_backend_header_cpp abi/backend_header_cpp.cpp)
target_include_directories(saccade_backend_header_cpp PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_backend_header_cpp PRIVATE saccade_build_settings)
add_test(NAME abi.backend.header_cpp COMMAND saccade_backend_header_cpp)

add_executable(saccade_backend_layout abi/backend_layout.c)
target_include_directories(saccade_backend_layout PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_backend_layout PRIVATE saccade_build_settings)
add_test(NAME abi.backend.layout COMMAND saccade_backend_layout)

add_executable(saccade_abi_manifest abi/manifest.c)
target_include_directories(saccade_abi_manifest PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_abi_manifest PRIVATE saccade_build_settings)
add_test(NAME abi.manifest.layout COMMAND saccade_abi_manifest)

add_test(
    NAME abi.manifest.symbols
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/symbol_manifest.cmake")

add_executable(saccade_core_error core/error.cpp)
target_include_directories(saccade_core_error PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_core_error PRIVATE
    saccade_core
    saccade_build_settings
    Threads::Threads)
add_test(NAME core.error COMMAND saccade_core_error)

add_executable(saccade_core_stack_string_builder core/stack_string_builder.cpp)
target_include_directories(saccade_core_stack_string_builder PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_core_stack_string_builder PRIVATE
    saccade_build_settings)
add_test(NAME core.stack_string_builder COMMAND saccade_core_stack_string_builder)

add_executable(saccade_core_handle_table core/handle_table.cpp)
target_include_directories(saccade_core_handle_table PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_core_handle_table PRIVATE saccade_build_settings)
add_test(NAME core.handle_table COMMAND saccade_core_handle_table)

add_executable(saccade_core_frame_lease core/frame_lease.cpp)
target_include_directories(saccade_core_frame_lease PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_core_frame_lease PRIVATE saccade_build_settings)
add_test(NAME core.frame_lease COMMAND saccade_core_frame_lease)

add_executable(saccade_core_newest_frame_mailbox core/newest_frame_mailbox.cpp)
target_include_directories(saccade_core_newest_frame_mailbox PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_core_newest_frame_mailbox PRIVATE
    saccade_build_settings
    Threads::Threads)
add_test(NAME core.newest_frame_mailbox COMMAND saccade_core_newest_frame_mailbox)

add_executable(saccade_kernels_image_luma kernels/image_luma.cpp)
target_include_directories(saccade_kernels_image_luma PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_kernels_image_luma PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME kernels.image_luma COMMAND saccade_kernels_image_luma)

add_executable(saccade_kernels_target_postprocess
    kernels/target_postprocess.cpp)
target_include_directories(saccade_kernels_target_postprocess PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_kernels_target_postprocess PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME kernels.target_postprocess
    COMMAND saccade_kernels_target_postprocess)

add_executable(saccade_input_plan input/plan.cpp)
target_include_directories(saccade_input_plan PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_input_plan PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME input.plan COMMAND saccade_input_plan)

add_executable(saccade_input_execution_preflight input/execution_preflight.cpp)
target_include_directories(saccade_input_execution_preflight PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_input_execution_preflight PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME input.execution_preflight COMMAND saccade_input_execution_preflight)

add_executable(saccade_model_artifact model/artifact.cpp)
target_include_directories(saccade_model_artifact PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_model_artifact PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME model.artifact COMMAND saccade_model_artifact)

add_executable(saccade_model_coreml_contract model/coreml_contract.cpp)
target_include_directories(saccade_model_coreml_contract PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_model_coreml_contract PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME model.coreml_contract COMMAND saccade_model_coreml_contract)

add_executable(saccade_interaction_hints interaction/hints.cpp)
target_include_directories(saccade_interaction_hints PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_interaction_hints PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME interaction.hints COMMAND saccade_interaction_hints)

add_executable(saccade_interaction_action_planner
    interaction/action_planner.cpp)
target_include_directories(saccade_interaction_action_planner PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_interaction_action_planner PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME interaction.action_planner
    COMMAND saccade_interaction_action_planner)

add_executable(saccade_interaction_selection_reducer
    interaction/selection_reducer.cpp)
target_include_directories(saccade_interaction_selection_reducer PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_interaction_selection_reducer PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME interaction.selection_reducer
    COMMAND saccade_interaction_selection_reducer)

add_executable(saccade_application_session application/session.cpp)
target_include_directories(saccade_application_session PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_session PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME application.session COMMAND saccade_application_session)

add_executable(saccade_application_desktop_host application/desktop_host.cpp)
target_include_directories(saccade_application_desktop_host PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_desktop_host PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME application.desktop_host COMMAND saccade_application_desktop_host)

add_executable(saccade_application_desktop_runtime
    application/desktop_runtime.cpp)
target_include_directories(saccade_application_desktop_runtime PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_desktop_runtime PRIVATE
    saccade_backend_reference_cpu
    saccade_core
    saccade_build_settings)
add_test(NAME application.desktop_runtime
    COMMAND saccade_application_desktop_runtime)

add_executable(saccade_application_debugger application/debugger.cpp)
target_include_directories(saccade_application_debugger PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_debugger PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME application.debugger COMMAND saccade_application_debugger)

add_executable(saccade_application_debugger_layout application/debugger_layout.cpp)
target_include_directories(saccade_application_debugger_layout PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_debugger_layout PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME application.debugger_layout COMMAND saccade_application_debugger_layout)

add_executable(saccade_application_settings application/settings.cpp)
target_include_directories(saccade_application_settings PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_settings PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME application.settings COMMAND saccade_application_settings)

add_executable(saccade_application_overlay_composer
    application/overlay_composer.cpp)
target_include_directories(saccade_application_overlay_composer PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_overlay_composer PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME application.overlay_composer
    COMMAND saccade_application_overlay_composer)

add_executable(saccade_application_interaction_controller
    application/interaction_controller.cpp)
target_include_directories(saccade_application_interaction_controller PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_interaction_controller PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME application.interaction_controller
    COMMAND saccade_application_interaction_controller)

add_executable(saccade_application_recovery_schedule
    application/recovery_schedule.cpp)
target_include_directories(saccade_application_recovery_schedule PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_recovery_schedule PRIVATE
    saccade_build_settings)
add_test(NAME application.recovery_schedule
    COMMAND saccade_application_recovery_schedule)

add_executable(saccade_application_window_navigator
    application/window_navigator.cpp)
target_include_directories(saccade_application_window_navigator PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_window_navigator PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME application.window_navigator
    COMMAND saccade_application_window_navigator)

add_executable(saccade_application_scene_coordinator
    application/scene_coordinator.cpp)
target_include_directories(saccade_application_scene_coordinator PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_application_scene_coordinator PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME application.scene_coordinator
    COMMAND saccade_application_scene_coordinator)

add_executable(saccade_overlay_packet overlay/packet.cpp)
target_include_directories(saccade_overlay_packet PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_overlay_packet PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME overlay.packet COMMAND saccade_overlay_packet)

add_executable(saccade_scene_packet scene/packet.cpp)
target_include_directories(saccade_scene_packet PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_scene_packet PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME scene.packet COMMAND saccade_scene_packet)

add_executable(saccade_scene_grid scene/grid.cpp)
target_include_directories(saccade_scene_grid PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_scene_grid PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME scene.grid COMMAND saccade_scene_grid)

add_executable(saccade_scene_store scene/store.cpp)
target_include_directories(saccade_scene_store PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_scene_store PRIVATE
    saccade_core
    saccade_build_settings
    Threads::Threads)
add_test(NAME scene.store COMMAND saccade_scene_store)

add_executable(saccade_scene_fusion scene/fusion.cpp)
target_include_directories(saccade_scene_fusion PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_scene_fusion PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME scene.fusion COMMAND saccade_scene_fusion)

add_executable(saccade_geometry_coordinate_transform
    geometry/coordinate_transform.cpp)
target_include_directories(saccade_geometry_coordinate_transform PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_geometry_coordinate_transform PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME geometry.coordinate_transform
    COMMAND saccade_geometry_coordinate_transform)

add_executable(saccade_geometry_display_catalog geometry/display_catalog.cpp)
target_include_directories(saccade_geometry_display_catalog PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_geometry_display_catalog PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME geometry.display_catalog COMMAND saccade_geometry_display_catalog)

add_executable(saccade_geometry_scope_atlas geometry/scope_atlas.cpp)
target_include_directories(saccade_geometry_scope_atlas PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_geometry_scope_atlas PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME geometry.scope_atlas COMMAND saccade_geometry_scope_atlas)

add_executable(saccade_scheduler_dual_rate scheduler/dual_rate_scheduler.cpp)
target_include_directories(saccade_scheduler_dual_rate PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_scheduler_dual_rate PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME scheduler.dual_rate COMMAND saccade_scheduler_dual_rate)

add_executable(saccade_scheduler_neural_coordinator
    scheduler/neural_coordinator.cpp)
target_include_directories(saccade_scheduler_neural_coordinator PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_scheduler_neural_coordinator PRIVATE
    saccade_backend_reference_cpu
    saccade_core
    saccade_build_settings)
add_test(NAME scheduler.neural_coordinator
    COMMAND saccade_scheduler_neural_coordinator)

add_executable(saccade_scheduler_desktop_neural_coordinator
    scheduler/desktop_neural_coordinator.cpp)
target_include_directories(saccade_scheduler_desktop_neural_coordinator PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_scheduler_desktop_neural_coordinator PRIVATE
    saccade_backend_reference_cpu
    saccade_core
    saccade_build_settings)
add_test(NAME scheduler.desktop_neural_coordinator
    COMMAND saccade_scheduler_desktop_neural_coordinator)

if(TARGET saccade_platform_macos)
    add_executable(saccade_platform_macos_surface_qualifier
        platform/macos/surface_qualifier.mm)
    target_include_directories(saccade_platform_macos_surface_qualifier PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(saccade_platform_macos_surface_qualifier PRIVATE -fobjc-arc)
    target_link_libraries(saccade_platform_macos_surface_qualifier PRIVATE
        saccade_platform_macos
        saccade_build_settings
        "-framework ApplicationServices"
        "-framework AppKit"
        "-framework CoreGraphics")
    add_test(NAME platform.macos.surface_qualifier
        COMMAND saccade_platform_macos_surface_qualifier)

    add_executable(saccade_model_p256_verifier model/p256_verifier.cpp)
    target_include_directories(saccade_model_p256_verifier PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_model_p256_verifier PRIVATE
        saccade_platform_macos
        saccade_build_settings)
    add_test(NAME model.p256_verifier COMMAND saccade_model_p256_verifier)

    add_executable(saccade_platform_macos_coreml_model
        platform/macos/coreml_model.cpp)
    target_include_directories(saccade_platform_macos_coreml_model PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_coreml_model PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings
        "-framework CoreML"
        "-framework CoreVideo"
        "-framework Foundation")
    add_test(NAME platform.macos.coreml_model
        COMMAND saccade_platform_macos_coreml_model)

    add_executable(saccade_platform_macos_coreml_provider
        platform/macos/coreml_provider.cpp)
    set_source_files_properties(platform/macos/coreml_provider.cpp
        PROPERTIES LANGUAGE OBJCXX)
    target_include_directories(saccade_platform_macos_coreml_provider PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_coreml_provider PRIVATE
        saccade_platform_macos
        saccade_build_settings
        "-framework CoreFoundation"
        "-framework CoreML"
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework IOSurface")
    target_compile_options(saccade_platform_macos_coreml_provider PRIVATE
        -fobjc-arc)
    if(TARGET saccade_backend_metal)
        target_compile_definitions(saccade_platform_macos_coreml_provider PRIVATE
            SACCADE_TEST_METAL_PREPROCESSOR=1)
        target_link_libraries(saccade_platform_macos_coreml_provider PRIVATE
            saccade_backend_metal)
        add_dependencies(saccade_platform_macos_coreml_provider
            saccade_overlay_metallib)
    endif()
    add_test(NAME platform.macos.coreml_provider
        COMMAND saccade_platform_macos_coreml_provider)

    add_executable(saccade_platform_macos_input_monitor
        platform/macos/input_monitor.cpp)
    target_include_directories(saccade_platform_macos_input_monitor PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_input_monitor PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings
        "-framework CoreGraphics")
    add_test(NAME platform.macos.input_monitor
        COMMAND saccade_platform_macos_input_monitor)

    add_executable(saccade_platform_macos_global_hotkeys
        platform/macos/global_hotkeys.cpp)
    target_include_directories(saccade_platform_macos_global_hotkeys PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_global_hotkeys PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings
        "-framework Carbon"
        "-framework IOKit")
    add_test(NAME platform.macos.global_hotkeys
        COMMAND saccade_platform_macos_global_hotkeys)
    set_tests_properties(platform.macos.global_hotkeys
        PROPERTIES LABELS "live;live-registration")

    add_executable(saccade_platform_macos_input_executor
        platform/macos/input_executor.cpp)
    target_include_directories(saccade_platform_macos_input_executor PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_input_executor PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings
        "-framework Carbon"
        "-framework CoreGraphics"
        "-framework IOKit")
    add_test(NAME platform.macos.input_executor
        COMMAND saccade_platform_macos_input_executor)

    add_executable(saccade_platform_macos_input_workflow_live
        platform/macos/input_workflow_live.mm)
    target_include_directories(saccade_platform_macos_input_workflow_live PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(saccade_platform_macos_input_workflow_live PRIVATE
        -fobjc-arc)
    target_link_libraries(saccade_platform_macos_input_workflow_live PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings
        "-framework AppKit"
        "-framework CoreGraphics")
    add_test(NAME platform.macos.input_workflow.live
        COMMAND saccade_platform_macos_input_workflow_live)
    set_tests_properties(platform.macos.input_workflow.live PROPERTIES
        LABELS "live;live-workflow"
        SKIP_RETURN_CODE 77
        TIMEOUT 60)

    add_executable(saccade_platform_macos_accessibility_provider
        platform/macos/accessibility_provider.cpp)
    set_source_files_properties(platform/macos/accessibility_provider.cpp
        PROPERTIES LANGUAGE OBJCXX)
    target_compile_options(saccade_platform_macos_accessibility_provider PRIVATE
        -fobjc-arc)
    target_include_directories(saccade_platform_macos_accessibility_provider PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_accessibility_provider PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings
        "-framework ApplicationServices"
        "-framework AppKit"
        "-framework CoreGraphics")
    add_test(NAME platform.macos.accessibility_provider
        COMMAND saccade_platform_macos_accessibility_provider)
    set_tests_properties(platform.macos.accessibility_provider
        PROPERTIES
            LABELS "live;live-owned-window"
            SKIP_RETURN_CODE 77
            TIMEOUT 30)

    set_source_files_properties(platform/macos/display_topology.cpp
        PROPERTIES LANGUAGE OBJCXX)
    add_executable(saccade_platform_macos_display_topology
        platform/macos/display_topology.cpp)
    target_include_directories(saccade_platform_macos_display_topology PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_display_topology PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings
        "-framework AppKit")
    target_compile_options(saccade_platform_macos_display_topology PRIVATE
        -fobjc-arc)
    add_test(NAME platform.macos.display_topology
        COMMAND saccade_platform_macos_display_topology)
    set_tests_properties(platform.macos.display_topology
        PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(saccade_platform_macos_glyph_atlas
        platform/macos/glyph_atlas.cpp)
    target_include_directories(saccade_platform_macos_glyph_atlas PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_glyph_atlas PRIVATE
        saccade_platform_macos
        saccade_core
        saccade_build_settings)
    add_test(NAME platform.macos.glyph_atlas
        COMMAND saccade_platform_macos_glyph_atlas)
endif()

if(TARGET saccade_platform_windows)
    add_executable(saccade_model_p256_verifier model/p256_verifier.cpp)
    target_include_directories(saccade_model_p256_verifier PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_model_p256_verifier PRIVATE
        saccade_platform_windows
        saccade_build_settings)
    add_test(NAME model.p256_verifier COMMAND saccade_model_p256_verifier)

    add_executable(saccade_core_win32_capture_import
        core/win32_capture_import.cpp)
    target_include_directories(saccade_core_win32_capture_import PRIVATE
        "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(saccade_core_win32_capture_import PRIVATE
        saccade_core
        saccade_build_settings)
    add_test(NAME core.win32_capture_import
        COMMAND saccade_core_win32_capture_import)

    add_executable(saccade_platform_windows_agent_pipe
        platform/windows/agent_pipe.cpp)
    target_include_directories(saccade_platform_windows_agent_pipe PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(saccade_platform_windows_agent_pipe PRIVATE
        saccade_platform_windows
        saccade_build_settings)
    add_test(NAME platform.windows.agent_pipe
        COMMAND saccade_platform_windows_agent_pipe)

    add_executable(saccade_platform_windows_global_hotkeys
        platform/windows/global_hotkeys.cpp)
    target_include_directories(saccade_platform_windows_global_hotkeys PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_windows_global_hotkeys PRIVATE
        saccade_platform_windows
        saccade_core
        saccade_build_settings
        user32)
    add_test(NAME platform.windows.global_hotkeys
        COMMAND saccade_platform_windows_global_hotkeys)
    set_tests_properties(platform.windows.global_hotkeys
        PROPERTIES LABELS "live;live-registration")

    add_executable(saccade_platform_windows_display_topology
        platform/windows/display_topology.cpp)
    target_include_directories(saccade_platform_windows_display_topology PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_windows_display_topology PRIVATE
        saccade_platform_windows
        saccade_core
        saccade_build_settings
        user32)
    add_test(NAME platform.windows.display_topology
        COMMAND saccade_platform_windows_display_topology)

    add_executable(saccade_platform_windows_operating_system
        platform/windows/operating_system.cpp)
    target_include_directories(saccade_platform_windows_operating_system PRIVATE
        "${PROJECT_SOURCE_DIR}")
    target_link_libraries(saccade_platform_windows_operating_system PRIVATE
        saccade_platform_windows
        saccade_build_settings)
    add_test(NAME platform.windows.operating_system
        COMMAND saccade_platform_windows_operating_system)

    add_executable(saccade_platform_windows_runtime_scheduling
        platform/windows/runtime_scheduling.cpp)
    target_include_directories(saccade_platform_windows_runtime_scheduling PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(saccade_platform_windows_runtime_scheduling PRIVATE
        saccade_platform_windows
        saccade_build_settings)
    add_test(NAME platform.windows.runtime_scheduling
        COMMAND saccade_platform_windows_runtime_scheduling)

    add_executable(saccade_platform_windows_glyph_atlas
        platform/windows/glyph_atlas.cpp)
    target_include_directories(saccade_platform_windows_glyph_atlas PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_windows_glyph_atlas PRIVATE
        saccade_platform_windows
        saccade_core
        saccade_build_settings)
    add_test(NAME platform.windows.glyph_atlas
        COMMAND saccade_platform_windows_glyph_atlas)

    add_executable(saccade_platform_windows_accessibility_provider
        platform/windows/accessibility_provider.cpp)
    target_include_directories(saccade_platform_windows_accessibility_provider PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_windows_accessibility_provider PRIVATE
        saccade_platform_windows
        saccade_core
        saccade_build_settings
        user32)
    add_test(NAME platform.windows.accessibility_provider
        COMMAND saccade_platform_windows_accessibility_provider)
    set_tests_properties(platform.windows.accessibility_provider
        PROPERTIES LABELS "live;live-owned-window")

    add_executable(saccade_platform_windows_accessibility_provider_lifecycle
        platform/windows/accessibility_provider_lifecycle.cpp)
    target_include_directories(saccade_platform_windows_accessibility_provider_lifecycle PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_windows_accessibility_provider_lifecycle PRIVATE
        saccade_platform_windows
        saccade_core
        saccade_build_settings)
    add_test(NAME platform.windows.accessibility_provider_lifecycle
        COMMAND saccade_platform_windows_accessibility_provider_lifecycle)

    add_executable(saccade_platform_windows_accessibility_target_policy
        platform/windows/accessibility_target_policy.cpp)
    target_include_directories(saccade_platform_windows_accessibility_target_policy PRIVATE
        "${PROJECT_SOURCE_DIR}")
    target_link_libraries(saccade_platform_windows_accessibility_target_policy PRIVATE
        saccade_build_settings)
    add_test(NAME platform.windows.accessibility_target_policy
        COMMAND saccade_platform_windows_accessibility_target_policy)

    add_executable(saccade_platform_windows_input_executor
        platform/windows/input_executor.cpp)
    target_include_directories(saccade_platform_windows_input_executor PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_windows_input_executor PRIVATE
        saccade_platform_windows
        saccade_core
        saccade_build_settings
        user32)
    add_test(NAME platform.windows.input_executor
        COMMAND saccade_platform_windows_input_executor)

    add_executable(saccade_platform_windows_input_workflow_live
        platform/windows/input_workflow_live.cpp)
    target_include_directories(saccade_platform_windows_input_workflow_live PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_windows_input_workflow_live PRIVATE
        saccade_platform_windows
        saccade_core
        saccade_build_settings
        user32)
    add_test(NAME platform.windows.input_workflow.live
        COMMAND saccade_platform_windows_input_workflow_live)
    set_tests_properties(platform.windows.input_workflow.live PROPERTIES
        LABELS "live;live-workflow"
        SKIP_RETURN_CODE 77
        TIMEOUT 60)

    add_executable(saccade_platform_windows_screen_capture
        platform/windows/screen_capture.cpp)
    target_include_directories(saccade_platform_windows_screen_capture PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_windows_screen_capture PRIVATE
        saccade_platform_windows
        saccade_core
        saccade_build_settings
        d3d11
        user32)
    add_test(NAME platform.windows.screen_capture
        COMMAND saccade_platform_windows_screen_capture)
    set_tests_properties(platform.windows.screen_capture
        PROPERTIES LABELS "live;live-capture" SKIP_RETURN_CODE 77)

    add_executable(saccade_platform_windows_scene_capture
        platform/windows/scene_capture.cpp)
    target_include_directories(saccade_platform_windows_scene_capture PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_windows_scene_capture PRIVATE
        saccade_platform_windows
        saccade_core
        saccade_build_settings
        d3d11
        user32)
    add_test(NAME platform.windows.scene_capture
        COMMAND saccade_platform_windows_scene_capture)
    set_tests_properties(platform.windows.scene_capture
        PROPERTIES LABELS "live;live-capture" SKIP_RETURN_CODE 77)

    if(TARGET saccade_backend_d3d12)
        add_executable(saccade_platform_windows_overlay_surface
            platform/windows/overlay_surface.cpp)
        target_include_directories(saccade_platform_windows_overlay_surface PRIVATE
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(saccade_platform_windows_overlay_surface PRIVATE
            saccade_platform_windows
            saccade_backend_d3d12
            saccade_core
            saccade_build_settings
            d3d12
            user32)
        add_dependencies(saccade_platform_windows_overlay_surface
            saccade_d3d12_shaders)
        add_test(NAME platform.windows.overlay_surface
            COMMAND saccade_platform_windows_overlay_surface
                "${PROJECT_BINARY_DIR}/backends/d3d12")
        set_tests_properties(platform.windows.overlay_surface
            PROPERTIES LABELS "live;live-overlay" SKIP_RETURN_CODE 77)

        add_executable(saccade_platform_windows_overlay_surface_set
            platform/windows/overlay_surface_set.cpp)
        target_include_directories(
            saccade_platform_windows_overlay_surface_set PRIVATE
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(saccade_platform_windows_overlay_surface_set PRIVATE
            saccade_platform_windows
            saccade_backend_d3d12
            saccade_core
            saccade_build_settings
            d3d12
            user32)
        add_dependencies(saccade_platform_windows_overlay_surface_set
            saccade_d3d12_shaders)
        add_test(NAME platform.windows.overlay_surface_set
            COMMAND saccade_platform_windows_overlay_surface_set
                "${PROJECT_BINARY_DIR}/backends/d3d12")
        set_tests_properties(platform.windows.overlay_surface_set
            PROPERTIES LABELS "live;live-overlay" SKIP_RETURN_CODE 77)
    endif()
endif()

if(TARGET saccade_backend_metal)
    add_executable(saccade_kernels_metal_target_postprocess
        kernels/metal_target_postprocess.mm)
    target_include_directories(saccade_kernels_metal_target_postprocess PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_kernels_metal_target_postprocess PRIVATE
        saccade_backend_metal
        saccade_core
        saccade_build_settings
        "-framework Metal")
    target_compile_options(saccade_kernels_metal_target_postprocess PRIVATE
        -fobjc-arc)
    add_dependencies(saccade_kernels_metal_target_postprocess
        saccade_overlay_metallib)
    add_test(NAME kernels.metal_target_postprocess
        COMMAND saccade_kernels_metal_target_postprocess
            "${PROJECT_BINARY_DIR}/backends/metal/saccade_overlay.metallib")
    set_tests_properties(kernels.metal_target_postprocess
        PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(saccade_overlay_metal_preprocessor
        overlay/metal_preprocessor.mm)
    target_include_directories(saccade_overlay_metal_preprocessor PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_overlay_metal_preprocessor PRIVATE
        saccade_backend_metal
        saccade_build_settings
        "-framework Metal")
    target_compile_options(saccade_overlay_metal_preprocessor PRIVATE -fobjc-arc)
    add_dependencies(saccade_overlay_metal_preprocessor saccade_overlay_metallib)
    add_test(NAME overlay.metal_preprocessor
        COMMAND saccade_overlay_metal_preprocessor
            "${PROJECT_BINARY_DIR}/backends/metal/saccade_overlay.metallib")
    set_tests_properties(overlay.metal_preprocessor
        PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(saccade_platform_macos_screen_capture
        platform/macos/screen_capture.mm)
    target_include_directories(saccade_platform_macos_screen_capture PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_screen_capture PRIVATE
        saccade_platform_macos_capture
        saccade_build_settings
        "-framework CoreGraphics"
        "-framework Metal")
    target_compile_options(saccade_platform_macos_screen_capture PRIVATE
        -fobjc-arc)
    add_test(NAME platform.macos.screen_capture
        COMMAND saccade_platform_macos_screen_capture)
    set_tests_properties(platform.macos.screen_capture
        PROPERTIES LABELS "live;live-capture" SKIP_RETURN_CODE 77)

    add_executable(saccade_platform_macos_scene_capture
        platform/macos/scene_capture.mm)
    target_include_directories(saccade_platform_macos_scene_capture PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_platform_macos_scene_capture PRIVATE
        saccade_platform_macos_capture
        saccade_build_settings
        "-framework AppKit"
        "-framework CoreGraphics"
        "-framework Metal")
    target_compile_options(saccade_platform_macos_scene_capture PRIVATE
        -fobjc-arc)
    add_dependencies(saccade_platform_macos_scene_capture
        saccade_overlay_metallib)
    add_test(NAME platform.macos.scene_capture
        COMMAND saccade_platform_macos_scene_capture
            "${PROJECT_BINARY_DIR}/backends/metal/saccade_overlay.metallib")
    set_tests_properties(platform.macos.scene_capture
        PROPERTIES LABELS "live;live-capture" SKIP_RETURN_CODE 77)

    add_executable(saccade_overlay_metal_expander overlay/metal_expander.cpp)
    target_include_directories(saccade_overlay_metal_expander PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_overlay_metal_expander PRIVATE
        saccade_backend_metal
        saccade_core
        saccade_build_settings)
    add_dependencies(saccade_overlay_metal_expander saccade_overlay_metallib)
    add_test(
        NAME overlay.metal_expander
        COMMAND saccade_overlay_metal_expander
            "${PROJECT_BINARY_DIR}/backends/metal/saccade_overlay.metallib")
    set_tests_properties(overlay.metal_expander PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(saccade_overlay_metal_renderer overlay/metal_renderer.mm)
    target_include_directories(saccade_overlay_metal_renderer PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_overlay_metal_renderer PRIVATE
        saccade_backend_metal
        saccade_core
        saccade_build_settings
        "-framework Metal")
    target_compile_options(saccade_overlay_metal_renderer PRIVATE -fobjc-arc)
    add_dependencies(saccade_overlay_metal_renderer saccade_overlay_metallib)
    add_test(
        NAME overlay.metal_renderer
        COMMAND saccade_overlay_metal_renderer
            "${PROJECT_BINARY_DIR}/backends/metal/saccade_overlay.metallib")
    set_tests_properties(overlay.metal_renderer PROPERTIES SKIP_RETURN_CODE 77)

    if(TARGET saccade_platform_macos_overlay)
        add_executable(saccade_platform_macos_overlay_surface
            platform/macos/overlay_surface.mm)
        target_include_directories(saccade_platform_macos_overlay_surface PRIVATE
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(saccade_platform_macos_overlay_surface PRIVATE
            saccade_platform_macos_overlay
            saccade_build_settings
            "-framework AppKit")
        target_compile_options(saccade_platform_macos_overlay_surface PRIVATE
            -fobjc-arc)
        add_dependencies(saccade_platform_macos_overlay_surface
            saccade_overlay_metallib)
        add_test(
            NAME platform.macos.overlay_surface
            COMMAND saccade_platform_macos_overlay_surface
                "${PROJECT_BINARY_DIR}/backends/metal/saccade_overlay.metallib")
        set_tests_properties(platform.macos.overlay_surface
            PROPERTIES LABELS "live;live-overlay" SKIP_RETURN_CODE 77)
    endif()
endif()

if(TARGET saccade_backend_d3d12)
    add_executable(saccade_kernels_d3d12_target_postprocess
        kernels/d3d12_target_postprocess.cpp)
    target_include_directories(saccade_kernels_d3d12_target_postprocess PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_kernels_d3d12_target_postprocess PRIVATE
        saccade_backend_d3d12
        saccade_core
        saccade_build_settings
        d3d11
        d3d12)
    add_dependencies(saccade_kernels_d3d12_target_postprocess
        saccade_d3d12_shaders)
    add_test(NAME kernels.d3d12_target_postprocess
        COMMAND saccade_kernels_d3d12_target_postprocess
            "${PROJECT_BINARY_DIR}/backends/d3d12")
    set_tests_properties(kernels.d3d12_target_postprocess
        PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(saccade_overlay_d3d12_preprocessor
        overlay/d3d12_preprocessor.cpp)
    target_include_directories(saccade_overlay_d3d12_preprocessor PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_overlay_d3d12_preprocessor PRIVATE
        saccade_backend_d3d12
        saccade_core
        saccade_build_settings
        d3d11
        d3d12)
    add_dependencies(saccade_overlay_d3d12_preprocessor saccade_d3d12_shaders)
    add_test(NAME overlay.d3d12_preprocessor
        COMMAND saccade_overlay_d3d12_preprocessor
            "${PROJECT_BINARY_DIR}/backends/d3d12")
    set_tests_properties(overlay.d3d12_preprocessor
        PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(saccade_overlay_d3d12_renderer
        overlay/d3d12_renderer.cpp)
    target_include_directories(saccade_overlay_d3d12_renderer PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_overlay_d3d12_renderer PRIVATE
        saccade_backend_d3d12
        saccade_core
        saccade_build_settings
        d3d11
        d3d12)
    add_dependencies(saccade_overlay_d3d12_renderer saccade_d3d12_shaders)
    add_test(NAME overlay.d3d12_renderer
        COMMAND saccade_overlay_d3d12_renderer
            "${PROJECT_BINARY_DIR}/backends/d3d12")
    set_tests_properties(overlay.d3d12_renderer
        PROPERTIES SKIP_RETURN_CODE 77)

    if(SACCADE_WINDOWS_ML_AVAILABLE)
        set(saccade_windows_ml_runtime_files
            "${WINML_BINARY_DIR}/onnxruntime.dll"
            "${WINML_DIRECTML_DLL}")
        if(NOT "${WINML_ONNXRUNTIME_PROVIDERS_SHARED_DLL}" STREQUAL "")
            list(APPEND saccade_windows_ml_runtime_files
                "${WINML_ONNXRUNTIME_PROVIDERS_SHARED_DLL}")
        endif()

        add_executable(saccade_windows_d3d12_memory_soak
            "${PROJECT_SOURCE_DIR}/benchmarks/windows_d3d12_memory_soak.cpp")
        target_include_directories(saccade_windows_d3d12_memory_soak PRIVATE
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(saccade_windows_d3d12_memory_soak PRIVATE
            saccade_backend_d3d12
            saccade_build_settings
            d3d12
            dxgi
            psapi)
        add_dependencies(saccade_windows_d3d12_memory_soak
            saccade_d3d12_shaders)
        foreach(runtime_file IN LISTS saccade_windows_ml_runtime_files)
            add_custom_command(TARGET saccade_windows_d3d12_memory_soak POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_file}"
                    "$<TARGET_FILE_DIR:saccade_windows_d3d12_memory_soak>"
                VERBATIM)
        endforeach()
        if(EXISTS "${SACCADE_MODEL_ARTIFACT}")
            add_test(NAME windows.d3d12_memory_soak
                COMMAND saccade_windows_d3d12_memory_soak
                    "${SACCADE_MODEL_ARTIFACT}"
                    "${PROJECT_BINARY_DIR}/backends/d3d12"
                    --duration=3
                    --private-budget-mb=64
                    --handle-budget=8
                    --video-budget-mb=64
                    --saccade-budget-mb=64
                    --monotonic-samples=8
                    --source-width=3840
                    --source-height=2160)
            set_tests_properties(windows.d3d12_memory_soak
                PROPERTIES SKIP_RETURN_CODE 77)
        endif()

        add_executable(saccade_model_directml_inference
            model/directml_inference.cpp)
        target_include_directories(saccade_model_directml_inference PRIVATE
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(saccade_model_directml_inference PRIVATE
            saccade_backend_d3d12
            saccade_core
            saccade_build_settings
            d3d12)
        foreach(runtime_file IN LISTS saccade_windows_ml_runtime_files)
            add_custom_command(TARGET saccade_model_directml_inference POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_file}"
                    $<TARGET_FILE_DIR:saccade_model_directml_inference>
                VERBATIM)
        endforeach()
        add_test(NAME model.directml_inference
            COMMAND saccade_model_directml_inference)
        set_tests_properties(model.directml_inference
            PROPERTIES SKIP_RETURN_CODE 77)

        add_executable(saccade_model_directml_provider
            model/directml_provider.cpp)
        target_include_directories(saccade_model_directml_provider PRIVATE
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(saccade_model_directml_provider PRIVATE
            saccade_platform_windows
            saccade_backend_d3d12
            saccade_core
            saccade_build_settings
            d3d11
            d3d12)
        add_dependencies(saccade_model_directml_provider
            saccade_d3d12_shaders)
        foreach(runtime_file IN LISTS saccade_windows_ml_runtime_files)
            add_custom_command(TARGET saccade_model_directml_provider POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_file}"
                    $<TARGET_FILE_DIR:saccade_model_directml_provider>
                VERBATIM)
        endforeach()
        add_test(NAME model.directml_provider
            COMMAND saccade_model_directml_provider
                --headless
                "${PROJECT_BINARY_DIR}/backends/d3d12")
        set_tests_properties(model.directml_provider
            PROPERTIES SKIP_RETURN_CODE 77)

        add_test(NAME model.directml_provider.live
            COMMAND saccade_model_directml_provider
                --live
                "${PROJECT_BINARY_DIR}/backends/d3d12"
                "${PROJECT_BINARY_DIR}/tests/saccade-directml-pipeline-test.scmd")
        set_tests_properties(model.directml_provider.live
            PROPERTIES LABELS "live;live-capture" SKIP_RETURN_CODE 77)
    endif()
endif()

add_test(
    NAME build.target_architecture
    COMMAND "${CMAKE_COMMAND}"
        "-DMODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/target_architecture.cmake")

set(saccade_symbol_tool "")
set(saccade_symbol_tool_mode "")
if(CMAKE_NM AND EXISTS "${CMAKE_NM}")
    set(saccade_symbol_tool "${CMAKE_NM}")
    get_filename_component(saccade_symbol_tool_name "${CMAKE_NM}" NAME)
    if(saccade_symbol_tool_name MATCHES "dumpbin")
        set(saccade_symbol_tool_mode dumpbin)
    else()
        set(saccade_symbol_tool_mode nm)
    endif()
else()
    get_filename_component(saccade_compiler_directory
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(saccade_llvm_nm
        NAMES llvm-nm llvm-nm.exe
        HINTS "${saccade_compiler_directory}")
    if(saccade_llvm_nm)
        set(saccade_symbol_tool "${saccade_llvm_nm}")
        set(saccade_symbol_tool_mode nm)
    else()
        find_program(saccade_dumpbin
            NAMES dumpbin dumpbin.exe
            HINTS "${saccade_compiler_directory}")
        if(saccade_dumpbin)
            set(saccade_symbol_tool "${saccade_dumpbin}")
            set(saccade_symbol_tool_mode dumpbin)
        endif()
    endif()
endif()
if(NOT saccade_symbol_tool)
    message(FATAL_ERROR
        "Tests require nm, llvm-nm, or dumpbin for allocator symbol gates")
endif()
add_test(
    NAME core.no_allocator_symbols
    COMMAND "${CMAKE_COMMAND}"
        "-DSYMBOL_TOOL=${saccade_symbol_tool}"
        "-DSYMBOL_TOOL_MODE=${saccade_symbol_tool_mode}"
        "-DLIBRARY=$<TARGET_FILE:saccade_core>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/no_allocator_symbols.cmake")

add_executable(saccade_backend_registry backend/registry.cpp)
target_include_directories(saccade_backend_registry PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_backend_registry PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME backend.registry COMMAND saccade_backend_registry)

add_executable(saccade_backend_device_registry backend/device_registry.cpp)
target_include_directories(saccade_backend_device_registry PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_backend_device_registry PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME backend.device_registry COMMAND saccade_backend_device_registry)

if(TARGET saccade_backend_mock)
    add_executable(saccade_backend_mock_contract backend/mock_contract.cpp)
    target_include_directories(saccade_backend_mock_contract PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_backend_mock_contract PRIVATE
        saccade_backend_mock
        saccade_core
        saccade_build_settings)
    add_test(NAME backend.mock_contract COMMAND saccade_backend_mock_contract)
endif()

if(TARGET saccade_backend_reference_cpu)
    add_executable(saccade_backend_reference_cpu_contract
        backend/reference_cpu_contract.cpp)
    target_include_directories(saccade_backend_reference_cpu_contract PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(saccade_backend_reference_cpu_contract PRIVATE
        saccade_backend_reference_cpu
        saccade_core
        saccade_build_settings)
    add_test(
        NAME backend.reference_cpu_contract
        COMMAND saccade_backend_reference_cpu_contract)
endif()

add_executable(saccade_backend_registry_guard backend/registry_guard.cpp)
target_include_directories(saccade_backend_registry_guard PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_backend_registry_guard PRIVATE
    saccade_core
    saccade_build_settings)
add_test(NAME backend.registry_guard COMMAND saccade_backend_registry_guard)

add_executable(saccade_core_runtime core/runtime.cpp)
target_include_directories(saccade_core_runtime PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_core_runtime PRIVATE
    saccade_backend_reference_cpu
    saccade_core
    saccade_build_settings)
if(APPLE)
    add_executable(saccade_platform_macos_agent_socket
        platform/macos/agent_socket.mm)
    target_include_directories(saccade_platform_macos_agent_socket PRIVATE
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include")
    target_compile_options(saccade_platform_macos_agent_socket PRIVATE -fobjc-arc)
    target_link_libraries(saccade_platform_macos_agent_socket PRIVATE
        saccade_platform_macos
        saccade_build_settings)
    add_test(NAME platform.macos.agent_socket
        COMMAND saccade_platform_macos_agent_socket)

    target_link_libraries(saccade_core_runtime PRIVATE
        "-framework CoreVideo"
        "-framework IOSurface")
endif()
add_test(NAME core.runtime COMMAND saccade_core_runtime)

add_executable(saccade_core_runtime_static_destruction
    core/runtime_static_destruction.cpp)
target_include_directories(saccade_core_runtime_static_destruction PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_core_runtime_static_destruction PRIVATE
    saccade_core
    saccade_build_settings)
add_test(
    NAME core.runtime_static_destruction
    COMMAND saccade_core_runtime_static_destruction)

add_test(
    NAME package.shared_build
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/shared-build"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DC_COMPILER=${CMAKE_C_COMPILER}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DNM_TOOL=${CMAKE_NM}"
        "-DAPPLE_HOST=${APPLE}"
        "-DWIN32_HOST=${WIN32}"
        "-DSYMBOL_MANIFEST=${PROJECT_SOURCE_DIR}/abi/symbols-v1.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shared_build.cmake")

add_test(
    NAME package.distribution_rejects_inert
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/distribution-reject"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/distribution_reject.cmake")

set(install_consumer_arguments
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/install-consumer"
    "-DBINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/install-consumer"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DC_COMPILER=${CMAKE_C_COMPILER}"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DMETAL_BACKEND=${SACCADE_BACKEND_METAL}")

add_test(
    NAME package.install.static
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=static
        ${install_consumer_arguments}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_consumer.cmake")

add_test(
    NAME package.install.shared
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=shared
        ${install_consumer_arguments}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_consumer.cmake")

add_test(
    NAME package.install.both
    COMMAND "${CMAKE_COMMAND}"
        -DMODE=both
        ${install_consumer_arguments}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_consumer.cmake")

add_test(
    NAME quality.repository
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/scripts/check_repository.cmake")

add_test(
    NAME quality.text
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/scripts/check_text.cmake")

add_test(
    NAME quality.documentation
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/scripts/check_documentation.cmake")
