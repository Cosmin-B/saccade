file(GLOB saccade_dxc_candidates
    "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe")
list(SORT saccade_dxc_candidates COMPARE NATURAL ORDER DESCENDING)
list(GET saccade_dxc_candidates 0 SACCADE_DXC)

set(saccade_preprocess_hlsl
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/preprocess.hlsl")
set(saccade_d3d12_shader_outputs)
foreach(shader IN ITEMS
        "preprocess_fp16;SACCADE_PREPROCESS_FP16"
        "preprocess_int8;SACCADE_PREPROCESS_INT8")
    list(GET shader 0 entry)
    list(GET shader 1 definition)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${entry}.dxil")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${SACCADE_DXC}" -nologo -WX -Ges -O3
            -Qstrip_debug -Qstrip_reflect
            -D "${definition}=1"
            -T cs_6_0 -E "${entry}" -Fo "${output}"
            "${saccade_preprocess_hlsl}"
        DEPENDS "${saccade_preprocess_hlsl}"
        VERBATIM)
    list(APPEND saccade_d3d12_shader_outputs "${output}")
endforeach()

set(saccade_target_postprocess_hlsl
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/target_postprocess.hlsl")
foreach(entry IN ITEMS
        targets_pack_normalized
        targets_prepare
        targets_radix_histogram
        targets_radix_scan
        targets_radix_scatter
        targets_suppression_masks
        targets_finalize)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${entry}.dxil")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${SACCADE_DXC}" -nologo -WX -Ges -O3
            -Qstrip_debug -Qstrip_reflect
            -T cs_6_0 -E "${entry}" -Fo "${output}"
            "${saccade_target_postprocess_hlsl}"
        DEPENDS "${saccade_target_postprocess_hlsl}"
        VERBATIM)
    list(APPEND saccade_d3d12_shader_outputs "${output}")
endforeach()

set(saccade_overlay_hlsl
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/overlay.hlsl")
foreach(shader IN ITEMS
        "expand_static;cs_6_0"
        "update_active;cs_6_0"
        "overlay_vertex;vs_6_0"
        "overlay_pixel;ps_6_0")
    list(GET shader 0 entry)
    list(GET shader 1 profile)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${entry}.dxil")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${SACCADE_DXC}" -nologo -WX -Ges -O3
            -Qstrip_debug -Qstrip_reflect
            -T "${profile}" -E "${entry}" -Fo "${output}"
            "${saccade_overlay_hlsl}"
        DEPENDS "${saccade_overlay_hlsl}"
        VERBATIM)
    list(APPEND saccade_d3d12_shader_outputs "${output}")
endforeach()

add_custom_target(saccade_d3d12_shaders
    DEPENDS ${saccade_d3d12_shader_outputs})

add_library(saccade_backend_d3d12 STATIC
    graphics_device.cpp
    overlay_renderer.cpp
    preprocessor.cpp
    target_postprocessor.cpp)
if(SACCADE_WINDOWS_ML_AVAILABLE)
    target_sources(saccade_backend_d3d12 PRIVATE
        directml_inference.cpp
        directml_provider.cpp)
    target_compile_definitions(saccade_backend_d3d12
        PUBLIC SACCADE_HAS_WINDOWS_ML=1)
    target_include_directories(saccade_backend_d3d12
        PRIVATE "${WINML_INCLUDE_DIR}/winml")
endif()
add_dependencies(saccade_backend_d3d12 saccade_d3d12_shaders)
target_include_directories(saccade_backend_d3d12
    PUBLIC
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(saccade_backend_d3d12
    PUBLIC saccade_core
    PRIVATE
        saccade_build_settings
        d3d11
        d3d12
        dxgi)
add_library(Saccade::d3d12 ALIAS saccade_backend_d3d12)

set(SACCADE_D3D12_SHADER_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    PARENT_SCOPE)
