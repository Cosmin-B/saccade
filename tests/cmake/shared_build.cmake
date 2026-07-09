if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR OR NOT DEFINED GENERATOR)
    message(FATAL_ERROR "SOURCE_DIR, BINARY_DIR, and GENERATOR are required")
endif()

file(REMOVE_RECURSE "${BINARY_DIR}")

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${SOURCE_DIR}"
    -B "${BINARY_DIR}"
    -G "${GENERATOR}"
    -DSACCADE_BUILD_STATIC=OFF
    -DSACCADE_BUILD_SHARED=ON
    -DSACCADE_BUILD_TESTS=OFF
    -DSACCADE_BUILD_TOOLS=OFF
    -DSACCADE_BUILD_BENCHMARKS=OFF)

if(DEFINED C_COMPILER AND NOT C_COMPILER STREQUAL "")
    list(APPEND configure_command "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
if(DEFINED CXX_COMPILER AND NOT CXX_COMPILER STREQUAL "")
    list(APPEND configure_command "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "shared-only configure failed:\n${configure_output}\n${configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}" --target saccade_core
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "shared-only build failed:\n${build_output}\n${build_error}")
endif()

if(WIN32_HOST)
    file(GLOB_RECURSE shared_candidates "${BINARY_DIR}/saccade_core.dll")
elseif(APPLE_HOST)
    file(GLOB_RECURSE shared_candidates "${BINARY_DIR}/libsaccade_core.dylib")
else()
    file(GLOB_RECURSE shared_candidates "${BINARY_DIR}/libsaccade_core.so")
endif()

list(LENGTH shared_candidates shared_count)
if(NOT shared_count EQUAL 1)
    message(FATAL_ERROR
        "shared-only build produced ${shared_count} shared libraries: ${shared_candidates}")
endif()
list(GET shared_candidates 0 shared_library)

if(NOT WIN32_HOST)
    file(GLOB_RECURSE static_candidates "${BINARY_DIR}/libsaccade_core.a")
    if(static_candidates)
        message(FATAL_ERROR "shared-only build also produced a static library")
    endif()

    if(APPLE_HOST)
        execute_process(
            COMMAND "${NM_TOOL}" -gU "${shared_library}"
            RESULT_VARIABLE nm_result
            OUTPUT_VARIABLE exports
            ERROR_VARIABLE nm_error)
    else()
        execute_process(
            COMMAND "${NM_TOOL}" -D --defined-only "${shared_library}"
            RESULT_VARIABLE nm_result
            OUTPUT_VARIABLE exports
            ERROR_VARIABLE nm_error)
    endif()
    if(NOT nm_result EQUAL 0)
        message(FATAL_ERROR "could not inspect shared exports: ${nm_error}")
    endif()

    foreach(symbol IN ITEMS
        saccade_api_version
        saccade_last_error
        saccade_runtime_create
        saccade_runtime_freeze
        saccade_runtime_destroy
        saccade_frame_import_host
        saccade_frame_import_iosurface
        saccade_frame_import_d3d11
        saccade_register_inference_provider
        saccade_register_capture_provider
        saccade_register_overlay_provider
        saccade_register_accessibility_provider
        saccade_register_input_provider)
        if(NOT exports MATCHES "(^|[ \t_])${symbol}([ \t\r\n]|$)")
            message(FATAL_ERROR "shared library does not export ${symbol}")
        endif()
    endforeach()

    if(exports MATCHES "_ZN7saccade|__ZN7saccade")
        message(FATAL_ERROR "shared library exports private C++ symbols")
    endif()
endif()
