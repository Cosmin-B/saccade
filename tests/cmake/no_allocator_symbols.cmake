if(NOT DEFINED SYMBOL_TOOL OR NOT DEFINED SYMBOL_TOOL_MODE OR
   NOT DEFINED LIBRARY)
    message(FATAL_ERROR "SYMBOL_TOOL, SYMBOL_TOOL_MODE, and LIBRARY are required")
endif()

if(SYMBOL_TOOL_MODE STREQUAL "nm")
    set(symbol_arguments -u "${LIBRARY}")
elseif(SYMBOL_TOOL_MODE STREQUAL "dumpbin")
    set(symbol_arguments /symbols "${LIBRARY}")
else()
    message(FATAL_ERROR "unsupported symbol tool mode: ${SYMBOL_TOOL_MODE}")
endif()

execute_process(
    COMMAND "${SYMBOL_TOOL}" ${symbol_arguments}
    RESULT_VARIABLE symbol_result
    OUTPUT_VARIABLE symbol_output
    ERROR_VARIABLE symbol_error)
if(NOT symbol_result EQUAL 0)
    message(FATAL_ERROR "failed to inspect ${LIBRARY}: ${symbol_error}")
endif()

string(REPLACE "\r\n" "\n" symbol_output "${symbol_output}")
string(REPLACE "\n" ";" undefined_symbol_lines "${symbol_output}")
foreach(symbol_line IN LISTS undefined_symbol_lines)
    if(SYMBOL_TOOL_MODE STREQUAL "dumpbin" AND
       NOT symbol_line MATCHES "UNDEF")
        continue()
    endif()
    if(symbol_line MATCHES "asan_stack_malloc")
        continue()
    endif()
    if(symbol_line MATCHES
       "malloc|calloc|realloc|aligned_alloc|posix_memalign|_Znw|_Zna|[?][?]2@|[?][?]_U@")
        message(FATAL_ERROR
            "core library references a process allocator: ${symbol_line}")
    endif()
endforeach()
