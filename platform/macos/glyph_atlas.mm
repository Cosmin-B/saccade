#include "platform/macos/glyph_atlas.hpp"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstring>

namespace saccade::platform::macos {
namespace {

constexpr CGFloat atlas_font_size = 48.0;

NSFont* atlas_font(const application::AppearanceSettings& appearance) noexcept {
    const CGFloat weight = std::clamp((static_cast<CGFloat>(appearance.font_weight) - 400.0) / 625.0, -0.8, 0.62);
    if (std::strcmp(appearance.font_family.data(), "system-ui") == 0) {
        return [NSFont systemFontOfSize:atlas_font_size weight:weight];
    }

    NSString* family = [NSString stringWithUTF8String:appearance.font_family.data()];
    if (family == nil) return nil;
    NSDictionary* traits = @{NSFontWeightTrait : @(weight)};
    NSFontDescriptor* descriptor = [NSFontDescriptor
        fontDescriptorWithFontAttributes:@{NSFontFamilyAttribute : family, NSFontTraitsAttribute : traits}];
    return [NSFont fontWithDescriptor:descriptor size:atlas_font_size];
}

void flip_rows(overlay::GlyphAtlasStorage* atlas) noexcept {
    for (uint32_t top = 0, bottom = overlay::glyph_atlas_height - 1U; top < bottom; ++top, --bottom) {
        uint8_t* top_row = atlas->pixels.data() + static_cast<size_t>(top) * overlay::glyph_atlas_width;
        uint8_t* bottom_row = atlas->pixels.data() + static_cast<size_t>(bottom) * overlay::glyph_atlas_width;
        for (uint32_t column = 0; column < overlay::glyph_atlas_width; ++column)
            std::swap(top_row[column], bottom_row[column]);
    }
}

} // namespace

SaccadeResult rasterize_glyph_atlas(const application::SettingsDocument& settings,
                                    overlay::GlyphAtlasStorage* output) noexcept {
    if (output == nullptr || settings.hints.alphabet_count < 2 ||
        settings.hints.alphabet_count > overlay::glyph_atlas_capacity) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    output->pixels.fill(0);
    output->symbols.fill(0);
    output->glyph_count = 0;

    @autoreleasepool {
        NSFont* font = atlas_font(settings.appearance);
        if (font == nil) return SACCADE_ERROR_NOT_FOUND;
        unsigned char* planes[5]{output->pixels.data(), nullptr, nullptr, nullptr, nullptr};
        NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:planes
                                                                           pixelsWide:overlay::glyph_atlas_width
                                                                           pixelsHigh:overlay::glyph_atlas_height
                                                                        bitsPerSample:8
                                                                      samplesPerPixel:1
                                                                             hasAlpha:NO
                                                                             isPlanar:NO
                                                                       colorSpaceName:NSDeviceWhiteColorSpace
                                                                         bitmapFormat:0
                                                                          bytesPerRow:overlay::glyph_atlas_width
                                                                         bitsPerPixel:8];
        NSGraphicsContext* context = bitmap == nil ? nil : [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
        if (context == nil) return SACCADE_ERROR_BACKEND;
        NSDictionary* attributes = @{NSFontAttributeName : font, NSForegroundColorAttributeName : NSColor.whiteColor};

        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:context];
        context.shouldAntialias = YES;
        for (uint32_t index = 0; index < settings.hints.alphabet_count; ++index) {
            const unichar character = settings.hints.alphabet[index];
            if (character >= 0xd800 && character <= 0xdfff) {
                [NSGraphicsContext restoreGraphicsState];
                return SACCADE_ERROR_UNSUPPORTED;
            }
            NSString* text = [[NSString alloc] initWithCharacters:&character length:1];
            const NSSize size = [text sizeWithAttributes:attributes];
            const uint32_t column = index % overlay::glyph_atlas_columns;
            const uint32_t row = index / overlay::glyph_atlas_columns;
            const NSPoint origin{
                column * overlay::glyph_atlas_cell_width + (overlay::glyph_atlas_cell_width - size.width) * 0.5,
                row * overlay::glyph_atlas_cell_height + (overlay::glyph_atlas_cell_height - size.height) * 0.5};
            [text drawAtPoint:origin withAttributes:attributes];
            output->symbols[index] = character;
        }
        [context flushGraphics];
        [NSGraphicsContext restoreGraphicsState];
    }
    flip_rows(output);
    output->glyph_count = settings.hints.alphabet_count;
    return SACCADE_OK;
}

} // namespace saccade::platform::macos
