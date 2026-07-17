if(NOT DEFINED MCP_EXECUTABLE OR NOT DEFINED INPUT_FILE)
    message(FATAL_ERROR "MCP_EXECUTABLE and INPUT_FILE are required")
endif()

execute_process(
    COMMAND "${MCP_EXECUTABLE}"
    INPUT_FILE "${INPUT_FILE}"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    RESULT_VARIABLE result)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "MCP server exited with ${result}: ${error}")
endif()

string(REGEX MATCHALL "[^\n]+" lines "${output}")
list(LENGTH lines line_count)
if(NOT line_count EQUAL 11)
    message(FATAL_ERROR "Expected eleven MCP responses, received ${line_count}: ${output}")
endif()

foreach(expected IN ITEMS
        "\\\"id\\\":1"
        "\\\"protocolVersion\\\":\\\"2025-06-18\\\""
        "\\\"name\\\":\\\"saccade_observe\\\""
        "\\\"name\\\":\\\"saccade_query\\\""
        "\\\"name\\\":\\\"saccade_act\\\""
        "\\\"hover\\\""
        "\\\"key-chord\\\""
        "\\\"invoke\\\""
        "\\\"secondaryXQ8\\\""
        "\\\"active-window\\\""
        "\\\"sourceMode\\\""
        "\\\"afterGeneration\\\""
        "\\\"verifyNextGeneration\\\""
        "\\\"processId\\\""
        "\\\"physicalSequence\\\""
        "\\\"maximum\\\":9007199254740991"
        "\\\"dryRun\\\""
        "\\\"id\\\":3,\\\"result\\\":\\{\\}"
        "\\\"id\\\":4,\\\"error\\\":\\{\\\"code\\\":-32602,\\\"message\\\":\\\"Invalid observation scope\\\""
        "\\\"id\\\":5,\\\"error\\\":\\{\\\"code\\\":-32602,\\\"message\\\":\\\"Action points require coordinate pairs\\\""
        "\\\"id\\\":6,\\\"error\\\":\\{\\\"code\\\":-32602,\\\"message\\\":\\\"Invalid text\\\""
        "\\\"id\\\":7,\\\"error\\\":\\{\\\"code\\\":-32602,\\\"message\\\":\\\"Invalid action integer\\\""
        "\\\"id\\\":8,\\\"error\\\":\\{\\\"code\\\":-32602,\\\"message\\\":\\\"Physical expectations require physicalSequence\\\""
        "\\\"id\\\":9,\\\"error\\\":\\{\\\"code\\\":-32602,\\\"message\\\":\\\"Invalid action integer\\\""
        "\\\"id\\\":10,\\\"error\\\":\\{\\\"code\\\":-32602,\\\"message\\\":\\\"Invalid afterGeneration\\\""
        "\\\"id\\\":11,\\\"error\\\":\\{\\\"code\\\":-32602,\\\"message\\\":\\\"Invalid verifyNextGeneration value\\\"")
    if(NOT output MATCHES "${expected}")
        message(FATAL_ERROR "Missing MCP output ${expected}: ${output}")
    endif()
endforeach()
