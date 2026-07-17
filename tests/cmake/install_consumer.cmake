foreach(required IN ITEMS
        SOURCE_DIR CONSUMER_SOURCE_DIR BINARY_ROOT GENERATOR C_COMPILER CXX_COMPILER
        METAL_BACKEND MODE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT MODE STREQUAL "static" AND NOT MODE STREQUAL "shared" AND
   NOT MODE STREQUAL "both")
    message(FATAL_ERROR "MODE must be static, shared, or both")
endif()

set(package_build "${BINARY_ROOT}/${MODE}/package")
set(install_root "${BINARY_ROOT}/${MODE}/prefix")
set(consumer_build "${BINARY_ROOT}/${MODE}/consumer")
file(REMOVE_RECURSE "${BINARY_ROOT}/${MODE}")

if(MODE STREQUAL "shared")
    set(build_static OFF)
    set(build_shared ON)
    set(use_shared ON)
    set(core_is_shared ON)
elseif(MODE STREQUAL "both")
    set(build_static ON)
    set(build_shared ON)
    set(use_shared ON)
    set(core_is_shared OFF)
else()
    set(build_static ON)
    set(build_shared OFF)
    set(use_shared OFF)
    set(core_is_shared OFF)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${SOURCE_DIR}"
        -B "${package_build}"
        -G "${GENERATOR}"
        "-DCMAKE_C_COMPILER=${C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_INSTALL_PREFIX=${install_root}"
        -DCMAKE_INSTALL_INCLUDEDIR=custom/include
        "-DSACCADE_BUILD_STATIC=${build_static}"
        "-DSACCADE_BUILD_SHARED=${build_shared}"
        -DSACCADE_BUILD_TESTS=OFF
        -DSACCADE_BUILD_BENCHMARKS=OFF
        -DSACCADE_BUILD_TOOLS=OFF
        -DSACCADE_BACKEND_MOCK=OFF
        -DSACCADE_BACKEND_REFERENCE_CPU=OFF
        "-DSACCADE_BACKEND_METAL=${METAL_BACKEND}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "package configure failed:\n${configure_output}\n${configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${package_build}" --config Release --target install
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "package install failed:\n${install_output}\n${install_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${CONSUMER_SOURCE_DIR}"
        -B "${consumer_build}"
        -G "${GENERATOR}"
        "-DCMAKE_C_COMPILER=${C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_PREFIX_PATH=${install_root}"
        "-DSACCADE_USE_SHARED=${use_shared}"
        "-DSACCADE_CORE_IS_SHARED=${core_is_shared}"
    RESULT_VARIABLE consumer_configure_result
    OUTPUT_VARIABLE consumer_configure_output
    ERROR_VARIABLE consumer_configure_error)
if(NOT consumer_configure_result EQUAL 0)
    message(FATAL_ERROR
        "consumer configure failed:\n"
        "${consumer_configure_output}\n${consumer_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config Release
    RESULT_VARIABLE consumer_build_result
    OUTPUT_VARIABLE consumer_build_output
    ERROR_VARIABLE consumer_build_error)
if(NOT consumer_build_result EQUAL 0)
    message(FATAL_ERROR
        "consumer build failed:\n${consumer_build_output}\n${consumer_build_error}")
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${consumer_build}"
        -C Release --output-on-failure
    RESULT_VARIABLE consumer_test_result
    OUTPUT_VARIABLE consumer_test_output
    ERROR_VARIABLE consumer_test_error)
if(NOT consumer_test_result EQUAL 0)
    message(FATAL_ERROR
        "consumer tests failed:\n${consumer_test_output}\n${consumer_test_error}")
endif()
