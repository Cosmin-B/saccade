#include <metal_stdlib>

using namespace metal;

struct PreprocessParameters {
    float4 source_rect;
    float4 content_rect;
    float4 channel_scale;
    float4 channel_bias;
    float4 letterbox_rgb;
    uint2 output_size;
    uint2 reserved;
};

static_assert(sizeof(PreprocessParameters) == 96, "PreprocessParameters layout");

struct AtlasClearParameters {
    float4 color;
    uint2 output_size;
    uint2 reserved;
};

struct AtlasParameters {
    float4 source_rect;
    float4 channel_scale;
    float4 channel_bias;
    uint4 destination_rect;
};

static_assert(sizeof(AtlasClearParameters) == 32, "AtlasClearParameters layout");
static_assert(sizeof(AtlasParameters) == 64, "AtlasParameters layout");

float3 preprocess_sample(texture2d<float, access::sample> source, constant PreprocessParameters& parameters,
                         uint2 position) {
    const float2 pixel = float2(position) + 0.5;
    const float2 content_min = parameters.content_rect.xy;
    const float2 content_max = content_min + parameters.content_rect.zw;
    if (any(pixel < content_min) || any(pixel >= content_max)) {
        return parameters.letterbox_rgb.rgb * parameters.channel_scale.rgb + parameters.channel_bias.rgb;
    }

    constexpr sampler linear_sampler(coord::pixel, address::clamp_to_edge, filter::linear);
    const float2 local = (pixel - content_min) / parameters.content_rect.zw;
    const float2 source_min = parameters.source_rect.xy + 0.5;
    const float2 source_max = parameters.source_rect.xy + parameters.source_rect.zw - 0.5;
    const float2 source_pixel =
        clamp(parameters.source_rect.xy + local * parameters.source_rect.zw, source_min, source_max);
    const float3 rgb = source.sample(linear_sampler, source_pixel).rgb;
    return rgb * parameters.channel_scale.rgb + parameters.channel_bias.rgb;
}

kernel void saccade_preprocess_fp16(texture2d<float, access::sample> source [[texture(0)]],
                                    constant PreprocessParameters& parameters [[buffer(0)]],
                                    device half* output [[buffer(1)]], uint2 position [[thread_position_in_grid]]) {
    const uint index = position.y * parameters.output_size.x + position.x;
    const uint plane = parameters.output_size.x * parameters.output_size.y;
    const float3 value = preprocess_sample(source, parameters, position);
    output[index] = half(value.r);
    output[plane + index] = half(value.g);
    output[plane * 2 + index] = half(value.b);
}

kernel void saccade_preprocess_int8(texture2d<float, access::sample> source [[texture(0)]],
                                    constant PreprocessParameters& parameters [[buffer(0)]],
                                    device char* output [[buffer(1)]], uint2 position [[thread_position_in_grid]]) {
    const uint index = position.y * parameters.output_size.x + position.x;
    const uint plane = parameters.output_size.x * parameters.output_size.y;
    const float3 value = round(preprocess_sample(source, parameters, position));
    output[index] = char(clamp(value.r, -128.0, 127.0));
    output[plane + index] = char(clamp(value.g, -128.0, 127.0));
    output[plane * 2 + index] = char(clamp(value.b, -128.0, 127.0));
}

kernel void saccade_preprocess_bgra8(texture2d<float, access::sample> source [[texture(0)]],
                                     texture2d<float, access::write> output [[texture(1)]],
                                     constant PreprocessParameters& parameters [[buffer(0)]],
                                     uint2 position [[thread_position_in_grid]]) {
    const float3 value = clamp(preprocess_sample(source, parameters, position), 0.0, 1.0);
    output.write(float4(value, 1.0), position);
}

kernel void saccade_preprocess_clear_bgra8(texture2d<float, access::write> output [[texture(1)]],
                                           constant AtlasClearParameters& parameters [[buffer(0)]],
                                           uint2 position [[thread_position_in_grid]]) {
    output.write(parameters.color, position);
}

kernel void saccade_preprocess_atlas_bgra8(texture2d<float, access::sample> source [[texture(0)]],
                                           texture2d<float, access::write> output [[texture(1)]],
                                           constant AtlasParameters& parameters [[buffer(0)]],
                                           uint2 position [[thread_position_in_grid]]) {
    constexpr sampler linear_sampler(coord::pixel, address::clamp_to_edge, filter::linear);
    const float2 local = (float2(position) + 0.5) / float2(parameters.destination_rect.zw);
    const float2 source_min = parameters.source_rect.xy + 0.5;
    const float2 source_max = parameters.source_rect.xy + parameters.source_rect.zw - 0.5;
    const float2 source_pixel =
        clamp(parameters.source_rect.xy + local * parameters.source_rect.zw, source_min, source_max);
    const float3 rgb = source.sample(linear_sampler, source_pixel).rgb;
    const float3 value = clamp(rgb * parameters.channel_scale.rgb + parameters.channel_bias.rgb, 0.0, 1.0);
    output.write(float4(value, 1.0), parameters.destination_rect.xy + position);
}
