if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB_RECURSE documents LIST_DIRECTORIES false "${SOURCE_DIR}/*.md")
foreach(path IN LISTS documents)
    if(path MATCHES "/build/")
        continue()
    endif()
    file(READ "${path}" content)
    foreach(pattern IN ITEMS
            "TODO"
            "FIXME"
            "TBD"
            "Lorem ipsum"
            "As an AI"
            "In conclusion")
        string(FIND "${content}" "${pattern}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR "Unfinished or generated prose in ${path}: ${pattern}")
        endif()
    endforeach()

    get_filename_component(document_dir "${path}" DIRECTORY)
    set(remaining "${content}")
    while(remaining MATCHES "\\]\\(([^\n)]+)\\)")
        set(link "${CMAKE_MATCH_0}")
        set(target "${CMAKE_MATCH_1}")
        string(REGEX REPLACE "#.*$" "" target "${target}")
        if(target STREQUAL "" OR target MATCHES "^[a-zA-Z][a-zA-Z0-9+.-]*:")
            set(target "")
        else()
            if(IS_ABSOLUTE "${target}" OR target MATCHES "(^|/)\.\.(/|$)")
                message(FATAL_ERROR
                    "Documentation link leaves the repository: ${path}: ${target}")
            endif()
            cmake_path(ABSOLUTE_PATH target BASE_DIRECTORY "${document_dir}" NORMALIZE)
            if(NOT EXISTS "${target}")
                message(FATAL_ERROR "Broken documentation link in ${path}: ${link}")
            endif()
        endif()
        string(FIND "${remaining}" "${link}" link_position)
        string(LENGTH "${link}" link_length)
        math(EXPR next_position "${link_position} + ${link_length}")
        string(SUBSTRING "${remaining}" ${next_position} -1 remaining)
    endwhile()
endforeach()
