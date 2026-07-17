set(CPACK_PACKAGE_NAME "Saccade")
set(CPACK_PACKAGE_VENDOR "Saccade")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native keyboard-driven pointer control")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_MONOLITHIC_INSTALL OFF)
set(CPACK_COMPONENTS_ALL Application)
set(CPACK_COMPONENT_APPLICATION_DISPLAY_NAME "Saccade")
set(CPACK_COMPONENT_APPLICATION_DESCRIPTION "Saccade desktop application and local agent tools")
set(CPACK_COMPONENT_APPLICATION_REQUIRED ON)

include(SaccadeTargetArchitecture)
saccade_resolve_target_architecture(
    _saccade_target_architecture
    "${CMAKE_SYSTEM_PROCESSOR}"
    "${CMAKE_OSX_ARCHITECTURES}"
    "${CMAKE_VS_PLATFORM_NAME}"
    "${CMAKE_GENERATOR_PLATFORM}"
    "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
saccade_resolve_package_architecture(
    _saccade_package_architecture
    "${CMAKE_SYSTEM_NAME}"
    "${_saccade_target_architecture}")
if(NOT _saccade_package_architecture)
    message(FATAL_ERROR
        "Saccade packaging does not support ${CMAKE_SYSTEM_NAME}/${_saccade_target_architecture}")
endif()

if(WIN32)
    set(CPACK_GENERATOR WIX)
    set(CPACK_WIX_VERSION 4)
    set(CPACK_WIX_INSTALL_SCOPE perMachine)
    set(CPACK_WIX_COMPONENT_INSTALL ON)
    set(CPACK_WIX_UPGRADE_GUID "87f6040e-c0bc-5c5b-9d9a-c501e7d63e87")
    set(CPACK_WIX_PATCH_FILE
        "${PROJECT_SOURCE_DIR}/cmake/SaccadeWixLaunchCondition.xml")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Saccade")
    set(CPACK_PACKAGE_EXECUTABLES Saccade Saccade)
    set(CPACK_WIX_ARCHITECTURE "${_saccade_package_architecture}")
    set(CPACK_PACKAGE_FILE_NAME
        "Saccade-${PROJECT_VERSION}-windows-${_saccade_package_architecture}")
    if(SACCADE_DISTRIBUTION_BUILD)
        configure_file(
            "${PROJECT_SOURCE_DIR}/cmake/SignWindowsPackage.cmake.in"
            "${PROJECT_BINARY_DIR}/SignWindowsPackage.cmake"
            @ONLY)
        set(CPACK_POST_BUILD_SCRIPTS
            "${PROJECT_BINARY_DIR}/SignWindowsPackage.cmake")
    endif()
elseif(APPLE)
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_DMG_VOLUME_NAME "Saccade ${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME
        "Saccade-${PROJECT_VERSION}-macos-${_saccade_package_architecture}")
    if(SACCADE_DISTRIBUTION_BUILD)
        configure_file(
            "${PROJECT_SOURCE_DIR}/cmake/NotarizeMacPackage.cmake.in"
            "${PROJECT_BINARY_DIR}/NotarizeMacPackage.cmake"
            @ONLY)
        set(CPACK_POST_BUILD_SCRIPTS
            "${PROJECT_BINARY_DIR}/NotarizeMacPackage.cmake")
    endif()
endif()

include(CPack)
