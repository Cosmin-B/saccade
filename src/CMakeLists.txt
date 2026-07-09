set(SACCADE_CORE_SOURCES
    backend/registry.cpp
    core/error.cpp
    core/runtime.cpp
    core/version.cpp)

if(NOT SACCADE_BUILD_STATIC AND NOT SACCADE_BUILD_SHARED)
    message(FATAL_ERROR "Enable SACCADE_BUILD_STATIC, SACCADE_BUILD_SHARED, or both")
endif()

function(saccade_configure_core target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${target} PRIVATE saccade_build_settings)
endfunction()

function(saccade_configure_shared_core target)
    saccade_configure_core(${target})
    target_compile_definitions(${target}
        PRIVATE SACCADE_SHARED_BUILD
        INTERFACE SACCADE_SHARED_USE)
    set_target_properties(${target} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
endfunction()

if(SACCADE_BUILD_STATIC)
    add_library(saccade_core STATIC ${SACCADE_CORE_SOURCES})
    saccade_configure_core(saccade_core)
    add_library(Saccade::core ALIAS saccade_core)

    if(SACCADE_BUILD_SHARED)
        add_library(saccade_core_shared SHARED ${SACCADE_CORE_SOURCES})
        saccade_configure_shared_core(saccade_core_shared)
        set_target_properties(saccade_core_shared PROPERTIES OUTPUT_NAME saccade_core)
        add_library(Saccade::shared ALIAS saccade_core_shared)
    endif()
else()
    add_library(saccade_core SHARED ${SACCADE_CORE_SOURCES})
    saccade_configure_shared_core(saccade_core)
    add_library(Saccade::core ALIAS saccade_core)
    add_library(Saccade::shared ALIAS saccade_core)
endif()
