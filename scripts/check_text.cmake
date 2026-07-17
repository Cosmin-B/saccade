if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB_RECURSE candidates LIST_DIRECTORIES false
    "${SOURCE_DIR}/*.c"
    "${SOURCE_DIR}/*.cc"
    "${SOURCE_DIR}/*.cmake"
    "${SOURCE_DIR}/*.cpp"
    "${SOURCE_DIR}/*.def"
    "${SOURCE_DIR}/*.h"
    "${SOURCE_DIR}/*.hpp"
    "${SOURCE_DIR}/*.in"
    "${SOURCE_DIR}/*.json"
    "${SOURCE_DIR}/*.md"
    "${SOURCE_DIR}/*.metal"
    "${SOURCE_DIR}/*.mm"
    "${SOURCE_DIR}/*.txt"
    "${SOURCE_DIR}/CMakeLists.txt")

foreach(path IN LISTS candidates)
    if(path MATCHES "/build(-[^/]+)?/" OR path MATCHES "/third_party/")
        continue()
    endif()
    file(READ "${path}" content)
    if(content MATCHES "\r")
        message(FATAL_ERROR "File uses CR or CRLF line endings: ${path}")
    endif()
    if(content MATCHES "[ \t]+\n" OR content MATCHES "[ \t]+$")
        message(FATAL_ERROR "File has trailing whitespace: ${path}")
    endif()
    string(FIND "${content}" "\t" tab_position)
    if(NOT tab_position EQUAL -1)
        message(FATAL_ERROR "File contains a tab: ${path}")
    endif()
    string(LENGTH "${content}" length)
    if(length GREATER 0)
        math(EXPR last_index "${length} - 1")
        string(SUBSTRING "${content}" ${last_index} 1 last_character)
        if(NOT last_character STREQUAL "\n")
            message(FATAL_ERROR "File has no final newline: ${path}")
        endif()
    endif()
endforeach()
