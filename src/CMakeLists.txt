set(SACCADE_CORE_SOURCES
    backend/registry.cpp
    core/error.cpp
    core/runtime.cpp
    core/version.cpp
    kernels/image/luma.cpp)

include(SaccadeTargetArchitecture)
saccade_resolve_target_architecture(
    saccade_target_architecture
    "${CMAKE_SYSTEM_PROCESSOR}"
    "${CMAKE_OSX_ARCHITECTURES}"
    "${CMAKE_VS_PLATFORM_NAME}"
    "${CMAKE_GENERATOR_PLATFORM}"
    "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
if(NOT saccade_target_architecture)
    message(FATAL_ERROR "Saccade kernel builds require one target architecture")
endif()

if(saccade_target_architecture STREQUAL "arm64")
    list(APPEND SACCADE_CORE_SOURCES kernels/image/luma_neon.cpp)
    set(SACCADE_CORE_PRIVATE_DEFINITIONS SACCADE_BUILD_LUMA_NEON=1)
elseif(saccade_target_architecture STREQUAL "x86_64")
    list(APPEND SACCADE_CORE_SOURCES kernels/image/luma_avx2.cpp)
    set(SACCADE_CORE_PRIVATE_DEFINITIONS SACCADE_BUILD_LUMA_AVX2=1)
    if(MSVC)
        set_source_files_properties(
            kernels/image/luma_avx2.cpp
            PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(
            kernels/image/luma_avx2.cpp
            PROPERTIES COMPILE_OPTIONS "-mavx2")
    endif()
endif()

if(NOT SACCADE_BUILD_STATIC AND NOT SACCADE_BUILD_SHARED)
    message(FATAL_ERROR "Enable SACCADE_BUILD_STATIC, SACCADE_BUILD_SHARED, or both")
endif()

function(saccade_configure_core target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${target} PRIVATE
        $<BUILD_INTERFACE:saccade_build_settings>
        Threads::Threads)
    if(SACCADE_CORE_PRIVATE_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE
            ${SACCADE_CORE_PRIVATE_DEFINITIONS})
    endif()
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
    set_target_properties(saccade_core PROPERTIES EXPORT_NAME core)
    if(WIN32 AND SACCADE_BUILD_SHARED)
        set_target_properties(saccade_core PROPERTIES OUTPUT_NAME saccade_core_static)
    endif()
    add_library(Saccade::core ALIAS saccade_core)

    if(SACCADE_BUILD_SHARED)
        add_library(saccade_core_shared SHARED ${SACCADE_CORE_SOURCES})
        saccade_configure_shared_core(saccade_core_shared)
        set_target_properties(saccade_core_shared PROPERTIES
            EXPORT_NAME shared
            OUTPUT_NAME saccade_core)
        add_library(Saccade::shared ALIAS saccade_core_shared)
    endif()
else()
    add_library(saccade_core SHARED ${SACCADE_CORE_SOURCES})
    saccade_configure_shared_core(saccade_core)
    set_target_properties(saccade_core PROPERTIES EXPORT_NAME core)
    add_library(Saccade::core ALIAS saccade_core)
    add_library(Saccade::shared ALIAS saccade_core)
endif()
