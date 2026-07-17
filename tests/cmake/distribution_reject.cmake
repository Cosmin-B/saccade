if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR OR NOT DEFINED GENERATOR)
    message(FATAL_ERROR "SOURCE_DIR, BINARY_DIR, and GENERATOR are required")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${SOURCE_DIR}"
        -B "${BINARY_DIR}"
        -G "${GENERATOR}"
        -DSACCADE_DISTRIBUTION_BUILD=ON
        -DSACCADE_MODEL_ARTIFACT=
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

if(result EQUAL 0)
    message(FATAL_ERROR "An inert distribution configuration was accepted")
endif()

set(log "${output}${error}")
if(NOT log MATCHES "distribution build requires an existing SACCADE_MODEL_ARTIFACT")
    message(FATAL_ERROR "Distribution configuration failed for the wrong reason: ${log}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${SOURCE_DIR}"
        -B "${BINARY_DIR}-without-tools"
        -G "${GENERATOR}"
        -DSACCADE_DISTRIBUTION_BUILD=ON
        -DSACCADE_BUILD_TOOLS=OFF
    RESULT_VARIABLE tools_result
    OUTPUT_VARIABLE tools_output
    ERROR_VARIABLE tools_error)

if(tools_result EQUAL 0)
    message(FATAL_ERROR "A distribution configuration without agent tools was accepted")
endif()
set(tools_log "${tools_output}${tools_error}")
if(NOT tools_log MATCHES "distribution build requires SACCADE_BUILD_TOOLS=ON")
    message(FATAL_ERROR "Tool-less distribution configuration failed for the wrong reason: ${tools_log}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${SOURCE_DIR}"
        -B "${BINARY_DIR}-without-static"
        -G "${GENERATOR}"
        -DSACCADE_DISTRIBUTION_BUILD=ON
        -DSACCADE_BUILD_APPLICATION=ON
        -DSACCADE_BUILD_STATIC=OFF
    RESULT_VARIABLE static_result
    OUTPUT_VARIABLE static_output
    ERROR_VARIABLE static_error)

if(static_result EQUAL 0)
    message(FATAL_ERROR "A distribution configuration without static runtime support was accepted")
endif()
set(static_log "${static_output}${static_error}")
if(NOT static_log MATCHES "distribution build requires SACCADE_BUILD_STATIC=ON")
    message(FATAL_ERROR "Non-static distribution configuration failed for the wrong reason: ${static_log}")
endif()
