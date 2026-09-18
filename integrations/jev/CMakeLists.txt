find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

configure_file("${PROJECT_SOURCE_DIR}/tools/tool_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/saccade_tool_version.h" @ONLY)
add_executable(saccade_jev_native_fixture
    "${PROJECT_SOURCE_DIR}/tools/mcp_server.cpp"
    "${PROJECT_SOURCE_DIR}/tests/agent/jev_native_fixture.cpp")
target_include_directories(saccade_jev_native_fixture PRIVATE
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tools" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(saccade_jev_native_fixture SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/jsmn")
target_link_libraries(saccade_jev_native_fixture PRIVATE saccade_core saccade_build_settings)
set_target_properties(saccade_jev_native_fixture PROPERTIES
    OUTPUT_NAME saccade-jev-native-fixture
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools")

set(_saccade_jev_mcp "")
if(TARGET saccade_mcp)
    set(_saccade_jev_mcp "$<TARGET_FILE:saccade_mcp>")
endif()
add_test(
    NAME integrations.jev
    COMMAND "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}"
        "PYTHONDONTWRITEBYTECODE=1"
        "SACCADE_MCP_TEST_EXECUTABLE=${_saccade_jev_mcp}"
        "SACCADE_JEV_NATIVE_FIXTURE=$<TARGET_FILE:saccade_jev_native_fixture>"
        "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -v)
set_tests_properties(integrations.jev PROPERTIES TIMEOUT 30)
