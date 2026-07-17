ByteAddressBuffer targets : register(t0);
ByteAddressBuffer styles : register(t1);
RWStructuredBuffer<uint2> output_rects : register(u0);
RWStructuredBuffer<uint> output_metadata : register(u1);
RWByteAddressBuffer output_arguments : register(u2);

cbuffer ExpandParameters : register(b0) {
    uint target_count;
    uint active_target_index;
    uint static_instance_count;
    uint has_active_target;
};

static const uint target_stride = 48;
static const uint style_stride = 64;
static const uint target_mask = 0x00003fff;
static const uint style_shift = 14;
static const uint style_mask = 0x0003c000;
static const uint kind_shift = 18;
static const uint kind_mask = 0x001c0000;
static const uint outline_kind = 1;
static const uint label_kind = 2;
static const uint active_kind = 3;

uint low16(uint value) { return value & 0xffff; }
uint high16(uint value) { return value >> 16; }

uint target_word(uint index, uint offset) {
    return targets.Load(index * target_stride + offset);
}

uint style_word(uint index, uint offset) {
    return styles.Load(index * style_stride + offset);
}

uint target_style(uint index) { return (target_word(index, 36) >> 16) & 0xff; }
uint target_glyph_count(uint index) { return target_word(index, 36) >> 24; }

uint metadata_value(uint target_index, uint style_index, uint kind) {
    return (target_index & target_mask) |
           ((style_index << style_shift) & style_mask) |
           ((kind << kind_shift) & kind_mask);
}

uint2 rect_value(uint x, uint y, uint width, uint height) {
    return uint2((x & 0xffff) | (y << 16),
                 (width & 0xffff) | (height << 16));
}

[numthreads(64, 1, 1)]
void expand_static(uint3 thread_id : SV_DispatchThreadID) {
    const uint index = thread_id.x;
    if (index >= target_count) {
        return;
    }
    const uint xy = target_word(index, 8);
    const uint wh = target_word(index, 12);
    const uint label_xy = target_word(index, 16);
    const uint x = low16(xy);
    const uint y = high16(xy);
    const uint width = low16(wh);
    const uint height = high16(wh);
    const uint style_index = target_style(index);
    const uint style_stroke_radius = style_word(style_index, 20);
    const uint stroke = min(low16(style_stroke_radius), min(width / 2, height / 2));
    const uint destination = index * 5;
    const uint outline = metadata_value(index, style_index, outline_kind);
    output_rects[destination] = rect_value(x, y, width, stroke);
    output_rects[destination + 1] =
        rect_value(x, y + height - stroke, width, stroke);
    output_rects[destination + 2] = rect_value(x, y, stroke, height);
    output_rects[destination + 3] =
        rect_value(x + width - stroke, y, stroke, height);
    output_metadata[destination] = outline;
    output_metadata[destination + 1] = outline;
    output_metadata[destination + 2] = outline;
    output_metadata[destination + 3] = outline;
    const uint label_height = low16(style_word(style_index, 24));
    const uint padding = low16(style_word(style_index, 28));
    const uint advance = high16(style_word(style_index, 32));
    const uint label_width = padding * 2 + advance * target_glyph_count(index);
    output_rects[destination + 4] =
        rect_value(low16(label_xy), high16(label_xy), label_width, label_height);
    output_metadata[destination + 4] =
        metadata_value(index, style_index, label_kind);
}

[numthreads(1, 1, 1)]
void update_active(uint3 thread_id : SV_DispatchThreadID) {
    uint count = static_instance_count;
    if (has_active_target != 0) {
        const uint xy = target_word(active_target_index, 8);
        const uint wh = target_word(active_target_index, 12);
        const uint style_index = target_style(active_target_index);
        output_rects[count] = rect_value(low16(xy), high16(xy),
                                         low16(wh), high16(wh));
        output_metadata[count] =
            metadata_value(active_target_index, style_index, active_kind);
        ++count;
    }
    output_arguments.Store(0, 6);
    output_arguments.Store(4, count);
    output_arguments.Store(8, 0);
    output_arguments.Store(12, 0);
}

StructuredBuffer<uint2> render_rects : register(t0);
StructuredBuffer<uint> render_metadata : register(t1);
ByteAddressBuffer render_targets : register(t2);
ByteAddressBuffer render_styles : register(t3);

uint render_target_word(uint index, uint offset) {
    return render_targets.Load(index * target_stride + offset);
}
uint render_target_glyph_count(uint index) {
    return render_target_word(index, 36) >> 24;
}

uint render_target_glyph(uint index, uint slot) {
    const uint byte_offset = 22 + slot;
    const uint word = render_target_word(index, byte_offset & ~3u);
    return (word >> ((byte_offset & 3u) * 8u)) & 0xffu;
}

cbuffer DisplayConstants : register(b0) {
    float2 inverse_drawable_size;
    float animation_time_seconds;
    float scene_age_seconds;
};

struct RasterVertex {
    float4 position : SV_Position;
    float2 local_position : TEXCOORD0;
    float2 rectangle_size : TEXCOORD1;
    nointerpolation uint metadata : TEXCOORD2;
};

RasterVertex overlay_vertex(uint vertex_id : SV_VertexID,
                            uint instance_id : SV_InstanceID) {
    static const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(0, 1),
        float2(0, 1), float2(1, 0), float2(1, 1)};
    const uint2 packed = render_rects[instance_id];
    const float2 origin = float2(low16(packed.x), high16(packed.x)) * 0.125;
    const float2 size = float2(low16(packed.y), high16(packed.y)) * 0.125;
    const float2 corner = corners[vertex_id];
    const float2 pixel = origin + corner * size;
    RasterVertex result;
    result.position = float4(pixel.x * inverse_drawable_size.x * 2 - 1,
                             1 - pixel.y * inverse_drawable_size.y * 2, 0, 1);
    result.local_position = corner * size;
    result.rectangle_size = size;
    result.metadata = render_metadata[instance_id];
    return result;
}

Texture2D<float> glyph_atlas : register(t4);
SamplerState glyph_sampler : register(s0);

float4 unpack_rgba8(uint packed) {
    return float4((packed >> 24) & 0xff, (packed >> 16) & 0xff,
                  (packed >> 8) & 0xff, packed & 0xff) / 255.0;
}

float4 premultiply(float4 color) { return float4(color.rgb * color.a, color.a); }

float rounded_distance(float2 sample_position, float2 size, float radius) {
    radius = clamp(radius, 0.0, min(size.x, size.y) * 0.5);
    const float2 centered = abs(sample_position - size * 0.5) -
                            (size * 0.5 - radius);
    return length(max(centered, 0.0)) + min(max(centered.x, centered.y), 0.0) - radius;
}

float coverage(float distance) {
    const float width = max(fwidth(distance), 0.5);
    return 1.0 - smoothstep(-width, width, distance);
}

float4 overlay_pixel(RasterVertex input) : SV_Target {
    const uint target_index = input.metadata & target_mask;
    const uint style_index = (input.metadata & style_mask) >> style_shift;
    const uint kind = (input.metadata & kind_mask) >> kind_shift;
    const uint style_base = style_index * style_stride;
    const bool animated = (render_styles.Load(style_base + 40) & 1) != 0;
    const float reveal = animated ? smoothstep(0, 0.12, scene_age_seconds) : 1;
    if (kind == outline_kind) {
        return premultiply(unpack_rgba8(render_styles.Load(style_base))) * reveal;
    }
    if (kind == active_kind) {
        const float radius = high16(render_styles.Load(style_base + 20)) * 0.125;
        const float outer = coverage(rounded_distance(
            input.local_position, input.rectangle_size, radius));
        const float stroke = min(low16(render_styles.Load(style_base + 36)) * 0.125,
                                 min(input.rectangle_size.x,
                                     input.rectangle_size.y) * 0.5);
        const float2 inner_size = max(input.rectangle_size - stroke * 2, 0);
        const float inner = stroke == 0 ? outer : coverage(rounded_distance(
            input.local_position - stroke, inner_size, max(radius - stroke, 0)));
        const float outline = max(outer - inner, 0);
        const float4 fill = premultiply(unpack_rgba8(
            render_styles.Load(style_base + 12))) * outer;
        const float4 border = premultiply(unpack_rgba8(
            render_styles.Load(style_base + 16))) * outline;
        const float pulse = animated ? 0.88 + 0.12 * sin(animation_time_seconds * 9.42477796) : 1;
        return (border + fill * (1 - border.a)) * reveal * pulse;
    }
    const float radius = high16(render_styles.Load(style_base + 24)) * 0.125;
    const float background = coverage(rounded_distance(
        input.local_position, input.rectangle_size, radius));
    const float padding = low16(render_styles.Load(style_base + 28)) * 0.125;
    const float glyph_width = high16(render_styles.Load(style_base + 28)) * 0.125;
    const float glyph_height = low16(render_styles.Load(style_base + 32)) * 0.125;
    const float advance = high16(render_styles.Load(style_base + 32)) * 0.125;
    const float relative_x = input.local_position.x - padding;
    float glyph = 0;
    if (advance > 0 && glyph_width > 0 && glyph_height > 0 && relative_x >= 0) {
        const uint slot = (uint)(relative_x / advance);
        const float glyph_x = relative_x - slot * advance;
        const float glyph_y = input.local_position.y -
                              (input.rectangle_size.y - glyph_height) * 0.5;
        const uint glyph_count = render_target_glyph_count(target_index);
        if (slot < glyph_count && glyph_x >= 0 && glyph_x < glyph_width &&
            glyph_y >= 0 && glyph_y < glyph_height) {
            const uint glyph_index = min(render_target_glyph(target_index, slot), 31);
            const float2 cell = float2(glyph_index % 8, glyph_index / 8);
            const float2 local = float2(glyph_x / glyph_width, glyph_y / glyph_height);
            glyph = glyph_atlas.Sample(glyph_sampler, (cell + local) / float2(8, 4));
        }
    }
    const float4 background_color = premultiply(unpack_rgba8(
        render_styles.Load(style_base + 4))) * background;
    const float4 foreground_color = premultiply(unpack_rgba8(
        render_styles.Load(style_base + 8))) * glyph * background;
    return (foreground_color + background_color * (1 - foreground_color.a)) * reveal;
}
