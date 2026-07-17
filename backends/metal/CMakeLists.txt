find_program(SACCADE_XCRUN xcrun REQUIRED)

set(saccade_overlay_air "${CMAKE_CURRENT_BINARY_DIR}/saccade_overlay.air")
set(saccade_preprocess_air "${CMAKE_CURRENT_BINARY_DIR}/saccade_preprocess.air")
set(saccade_target_postprocess_air
    "${CMAKE_CURRENT_BINARY_DIR}/saccade_target_postprocess.air")
set(saccade_overlay_metallib
    "${CMAKE_CURRENT_BINARY_DIR}/saccade_overlay.metallib")

add_custom_command(
    OUTPUT "${saccade_overlay_air}"
    COMMAND "${SACCADE_XCRUN}" -sdk macosx metal
        -std=metal3.1
        -Werror
        -c "${CMAKE_CURRENT_SOURCE_DIR}/shaders/overlay_expand.metal"
        -o "${saccade_overlay_air}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/overlay_expand.metal"
    VERBATIM)
add_custom_command(
    OUTPUT "${saccade_preprocess_air}"
    COMMAND "${SACCADE_XCRUN}" -sdk macosx metal
        -std=metal3.1
        -Werror
        -c "${CMAKE_CURRENT_SOURCE_DIR}/shaders/preprocess.metal"
        -o "${saccade_preprocess_air}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/preprocess.metal"
    VERBATIM)
add_custom_command(
    OUTPUT "${saccade_target_postprocess_air}"
    COMMAND "${SACCADE_XCRUN}" -sdk macosx metal
        -std=metal3.1
        -Werror
        -c "${CMAKE_CURRENT_SOURCE_DIR}/shaders/target_postprocess.metal"
        -o "${saccade_target_postprocess_air}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/target_postprocess.metal"
    VERBATIM)
add_custom_command(
    OUTPUT "${saccade_overlay_metallib}"
    COMMAND "${SACCADE_XCRUN}" -sdk macosx metallib
        "${saccade_overlay_air}"
        "${saccade_preprocess_air}"
        "${saccade_target_postprocess_air}"
        -o "${saccade_overlay_metallib}"
    DEPENDS "${saccade_overlay_air}" "${saccade_preprocess_air}"
            "${saccade_target_postprocess_air}"
    VERBATIM)
add_custom_target(saccade_overlay_metallib
    DEPENDS "${saccade_overlay_metallib}")

add_library(saccade_backend_metal STATIC
    overlay_expander.mm
    preprocessor.mm
    target_postprocessor.mm)
add_dependencies(saccade_backend_metal saccade_overlay_metallib)
target_include_directories(saccade_backend_metal
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_options(saccade_backend_metal PRIVATE -fobjc-arc)
target_link_libraries(saccade_backend_metal
    PUBLIC saccade_core
    PRIVATE
        saccade_build_settings
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework IOSurface"
        "-framework Metal")
add_library(Saccade::metal ALIAS saccade_backend_metal)

set(SACCADE_OVERLAY_METALLIB "${saccade_overlay_metallib}"
    PARENT_SCOPE)
