cmake_minimum_required(VERSION 3.25)
project(Saccade VERSION 0.1.0 LANGUAGES C CXX)

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
find_package(Threads REQUIRED)

add_library(saccade_build_settings INTERFACE)
saccade_apply_warnings(saccade_build_settings)
target_link_libraries(saccade_build_settings INTERFACE saccade_sanitizers)

if(SACCADE_BUILD_TESTS)
    include(CTest)
    enable_testing()
endif()

add_subdirectory(src)
if(SACCADE_BUILD_STATIC AND
   (SACCADE_BACKEND_MOCK OR SACCADE_BACKEND_REFERENCE_CPU))
    add_subdirectory(backends)
endif()
if(SACCADE_BUILD_TESTS)
    add_subdirectory(tests)
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
