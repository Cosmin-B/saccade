cbuffer PreprocessParameters : register(b0) {
    float4 source_rect;
    float4 content_rect;
    float4 channel_scale;
    float4 channel_bias;
    float4 letterbox_rgb;
    uint2 output_size;
    uint2 reserved;
};

Texture2D<float4> source_texture : register(t0);
SamplerState linear_sampler : register(s0);

float3 preprocess_sample(uint2 position) {
    const float2 pixel = float2(position) + 0.5;
    const float2 content_min = content_rect.xy;
    const float2 content_max = content_min + content_rect.zw;
    if (any(pixel < content_min) || any(pixel >= content_max)) {
        return letterbox_rgb.rgb * channel_scale.rgb + channel_bias.rgb;
    }

    uint source_width;
    uint source_height;
    source_texture.GetDimensions(source_width, source_height);
    const float2 local = (pixel - content_min) / content_rect.zw;
    const float2 source_pixel = source_rect.xy + local * source_rect.zw;
    const float2 source_size = float2(source_width, source_height);
    const float3 rgb = source_texture.SampleLevel(
        linear_sampler, source_pixel / source_size, 0).rgb;
    return rgb * channel_scale.rgb + channel_bias.rgb;
}

#if defined(SACCADE_PREPROCESS_FP16)
RWBuffer<float> output_tensor : register(u0);

[numthreads(16, 16, 1)]
void preprocess_fp16(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint2 position = dispatch_id.xy;
    if (any(position >= output_size)) return;
    const uint index = position.y * output_size.x + position.x;
    const uint plane = output_size.x * output_size.y;
    const float3 value = preprocess_sample(position);
    output_tensor[index] = value.r;
    output_tensor[plane + index] = value.g;
    output_tensor[plane * 2 + index] = value.b;
}
#endif

#if defined(SACCADE_PREPROCESS_INT8)
RWBuffer<int> output_tensor : register(u0);

[numthreads(16, 16, 1)]
void preprocess_int8(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint2 position = dispatch_id.xy;
    if (any(position >= output_size)) return;
    const uint index = position.y * output_size.x + position.x;
    const uint plane = output_size.x * output_size.y;
    const float3 value = round(preprocess_sample(position));
    output_tensor[index] = (int)clamp(value.r, -128.0, 127.0);
    output_tensor[plane + index] = (int)clamp(value.g, -128.0, 127.0);
    output_tensor[plane * 2 + index] = (int)clamp(value.b, -128.0, 127.0);
}
#endif
