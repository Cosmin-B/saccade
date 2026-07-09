if(NOT DEFINED MODE)
    message(FATAL_ERROR "MODE is required")
endif()

function(run_compiler language source action result_var output_var)
    if(language STREQUAL "C")
        set(compiler "${C_COMPILER}")
        set(compiler_id "${C_COMPILER_ID}")
        set(standard "c11")
    else()
        set(compiler "${CXX_COMPILER}")
        set(compiler_id "${CXX_COMPILER_ID}")
        set(standard "c++20")
    endif()

    if(compiler_id STREQUAL "MSVC")
        set(language_options /std:${standard})
        if(language STREQUAL "CXX")
            list(APPEND language_options /permissive-)
        endif()
        if(action STREQUAL "compile")
            set(arguments
                /nologo ${language_options} /Zc:preprocessor
                /I${INCLUDE_DIR} /c "${source}"
                /Fo${BINARY_DIR}/typed_facade_unsupported_${language}.obj)
        else()
            set(arguments
                /nologo ${language_options} /Zc:preprocessor
                /I${INCLUDE_DIR} /EP "${source}")
        endif()
    else()
        if(action STREQUAL "compile")
            set(arguments
                -std=${standard} -pedantic-errors -I "${INCLUDE_DIR}"
                -c "${source}"
                -o "${BINARY_DIR}/typed_facade_unsupported_${language}.o")
        else()
            set(arguments
                -std=${standard} -pedantic-errors -I "${INCLUDE_DIR}"
                -E -P "${source}")
        endif()
    endif()

    execute_process(
        COMMAND "${compiler}" ${arguments}
        RESULT_VARIABLE compiler_result
        OUTPUT_VARIABLE compiler_stdout
        ERROR_VARIABLE compiler_stderr)
    set(${result_var} "${compiler_result}" PARENT_SCOPE)
    set(${output_var} "${compiler_stdout}\n${compiler_stderr}" PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "reject")
    run_compiler(C "${UNSUPPORTED_C_SOURCE}" compile c_result c_output)
    if(NOT c_result MATCHES "^[0-9]+$")
        message(FATAL_ERROR "C11 compiler did not run:\n${c_output}")
    endif()
    if(c_result EQUAL 0)
        message(FATAL_ERROR "C11 accepted an unsupported frame descriptor pointer")
    endif()
    if(NOT c_output MATCHES "UnsupportedFrameDesc|saccade_frame_import")
        message(FATAL_ERROR "C11 compilation failed for an unrelated reason:\n${c_output}")
    endif()

    run_compiler(CXX "${UNSUPPORTED_CXX_SOURCE}" compile cxx_result cxx_output)
    if(NOT cxx_result MATCHES "^[0-9]+$")
        message(FATAL_ERROR "C++20 compiler did not run:\n${cxx_output}")
    endif()
    if(cxx_result EQUAL 0)
        message(FATAL_ERROR "C++20 accepted an unsupported frame descriptor pointer")
    endif()
    if(NOT cxx_output MATCHES "UnsupportedFrameDesc|saccade_frame_import")
        message(FATAL_ERROR "C++20 compilation failed for an unrelated reason:\n${cxx_output}")
    endif()

    message(STATUS "C11 and C++20 rejected unsupported frame descriptor pointers")
elseif(MODE STREQUAL "preprocess")
    run_compiler(C "${SUPPORTED_C_SOURCE}" preprocess c_result c_output)
    if(NOT c_result EQUAL 0)
        message(FATAL_ERROR "C11 preprocessing failed:\n${c_output}")
    endif()

    run_compiler(CXX "${SUPPORTED_CXX_SOURCE}" preprocess cxx_result cxx_output)
    if(NOT cxx_result EQUAL 0)
        message(FATAL_ERROR "C++20 preprocessing failed:\n${cxx_output}")
    endif()

    if(NOT c_output MATCHES "_Generic")
        message(FATAL_ERROR "C11 facade did not preprocess to a generic selection")
    endif()
    if(c_output MATCHES "default[ \t\r\n]*:")
        message(FATAL_ERROR "C11 generic selection unexpectedly contains a default arm")
    endif()
    if(cxx_output MATCHES "_Generic")
        message(FATAL_ERROR "C++20 facade unexpectedly contains a C11 generic selection")
    endif()
    if(NOT cxx_output MATCHES "noexcept")
        message(FATAL_ERROR "C++20 facade did not preprocess to noexcept overloads")
    endif()

    if(NOT c_output MATCHES
       "SaccadeHostFrameDesc[ \t]*\\*[ \t]*:[ \t]*saccade_frame_import_host")
        message(FATAL_ERROR "C11 host descriptor association is missing")
    endif()
    if(NOT c_output MATCHES
       "SaccadeIOSurfaceFrameDesc[ \t]*\\*[ \t]*:[ \t]*saccade_frame_import_iosurface")
        message(FATAL_ERROR "C11 IOSurface descriptor association is missing")
    endif()
    if(NOT c_output MATCHES
       "SaccadeD3D11FrameDesc[ \t]*\\*[ \t]*:[ \t]*saccade_frame_import_d3d11")
        message(FATAL_ERROR "C11 D3D11 descriptor association is missing")
    endif()

    foreach(function IN ITEMS host iosurface d3d11)
        if(NOT cxx_output MATCHES
           "return saccade_frame_import_${function}\\(runtime, desc, out_frame\\);")
            message(FATAL_ERROR "C++20 ${function} overload is missing")
        endif()
    endforeach()

    set(runtime_switch_pattern "(^|[^A-Za-z0-9_])switch[ \t\r\n]*\\(")
    if(c_output MATCHES "${runtime_switch_pattern}" OR
       cxx_output MATCHES "${runtime_switch_pattern}")
        message(FATAL_ERROR "typed facade preprocessor output contains a runtime switch")
    endif()

    message(STATUS "preprocessor output uses static language dispatch with no runtime switch")
else()
    message(FATAL_ERROR "unknown MODE: ${MODE}")
endif()
