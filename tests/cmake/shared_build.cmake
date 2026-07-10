if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR OR NOT DEFINED GENERATOR OR
   NOT DEFINED SYMBOL_MANIFEST)
    message(FATAL_ERROR
        "SOURCE_DIR, BINARY_DIR, GENERATOR, and SYMBOL_MANIFEST are required")
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
    file(GLOB_RECURSE shared_candidates "${BINARY_DIR}/*saccade_core.dll")
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

if(WIN32_HOST)
    find_program(export_inspector NAMES dumpbin dumpbin.exe)
    if(export_inspector)
        execute_process(
            COMMAND "${export_inspector}" /nologo /exports "${shared_library}"
            RESULT_VARIABLE inspect_result
            OUTPUT_VARIABLE exports
            ERROR_VARIABLE inspect_error)
    else()
        unset(export_inspector CACHE)
        find_program(export_inspector NAMES llvm-readobj llvm-readobj.exe)
        if(NOT export_inspector)
            message(FATAL_ERROR "dumpbin or llvm-readobj is required to inspect DLL exports")
        endif()
        execute_process(
            COMMAND "${export_inspector}" --coff-exports "${shared_library}"
            RESULT_VARIABLE inspect_result
            OUTPUT_VARIABLE exports
            ERROR_VARIABLE inspect_error)
    endif()
else()
    file(GLOB_RECURSE static_candidates "${BINARY_DIR}/libsaccade_core.a")
    if(static_candidates)
        message(FATAL_ERROR "shared-only build also produced a static library")
    endif()

    if(NOT DEFINED NM_TOOL OR NM_TOOL STREQUAL "")
        message(FATAL_ERROR "NM_TOOL is required to inspect shared exports")
    endif()
    if(APPLE_HOST)
        execute_process(
            COMMAND "${NM_TOOL}" -gU "${shared_library}"
            RESULT_VARIABLE inspect_result
            OUTPUT_VARIABLE exports
            ERROR_VARIABLE inspect_error)
    else()
        execute_process(
            COMMAND "${NM_TOOL}" -D --defined-only "${shared_library}"
            RESULT_VARIABLE inspect_result
            OUTPUT_VARIABLE exports
            ERROR_VARIABLE inspect_error)
    endif()
endif()

if(NOT inspect_result EQUAL 0)
    message(FATAL_ERROR "could not inspect shared exports: ${inspect_error}")
endif()

file(STRINGS "${SYMBOL_MANIFEST}" public_symbols REGEX "^saccade_[a-z0-9_]+$")
foreach(symbol IN LISTS public_symbols)
    if(NOT exports MATCHES "(^|[ \t_])${symbol}([ \t\r\n]|$)")
        message(FATAL_ERROR "shared library does not export ${symbol}")
    endif()
endforeach()

if(NOT WIN32_HOST)
    if(exports MATCHES "_ZN7saccade|__ZN7saccade")
        message(FATAL_ERROR "shared library exports private C++ symbols")
    endif()
endif()
