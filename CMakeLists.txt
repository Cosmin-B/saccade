cmake_minimum_required(VERSION 3.25)
project(Saccade VERSION 0.1.0 LANGUAGES C CXX)

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

add_library(saccade_build_settings INTERFACE)
saccade_apply_warnings(saccade_build_settings)
target_link_libraries(saccade_build_settings INTERFACE saccade_sanitizers)

if(SACCADE_BUILD_TESTS)
    include(CTest)
    enable_testing()
endif()

add_subdirectory(src)
if(SACCADE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
