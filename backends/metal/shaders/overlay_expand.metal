#include <metal_stdlib>

using namespace metal;

struct OverlayTarget {
    ulong target_id;
    ushort x_q3;
    ushort y_q3;
    ushort width_q3;
    ushort height_q3;
    ushort label_x_q3;
    ushort label_y_q3;
    ushort confidence_q16;
    uchar glyphs[16];
    uchar style_index;
    uchar glyph_count;
    ushort flags;
    ushort reserved;
};

struct OverlayStyle {
    uint target_outline_rgba8;
    uint label_background_rgba8;
    uint label_foreground_rgba8;
    uint active_fill_rgba8;
    uint active_outline_rgba8;
    ushort target_stroke_q3;
    ushort target_radius_q3;
    ushort label_height_q3;
    ushort label_radius_q3;
    ushort label_padding_x_q3;
    ushort glyph_width_q3;
    ushort glyph_height_q3;
    ushort glyph_advance_q3;
    ushort active_stroke_q3;
    ushort reserved16;
    uint flags;
    uint reserved32;
    ulong reserved0;
    ulong reserved1;
};

struct OverlayRect {
    ushort x_q3;
    ushort y_q3;
    ushort width_q3;
    ushort height_q3;
};

struct ExpandParameters {
    uint target_count;
    uint active_target_index;
    uint static_instance_count;
    uint has_active_target;
};

struct DrawArguments {
    uint vertex_count;
    uint instance_count;
    uint vertex_start;
    uint base_instance;
};

struct DisplayConstants {
    float2 inverse_drawable_size;
    float animation_time_seconds;
    float scene_age_seconds;
};

struct RasterVertex {
    float4 position [[position]];
    float2 local_position;
    float2 rectangle_size;
    uint metadata;
};

static_assert(sizeof(OverlayTarget) == 48, "OverlayTarget layout");
static_assert(sizeof(OverlayStyle) == 64, "OverlayStyle layout");
static_assert(sizeof(OverlayRect) == 8, "OverlayRect layout");
static_assert(sizeof(ExpandParameters) == 16, "ExpandParameters layout");
static_assert(sizeof(DrawArguments) == 16, "DrawArguments layout");
static_assert(sizeof(DisplayConstants) == 16, "DisplayConstants layout");

constant uint target_mask = 0x00003FFF;
constant uint style_shift = 14;
constant uint style_mask = 0x0003C000;
constant uint kind_shift = 18;
constant uint kind_mask = 0x001C0000;
constant uint outline_kind = 1;
constant uint label_kind = 2;
constant uint active_kind = 3;
constant uint animated_style = 1;

uint instance_metadata(uint target_index, uint style_index, uint kind) {
    return (target_index & target_mask) | ((style_index << style_shift) & style_mask) |
           ((kind << kind_shift) & kind_mask);
}

OverlayRect make_rect(ushort x_q3, ushort y_q3, ushort width_q3, ushort height_q3) {
    return OverlayRect{x_q3, y_q3, width_q3, height_q3};
}

kernel void saccade_expand_static(device const OverlayTarget* targets [[buffer(0)]],
                                  device const OverlayStyle* styles [[buffer(1)]],
                                  device OverlayRect* rects [[buffer(2)]], device uint* metadata [[buffer(3)]],
                                  constant ExpandParameters& parameters [[buffer(4)]],
                                  uint index [[thread_position_in_grid]]) {
    if (index >= parameters.target_count) {
        return;
    }

    const OverlayTarget target = targets[index];
    const OverlayStyle style = styles[target.style_index];
    const ushort stroke = min(style.target_stroke_q3, min(ushort(target.width_q3 / 2), ushort(target.height_q3 / 2)));
    const ushort right_x = ushort(target.x_q3 + target.width_q3 - stroke);
    const ushort bottom_y = ushort(target.y_q3 + target.height_q3 - stroke);
    const uint destination = index * 5;
    const uint outline_metadata = instance_metadata(index, target.style_index, outline_kind);

    rects[destination] = make_rect(target.x_q3, target.y_q3, target.width_q3, stroke);
    metadata[destination] = outline_metadata;
    rects[destination + 1] = make_rect(target.x_q3, bottom_y, target.width_q3, stroke);
    metadata[destination + 1] = outline_metadata;
    rects[destination + 2] = make_rect(target.x_q3, target.y_q3, stroke, target.height_q3);
    metadata[destination + 2] = outline_metadata;
    rects[destination + 3] = make_rect(right_x, target.y_q3, stroke, target.height_q3);
    metadata[destination + 3] = outline_metadata;

    const ushort label_width = ushort(style.label_padding_x_q3 * 2 + style.glyph_advance_q3 * target.glyph_count);
    rects[destination + 4] = make_rect(target.label_x_q3, target.label_y_q3, label_width, style.label_height_q3);
    metadata[destination + 4] = instance_metadata(index, target.style_index, label_kind);
}

kernel void saccade_update_active(device const OverlayTarget* targets [[buffer(0)]],
                                  device const OverlayStyle* styles [[buffer(1)]],
                                  device OverlayRect* rects [[buffer(2)]], device uint* metadata [[buffer(3)]],
                                  constant ExpandParameters& parameters [[buffer(4)]],
                                  device DrawArguments* arguments [[buffer(5)]],
                                  uint index [[thread_position_in_grid]]) {
    if (index != 0) {
        return;
    }

    uint instance_count = parameters.static_instance_count;
    if (parameters.has_active_target != 0) {
        const uint target_index = parameters.active_target_index;
        const OverlayTarget target = targets[target_index];
        rects[instance_count] = make_rect(target.x_q3, target.y_q3, target.width_q3, target.height_q3);
        metadata[instance_count] = instance_metadata(target_index, target.style_index, active_kind);
        ++instance_count;
    }

    arguments->vertex_count = 6;
    arguments->instance_count = instance_count;
    arguments->vertex_start = 0;
    arguments->base_instance = 0;
}

float4 unpack_rgba8(uint packed) {
    return float4(float((packed >> 24) & 0xFF), float((packed >> 16) & 0xFF), float((packed >> 8) & 0xFF),
                  float(packed & 0xFF)) /
           255.0;
}

float4 premultiply(float4 color) {
    return float4(color.rgb * color.a, color.a);
}

float rounded_distance(float2 point, float2 size, float radius) {
    radius = clamp(radius, 0.0, min(size.x, size.y) * 0.5);
    const float2 centered = abs(point - size * 0.5) - (size * 0.5 - radius);
    return length(max(centered, 0.0)) + min(max(centered.x, centered.y), 0.0) - radius;
}

float coverage(float distance) {
    const float width = max(fwidth(distance), 0.5);
    return 1.0 - smoothstep(-width, width, distance);
}

vertex RasterVertex saccade_overlay_vertex(uint vertex_id [[vertex_id]], uint instance_id [[instance_id]],
                                           device const OverlayRect* rects [[buffer(2)]],
                                           device const uint* metadata [[buffer(3)]],
                                           constant DisplayConstants& display [[buffer(6)]]) {
    const float2 corners[6] = {float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
                               float2(0.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0)};
    const OverlayRect rect = rects[instance_id];
    const float2 origin = float2(rect.x_q3, rect.y_q3) * 0.125;
    const float2 size = float2(rect.width_q3, rect.height_q3) * 0.125;
    const float2 corner = corners[vertex_id];
    const float2 pixel = origin + corner * size;

    RasterVertex result;
    result.position = float4(pixel.x * display.inverse_drawable_size.x * 2.0 - 1.0,
                             1.0 - pixel.y * display.inverse_drawable_size.y * 2.0, 0.0, 1.0);
    result.local_position = corner * size;
    result.rectangle_size = size;
    result.metadata = metadata[instance_id];
    return result;
}

fragment float4 saccade_overlay_fragment(RasterVertex input [[stage_in]],
                                         device const OverlayTarget* targets [[buffer(0)]],
                                         device const OverlayStyle* styles [[buffer(1)]],
                                         constant DisplayConstants& display [[buffer(6)]],
                                         texture2d<float, access::sample> glyph_atlas [[texture(0)]]) {
    const uint target_index = input.metadata & target_mask;
    const uint style_index = (input.metadata & style_mask) >> style_shift;
    const uint kind = (input.metadata & kind_mask) >> kind_shift;
    const OverlayTarget target = targets[target_index];
    const OverlayStyle style = styles[style_index];
    const bool animated = (style.flags & animated_style) != 0;
    const float reveal = animated ? smoothstep(0.0, 0.12, display.scene_age_seconds) : 1.0;

    if (kind == outline_kind) {
        return premultiply(unpack_rgba8(style.target_outline_rgba8)) * reveal;
    }

    if (kind == active_kind) {
        const float radius = float(style.target_radius_q3) * 0.125;
        const float outer = coverage(rounded_distance(input.local_position, input.rectangle_size, radius));
        const float stroke =
            min(float(style.active_stroke_q3) * 0.125, min(input.rectangle_size.x, input.rectangle_size.y) * 0.5);
        const float2 inner_size = max(input.rectangle_size - stroke * 2.0, 0.0);
        const float inner =
            stroke == 0.0
                ? outer
                : coverage(rounded_distance(input.local_position - stroke, inner_size, max(radius - stroke, 0.0)));
        const float outline = max(outer - inner, 0.0);
        const float4 fill_color = premultiply(unpack_rgba8(style.active_fill_rgba8));
        const float4 outline_color = premultiply(unpack_rgba8(style.active_outline_rgba8));
        const float4 fill = fill_color * outer;
        const float4 border = outline_color * outline;
        const float pulse = animated ? 0.88 + 0.12 * sin(display.animation_time_seconds * 9.42477796) : 1.0;
        return (border + fill * (1.0 - border.a)) * reveal * pulse;
    }

    const float radius = float(style.label_radius_q3) * 0.125;
    const float background = coverage(rounded_distance(input.local_position, input.rectangle_size, radius));
    float glyph = 0.0;
    const float padding = float(style.label_padding_x_q3) * 0.125;
    const float advance = float(style.glyph_advance_q3) * 0.125;
    const float glyph_width = float(style.glyph_width_q3) * 0.125;
    const float glyph_height = float(style.glyph_height_q3) * 0.125;
    const float glyph_y = (input.rectangle_size.y - glyph_height) * 0.5;
    const float relative_x = input.local_position.x - padding;
    if (advance > 0.0 && glyph_width > 0.0 && glyph_height > 0.0 && relative_x >= 0.0) {
        const uint glyph_slot = uint(relative_x / advance);
        const float glyph_x = relative_x - float(glyph_slot) * advance;
        const float local_y = input.local_position.y - glyph_y;
        if (glyph_slot < target.glyph_count && glyph_x >= 0.0 && glyph_x < glyph_width && local_y >= 0.0 &&
            local_y < glyph_height) {
            const uint glyph_index = min(uint(target.glyphs[glyph_slot]), 31U);
            constexpr sampler atlas_sampler(coord::normalized, address::clamp_to_edge, filter::linear);
            const float2 cell = float2(float(glyph_index % 8U), float(glyph_index / 8U));
            const float2 local = float2(glyph_x / glyph_width, local_y / glyph_height);
            glyph = glyph_atlas.sample(atlas_sampler, (cell + local) / float2(8.0, 4.0)).r;
        }
    }

    const float4 background_color = premultiply(unpack_rgba8(style.label_background_rgba8)) * background;
    const float4 foreground_color = premultiply(unpack_rgba8(style.label_foreground_rgba8)) * glyph * background;
    return (foreground_color + background_color * (1.0 - foreground_color.a)) * reveal;
}
