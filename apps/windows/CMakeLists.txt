add_executable(saccade_app WIN32
    main.cpp
    app.rc)
if(SACCADE_WINDOWS_UIACCESS)
    set(SACCADE_WINDOWS_UIACCESS_VALUE true)
else()
    set(SACCADE_WINDOWS_UIACCESS_VALUE false)
endif()
configure_file(app.manifest.in saccade.manifest @ONLY)
target_link_options(saccade_app PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/saccade.manifest")
target_include_directories(saccade_app PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_app PRIVATE
    saccade_build_settings
    saccade_platform_windows
    comdlg32
    shell32
    user32
    wtsapi32)
add_dependencies(saccade_app saccade_d3d12_shaders)
if(SACCADE_DISTRIBUTION_BUILD AND (NOT TARGET saccade_cli OR NOT TARGET saccade_mcp))
    message(FATAL_ERROR "A Windows distribution build requires the CLI and MCP helper targets")
endif()
if(TARGET saccade_cli AND TARGET saccade_mcp)
    add_dependencies(saccade_app saccade_cli saccade_mcp)
    set_property(TARGET saccade_app APPEND PROPERTY LINK_DEPENDS
        "$<TARGET_FILE:saccade_cli>"
        "$<TARGET_FILE:saccade_mcp>")
    foreach(tool IN ITEMS saccade_cli saccade_mcp)
        add_custom_command(TARGET saccade_app POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${tool}>" "$<TARGET_FILE_DIR:saccade_app>"
            VERBATIM)
    endforeach()
endif()
set(_saccade_windows_shader_files)
foreach(shader IN ITEMS
        preprocess_fp16 preprocess_int8
        targets_pack_normalized
        targets_prepare targets_radix_histogram targets_radix_scan
        targets_radix_scatter targets_suppression_masks targets_finalize
        expand_static update_active overlay_vertex overlay_pixel)
    add_custom_command(TARGET saccade_app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_BINARY_DIR}/backends/d3d12/${shader}.dxil"
            "$<TARGET_FILE_DIR:saccade_app>/${shader}.dxil"
        VERBATIM)
    list(APPEND _saccade_windows_shader_files
        "${PROJECT_BINARY_DIR}/backends/d3d12/${shader}.dxil")
endforeach()
set(_saccade_windows_runtime_files)
if(SACCADE_WINDOWS_ML_AVAILABLE)
    list(APPEND _saccade_windows_runtime_files
        "${WINML_BINARY_DIR}/onnxruntime.dll"
        "${WINML_DIRECTML_DLL}")
    foreach(runtime_file IN LISTS _saccade_windows_runtime_files)
        add_custom_command(TARGET saccade_app POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime_file}" "$<TARGET_FILE_DIR:saccade_app>"
            VERBATIM)
    endforeach()
endif()
if(SACCADE_MODEL_ARTIFACT)
    if(NOT EXISTS "${SACCADE_MODEL_ARTIFACT}")
        message(FATAL_ERROR "SACCADE_MODEL_ARTIFACT does not exist")
    endif()
    set_property(TARGET saccade_app APPEND PROPERTY LINK_DEPENDS
        "${SACCADE_MODEL_ARTIFACT}")
    add_custom_command(TARGET saccade_app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SACCADE_MODEL_ARTIFACT}"
            "$<TARGET_FILE_DIR:saccade_app>/saccade.model"
        VERBATIM)
endif()
if(SACCADE_MODEL_ARTIFACT AND TARGET saccade_runtime_probe)
    add_dependencies(saccade_app saccade_runtime_probe)
    add_custom_command(TARGET saccade_app PRE_LINK
        COMMAND "$<TARGET_FILE:saccade_runtime_probe>"
            "${SACCADE_MODEL_ARTIFACT}"
            "${PROJECT_BINARY_DIR}/backends/d3d12"
        COMMENT "Validating the signed DirectML runtime contract"
        VERBATIM)
endif()
if(SACCADE_MODEL_PUBLIC_KEY_XY)
    target_compile_definitions(saccade_app PRIVATE
        SACCADE_MODEL_PUBLIC_KEY_XY="${SACCADE_MODEL_PUBLIC_KEY_XY}")
endif()
if(SACCADE_DISTRIBUTION_BUILD)
    add_custom_command(TARGET saccade_app POST_BUILD
        COMMAND "${SACCADE_WINDOWS_SIGNTOOL}" sign
            /sha1 "${SACCADE_WINDOWS_SIGNING_CERTIFICATE_SHA1}"
            /fd SHA256
            /tr "${SACCADE_WINDOWS_TIMESTAMP_URL}"
            /td SHA256
            "$<TARGET_FILE:saccade_app>"
        COMMAND "${SACCADE_WINDOWS_SIGNTOOL}" verify
            /pa
            /all
            "$<TARGET_FILE:saccade_app>"
        VERBATIM)
endif()
set_target_properties(saccade_app PROPERTIES OUTPUT_NAME Saccade)
install(TARGETS saccade_app
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Application)
install(FILES
    ${_saccade_windows_runtime_files}
    ${_saccade_windows_shader_files}
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Application)
if(SACCADE_MODEL_ARTIFACT)
    install(FILES "${SACCADE_MODEL_ARTIFACT}"
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME saccade.model
        COMPONENT Application)
endif()
