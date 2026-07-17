cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED BUILD_DIR OR BUILD_DIR STREQUAL "")
    message(FATAL_ERROR "Pass -DBUILD_DIR=<configured build directory>")
endif()

if(NOT DEFINED CATEGORY OR CATEGORY STREQUAL "")
    message(FATAL_ERROR
        "Pass -DCATEGORY=registration, owned-window, workflow, capture, overlay, or all")
endif()

set(valid_categories
    registration
    owned-window
    workflow
    capture
    overlay
    all)
if(NOT CATEGORY IN_LIST valid_categories)
    message(FATAL_ERROR "Unknown live test category: ${CATEGORY}")
endif()

if(NOT "$ENV{SACCADE_ALLOW_LIVE_TESTS}" STREQUAL "${CATEGORY}")
    message(FATAL_ERROR
        "Live tests are disabled. Set SACCADE_ALLOW_LIVE_TESTS=${CATEGORY} "
        "after reviewing the selected category.")
endif()

if(CATEGORY STREQUAL "all")
    set(label "^live$")
else()
    set(label "^live-${CATEGORY}$")
endif()

set(ctest_arguments
    --test-dir "${BUILD_DIR}"
    --output-on-failure
    -L "${label}")
if(DEFINED REPORT_FILE AND NOT REPORT_FILE STREQUAL "")
    list(APPEND ctest_arguments --output-junit "${REPORT_FILE}")
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" ${ctest_arguments}
    RESULT_VARIABLE test_result)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR "Live ${CATEGORY} test lane failed: ${test_result}")
endif()
