set(SACCADE_SANITIZERS "none" CACHE STRING "Sanitizers to enable")
set_property(CACHE SACCADE_SANITIZERS PROPERTY STRINGS none address undefined thread)

add_library(saccade_sanitizers INTERFACE)

if(NOT SACCADE_SANITIZERS STREQUAL "none")
    set(_saccade_sanitizers ${SACCADE_SANITIZERS})
    set(_saccade_supported_sanitizers address undefined thread)
    list(FIND _saccade_sanitizers thread _saccade_thread_index)
    list(LENGTH _saccade_sanitizers _saccade_sanitizer_count)

    if(_saccade_thread_index GREATER -1 AND _saccade_sanitizer_count GREATER 1)
        message(FATAL_ERROR "ThreadSanitizer cannot be combined with other sanitizers")
    endif()

    foreach(_saccade_sanitizer IN LISTS _saccade_sanitizers)
        if(NOT _saccade_sanitizer IN_LIST _saccade_supported_sanitizers)
            message(FATAL_ERROR "Unsupported sanitizer: ${_saccade_sanitizer}")
        endif()
    endforeach()

    if(MSVC)
        if(NOT SACCADE_SANITIZERS STREQUAL "address")
            message(FATAL_ERROR "MSVC supports only the address sanitizer")
        endif()
        target_compile_options(saccade_sanitizers INTERFACE /fsanitize=address)
        target_link_options(saccade_sanitizers INTERFACE /fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        string(JOIN "," _saccade_sanitizer_flags ${_saccade_sanitizers})
        target_compile_options(saccade_sanitizers INTERFACE
            "-fsanitize=${_saccade_sanitizer_flags}"
            -fno-omit-frame-pointer
        )
        target_link_options(saccade_sanitizers INTERFACE
            "-fsanitize=${_saccade_sanitizer_flags}"
        )
    else()
        message(FATAL_ERROR "Sanitizers are unsupported by ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()
