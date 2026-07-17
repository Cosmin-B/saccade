function(saccade_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} INTERFACE
            /W4
            /wd4324
            $<$<COMPILE_LANGUAGE:CXX>:/permissive->
            /Zc:preprocessor
        )
        if(SACCADE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} INTERFACE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
        )
        if(SACCADE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} INTERFACE -Werror)
        endif()
    endif()
endfunction()
