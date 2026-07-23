cmake_minimum_required(VERSION 3.30)
project(Saccade VERSION 0.1.0 LANGUAGES C CXX)

set(SACCADE_MODEL_PUBLIC_KEY_XY "" CACHE STRING
    "Trusted model-signing P-256 public key as 128 hexadecimal digits")

if(APPLE)
    enable_language(OBJCXX)
endif()

if(APPLE AND "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING
        "Minimum macOS deployment target" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SaccadeOptions)
include(SaccadeWarnings)
include(SaccadeSanitizers)
if(MSVC AND SACCADE_MSVC_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
find_package(Threads REQUIRED)

set(SACCADE_WINDOWS_ML_AVAILABLE OFF)
if(WIN32 AND NOT "${SACCADE_WINDOWS_ML_ROOT}" STREQUAL "")
    set("microsoft.windows.ai.machinelearning_DIR"
        "${SACCADE_WINDOWS_ML_ROOT}/build/cmake"
        CACHE PATH "Windows ML package configuration" FORCE)
    set(_saccade_host_system_processor "${CMAKE_SYSTEM_PROCESSOR}")
    if(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
        # The package config otherwise selects the host runtime for Ninja cross builds.
        set(CMAKE_SYSTEM_PROCESSOR "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    endif()
    find_package(microsoft.windows.ai.machinelearning CONFIG REQUIRED
        PATHS "${SACCADE_WINDOWS_ML_ROOT}/build/cmake"
        NO_DEFAULT_PATH)
    set(CMAKE_SYSTEM_PROCESSOR "${_saccade_host_system_processor}")
    unset(_saccade_host_system_processor)
    include(SaccadeWindowsMl)
    saccade_verify_windows_ml_runtime()
    set(SACCADE_WINDOWS_ML_AVAILABLE ON)
endif()

if(APPLE AND SACCADE_BUILD_APPLICATION AND
   "${SACCADE_MACOS_CODESIGN_IDENTITY}" STREQUAL "-")
    message(WARNING
        "The macOS application is ad-hoc signed. Its Screen Recording and Accessibility "
        "grants will not survive a rebuild because the designated requirement changes "
        "with the executable. Configure SACCADE_MACOS_CODESIGN_IDENTITY with a stable "
        "Apple Development or Developer ID identity before attended testing.")
endif()

if(SACCADE_DISTRIBUTION_BUILD)
    if(NOT SACCADE_BUILD_APPLICATION)
        message(FATAL_ERROR "A distribution build requires SACCADE_BUILD_APPLICATION=ON")
    endif()
    if(NOT SACCADE_BUILD_STATIC)
        message(FATAL_ERROR "A distribution build requires SACCADE_BUILD_STATIC=ON")
    endif()
    if(NOT SACCADE_BUILD_TOOLS)
        message(FATAL_ERROR "A distribution build requires SACCADE_BUILD_TOOLS=ON")
    endif()
    if(NOT EXISTS "${SACCADE_MODEL_ARTIFACT}")
        message(FATAL_ERROR "A distribution build requires an existing SACCADE_MODEL_ARTIFACT")
    endif()
    string(LENGTH "${SACCADE_MODEL_PUBLIC_KEY_XY}" _saccade_public_key_size)
    if(NOT _saccade_public_key_size EQUAL 128 OR SACCADE_MODEL_PUBLIC_KEY_XY MATCHES "[^0-9A-Fa-f]")
        message(FATAL_ERROR
            "A distribution build requires SACCADE_MODEL_PUBLIC_KEY_XY as 128 hexadecimal digits")
    endif()

    if(APPLE)
        if(NOT IS_DIRECTORY "${SACCADE_COREML_MODEL_BUNDLE}")
            message(FATAL_ERROR "A macOS distribution build requires SACCADE_COREML_MODEL_BUNDLE")
        endif()
        if("${SACCADE_MACOS_CODESIGN_IDENTITY}" STREQUAL "" OR
           "${SACCADE_MACOS_CODESIGN_IDENTITY}" STREQUAL "-")
            message(FATAL_ERROR "A macOS distribution build requires a non-ad-hoc signing identity")
        endif()
        if(NOT SACCADE_MACOS_HARDENED_RUNTIME)
            message(FATAL_ERROR "A macOS distribution build requires the hardened runtime")
        endif()
        if("${SACCADE_MACOS_NOTARY_PROFILE}" STREQUAL "")
            message(FATAL_ERROR "A macOS distribution build requires SACCADE_MACOS_NOTARY_PROFILE")
        endif()
        find_program(SACCADE_MACOS_XCRUN xcrun REQUIRED)
        find_program(SACCADE_MACOS_CODESIGN codesign REQUIRED)
    elseif(WIN32)
        if(NOT SACCADE_WINDOWS_ML_AVAILABLE)
            message(FATAL_ERROR "A Windows distribution build requires SACCADE_WINDOWS_ML_ROOT")
        endif()
        if(NOT SACCADE_MSVC_STATIC_RUNTIME)
            message(FATAL_ERROR "A Windows distribution build requires SACCADE_MSVC_STATIC_RUNTIME=ON")
        endif()
        if(NOT SACCADE_WINDOWS_UIACCESS)
            message(FATAL_ERROR "A Windows distribution build requires SACCADE_WINDOWS_UIACCESS=ON")
        endif()
        string(LENGTH "${SACCADE_WINDOWS_SIGNING_CERTIFICATE_SHA1}" _saccade_certificate_size)
        if(NOT _saccade_certificate_size EQUAL 40 OR
           SACCADE_WINDOWS_SIGNING_CERTIFICATE_SHA1 MATCHES "[^0-9A-Fa-f]")
            message(FATAL_ERROR
                "A Windows distribution build requires a 40-digit SACCADE_WINDOWS_SIGNING_CERTIFICATE_SHA1")
        endif()
        if("${SACCADE_WINDOWS_TIMESTAMP_URL}" STREQUAL "")
            message(FATAL_ERROR "A Windows distribution build requires SACCADE_WINDOWS_TIMESTAMP_URL")
        endif()
        if("${SACCADE_WINDOWS_SIGNTOOL}" STREQUAL "")
            find_program(_saccade_windows_signtool signtool REQUIRED)
            set(SACCADE_WINDOWS_SIGNTOOL "${_saccade_windows_signtool}" CACHE FILEPATH
                "Path to signtool.exe for Windows distribution signing" FORCE)
        elseif(NOT EXISTS "${SACCADE_WINDOWS_SIGNTOOL}")
            message(FATAL_ERROR "SACCADE_WINDOWS_SIGNTOOL does not exist")
        endif()
    endif()
endif()

add_library(saccade_build_settings INTERFACE)
saccade_apply_warnings(saccade_build_settings)
target_link_libraries(saccade_build_settings INTERFACE saccade_sanitizers)

if(SACCADE_BUILD_TESTS)
    include(CTest)
    enable_testing()
endif()

add_subdirectory(src)
if(SACCADE_BUILD_STATIC AND
   (SACCADE_BACKEND_MOCK OR SACCADE_BACKEND_REFERENCE_CPU OR
    (APPLE AND SACCADE_BACKEND_METAL) OR
    (WIN32 AND SACCADE_BACKEND_D3D12)))
    add_subdirectory(backends)
endif()
if(APPLE OR WIN32)
    add_subdirectory(platform)
endif()
if(SACCADE_BUILD_TOOLS AND SACCADE_BUILD_STATIC AND (APPLE OR WIN32))
    add_subdirectory(tools)
endif()
if(SACCADE_BUILD_APPLICATION AND (APPLE OR WIN32))
    add_subdirectory(apps)
endif()
if(SACCADE_BUILD_TESTS)
    add_subdirectory(tests)
    add_test(
        NAME cmake.packaging_contract
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            -P "${PROJECT_SOURCE_DIR}/tests/cmake/packaging_contract.cmake")
endif()
if(SACCADE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_EXPORT_COMPILE_COMMANDS)
    add_custom_target(saccade_clang_analyzer
        COMMAND "${PROJECT_SOURCE_DIR}/scripts/run_clang_analyzer.sh"
            "${PROJECT_BINARY_DIR}/compile_commands.json"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM)
endif()

set(_saccade_install_targets saccade_core)
if(SACCADE_BUILD_STATIC AND SACCADE_BUILD_SHARED)
    list(APPEND _saccade_install_targets saccade_core_shared)
endif()

install(
    TARGETS ${_saccade_install_targets}
    EXPORT SaccadeTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(
    DIRECTORY "${PROJECT_SOURCE_DIR}/include/saccade"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h")

set(SACCADE_PACKAGE_SHARED_AS_CORE OFF)
if(SACCADE_BUILD_SHARED AND NOT SACCADE_BUILD_STATIC)
    set(SACCADE_PACKAGE_SHARED_AS_CORE ON)
endif()
set(_saccade_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/Saccade")
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/SaccadeConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/SaccadeConfig.cmake"
    INSTALL_DESTINATION "${_saccade_package_dir}")
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/SaccadeConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion)
install(
    EXPORT SaccadeTargets
    FILE SaccadeTargets.cmake
    NAMESPACE Saccade::
    DESTINATION "${_saccade_package_dir}")
install(
    FILES
        "${PROJECT_BINARY_DIR}/SaccadeConfig.cmake"
        "${PROJECT_BINARY_DIR}/SaccadeConfigVersion.cmake"
    DESTINATION "${_saccade_package_dir}")

if(SACCADE_BUILD_APPLICATION)
    include(SaccadePackaging)
endif()
