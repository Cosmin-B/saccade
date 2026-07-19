configure_file(tool_version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/saccade_tool_version.h" @ONLY)

add_library(saccade_agent_client STATIC agent_client.cpp)
target_include_directories(saccade_agent_client PRIVATE
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(saccade_agent_client PRIVATE saccade_build_settings)

add_executable(saccade_cli agent_cli.cpp)
target_include_directories(saccade_cli PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(saccade_cli PRIVATE saccade_agent_client saccade_build_settings)
if(WIN32)
    set_target_properties(saccade_cli PROPERTIES OUTPUT_NAME saccade-cli)
else()
    set_target_properties(saccade_cli PROPERTIES OUTPUT_NAME saccade)
endif()

add_executable(saccade_mcp mcp_server.cpp)
target_include_directories(saccade_mcp PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(saccade_mcp SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/jsmn")
target_link_libraries(saccade_mcp PRIVATE saccade_agent_client saccade_build_settings)
set_target_properties(saccade_mcp PROPERTIES OUTPUT_NAME saccade-mcp)

if(SACCADE_BUILD_TESTS)
    add_test(
        NAME tools.mcp_stdio
        COMMAND "${CMAKE_COMMAND}"
            "-DMCP_EXECUTABLE=$<TARGET_FILE:saccade_mcp>"
            "-DINPUT_FILE=${PROJECT_SOURCE_DIR}/tests/fixtures/mcp-stdio.jsonl"
            -P "${PROJECT_SOURCE_DIR}/tests/cmake/mcp_stdio.cmake")
endif()

install(TARGETS saccade_cli saccade_mcp
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Tools)
if(WIN32)
    install(TARGETS saccade_cli saccade_mcp
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        COMPONENT Application)
endif()
if(APPLE AND NOT "${SACCADE_MACOS_CODESIGN_IDENTITY}" STREQUAL "")
    find_program(_saccade_tools_codesign codesign REQUIRED)
    foreach(_saccade_tool IN ITEMS saccade_cli saccade_mcp)
        add_custom_command(TARGET ${_saccade_tool} POST_BUILD
            COMMAND "${_saccade_tools_codesign}" --force
                --sign "${SACCADE_MACOS_CODESIGN_IDENTITY}"
                "$<TARGET_FILE:${_saccade_tool}>"
            COMMAND "${_saccade_tools_codesign}" --verify --strict --verbose=2
                "$<TARGET_FILE:${_saccade_tool}>"
            VERBATIM)
    endforeach()
endif()
if(WIN32 AND SACCADE_DISTRIBUTION_BUILD)
    foreach(_saccade_tool IN ITEMS saccade_cli saccade_mcp)
        add_custom_command(TARGET ${_saccade_tool} POST_BUILD
            COMMAND "${SACCADE_WINDOWS_SIGNTOOL}" sign
                /sha1 "${SACCADE_WINDOWS_SIGNING_CERTIFICATE_SHA1}"
                /fd SHA256
                /tr "${SACCADE_WINDOWS_TIMESTAMP_URL}"
                /td SHA256
                "$<TARGET_FILE:${_saccade_tool}>"
            COMMAND "${SACCADE_WINDOWS_SIGNTOOL}" verify
                /pa
                /all
                "$<TARGET_FILE:${_saccade_tool}>"
            VERBATIM)
    endforeach()
endif()

if(WIN32 AND NOT SACCADE_WINDOWS_ML_AVAILABLE)
    return()
endif()

add_executable(saccade_runtime_probe runtime_probe.cpp)
target_include_directories(saccade_runtime_probe PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_runtime_probe PRIVATE
    saccade_build_settings)
if(APPLE)
    target_link_libraries(saccade_runtime_probe PRIVATE
        saccade_platform_macos)
elseif(WIN32)
    target_link_libraries(saccade_runtime_probe PRIVATE
        saccade_platform_windows)
    if(SACCADE_WINDOWS_ML_AVAILABLE)
        foreach(runtime_file IN ITEMS
                "${WINML_BINARY_DIR}/onnxruntime.dll"
                "${WINML_DIRECTML_DLL}"
                "${WINML_ONNXRUNTIME_PROVIDERS_SHARED_DLL}")
            add_custom_command(TARGET saccade_runtime_probe POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${runtime_file}" "$<TARGET_FILE_DIR:saccade_runtime_probe>"
                VERBATIM)
        endforeach()
    endif()
endif()
if(SACCADE_MODEL_PUBLIC_KEY_XY)
    target_compile_definitions(saccade_runtime_probe PRIVATE
        SACCADE_MODEL_PUBLIC_KEY_XY="${SACCADE_MODEL_PUBLIC_KEY_XY}")
endif()
