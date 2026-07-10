cmake_minimum_required(VERSION 3.25)
project(SaccadeInstallConsumer LANGUAGES C CXX)

enable_testing()
find_package(Saccade 0.1 CONFIG REQUIRED)

function(add_saccade_consumers suffix library runtime_library)
    set(c_target "saccade_consumer_${suffix}_c")
    add_executable(${c_target} consumer.c)
    set_target_properties(${c_target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    target_link_libraries(${c_target} PRIVATE ${library})
    add_test(NAME "consumer.${suffix}.c" COMMAND ${c_target})

    set(cxx_target "saccade_consumer_${suffix}_cpp")
    add_executable(${cxx_target} consumer.cpp)
    set_target_properties(${cxx_target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    target_link_libraries(${cxx_target} PRIVATE ${library})
    add_test(NAME "consumer.${suffix}.cpp" COMMAND ${cxx_target})

    if(WIN32 AND NOT "${runtime_library}" STREQUAL "")
        foreach(target IN ITEMS ${c_target} ${cxx_target})
            add_custom_command(
                TARGET ${target}
                POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:${runtime_library}>"
                    "$<TARGET_FILE_DIR:${target}>")
        endforeach()
    endif()
endfunction()

set(core_runtime_library "")
if(SACCADE_CORE_IS_SHARED)
    set(core_runtime_library Saccade::core)
endif()
add_saccade_consumers(core Saccade::core "${core_runtime_library}")

if(SACCADE_USE_SHARED)
    if(NOT TARGET Saccade::shared)
        message(FATAL_ERROR "The shared package target is missing")
    endif()
    if(SACCADE_CORE_IS_SHARED)
        set(shared_runtime_library Saccade::core)
    else()
        set(shared_runtime_library Saccade::shared)
    endif()
    add_saccade_consumers(shared Saccade::shared "${shared_runtime_library}")
endif()
