set(_saccade_macos_icon_files
    Saccade.icns
    SaccadeStatus.png
    SaccadeStatus@2x.png)
set_source_files_properties(${_saccade_macos_icon_files} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)

add_executable(saccade_app MACOSX_BUNDLE
    main.mm
    ${_saccade_macos_icon_files})
target_include_directories(saccade_app PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src")
target_compile_options(saccade_app PRIVATE -fobjc-arc)
add_dependencies(saccade_app saccade_overlay_metallib)
add_custom_command(TARGET saccade_app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_BUNDLE_CONTENT_DIR:saccade_app>/Resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_BINARY_DIR}/backends/metal/saccade_overlay.metallib"
        "$<TARGET_BUNDLE_CONTENT_DIR:saccade_app>/Resources/saccade_overlay.metallib"
    VERBATIM)
if(SACCADE_DISTRIBUTION_BUILD AND (NOT TARGET saccade_cli OR NOT TARGET saccade_mcp))
    message(FATAL_ERROR "A macOS distribution build requires the CLI and MCP helper targets")
endif()
if(TARGET saccade_cli AND TARGET saccade_mcp)
    add_dependencies(saccade_app saccade_cli saccade_mcp)
    set_property(TARGET saccade_app APPEND PROPERTY LINK_DEPENDS
        "$<TARGET_FILE:saccade_cli>"
        "$<TARGET_FILE:saccade_mcp>")
    add_custom_command(TARGET saccade_app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:saccade_app>/Helpers"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:saccade_cli>"
            "$<TARGET_BUNDLE_CONTENT_DIR:saccade_app>/Helpers/$<TARGET_FILE_NAME:saccade_cli>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:saccade_mcp>"
            "$<TARGET_BUNDLE_CONTENT_DIR:saccade_app>/Helpers/$<TARGET_FILE_NAME:saccade_mcp>"
        VERBATIM)
endif()
if(SACCADE_MODEL_ARTIFACT)
    if(NOT EXISTS "${SACCADE_MODEL_ARTIFACT}")
        message(FATAL_ERROR "SACCADE_MODEL_ARTIFACT does not exist")
    endif()
    set_property(TARGET saccade_app APPEND PROPERTY LINK_DEPENDS
        "${SACCADE_MODEL_ARTIFACT}")
    add_custom_command(TARGET saccade_app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SACCADE_MODEL_ARTIFACT}"
            "$<TARGET_BUNDLE_CONTENT_DIR:saccade_app>/Resources/saccade.model"
        VERBATIM)
endif()
if(SACCADE_COREML_MODEL_BUNDLE)
    if(NOT IS_DIRECTORY "${SACCADE_COREML_MODEL_BUNDLE}")
        message(FATAL_ERROR "SACCADE_COREML_MODEL_BUNDLE is not a directory")
    endif()
    get_filename_component(_saccade_coreml_bundle_name
        "${SACCADE_COREML_MODEL_BUNDLE}" NAME)
    file(GLOB_RECURSE _saccade_coreml_bundle_files
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${SACCADE_COREML_MODEL_BUNDLE}/*")
    set_property(TARGET saccade_app APPEND PROPERTY LINK_DEPENDS
        ${_saccade_coreml_bundle_files})
    add_custom_command(TARGET saccade_app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${SACCADE_COREML_MODEL_BUNDLE}"
            "$<TARGET_BUNDLE_CONTENT_DIR:saccade_app>/Resources/${_saccade_coreml_bundle_name}"
        VERBATIM)
endif()
target_link_libraries(saccade_app PRIVATE
    saccade_build_settings
    saccade_platform_macos_pipeline
    "-framework AppKit"
    "-framework Carbon")
if(SACCADE_MODEL_PUBLIC_KEY_XY)
    target_compile_definitions(saccade_app PRIVATE
        SACCADE_MODEL_PUBLIC_KEY_XY="${SACCADE_MODEL_PUBLIC_KEY_XY}")
endif()
set_target_properties(saccade_app PROPERTIES
    OUTPUT_NAME Saccade
    MACOSX_BUNDLE_BUNDLE_NAME Saccade
    MACOSX_BUNDLE_GUI_IDENTIFIER com.cosmin.saccade
    MACOSX_BUNDLE_ICON_FILE Saccade.icns
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
if(NOT "${SACCADE_MACOS_CODESIGN_IDENTITY}" STREQUAL "")
    find_program(_saccade_codesign codesign REQUIRED)
    set(_saccade_codesign_arguments
        --force --deep --sign "${SACCADE_MACOS_CODESIGN_IDENTITY}")
    if(SACCADE_MACOS_HARDENED_RUNTIME)
        list(APPEND _saccade_codesign_arguments --options runtime)
    endif()
    if(NOT "${SACCADE_MACOS_CODESIGN_IDENTITY}" STREQUAL "-")
        list(APPEND _saccade_codesign_arguments --timestamp)
    endif()
    add_custom_command(TARGET saccade_app POST_BUILD
        COMMAND "${_saccade_codesign}" ${_saccade_codesign_arguments}
            "$<TARGET_BUNDLE_DIR:saccade_app>"
        COMMAND "${_saccade_codesign}" --verify --deep --strict --verbose=2
            "$<TARGET_BUNDLE_DIR:saccade_app>"
        VERBATIM)
endif()
install(TARGETS saccade_app
    BUNDLE DESTINATION .
    COMPONENT Application)
