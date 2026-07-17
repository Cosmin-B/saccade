#include "platform/windows/glyph_atlas.hpp"

#include <cstdint>
#include <cstring>

namespace {

bool glyph_nonempty(const saccade::overlay::GlyphAtlasStorage& atlas, uint32_t glyph) noexcept {
    const uint32_t cell_x = (glyph % saccade::overlay::glyph_atlas_columns) * saccade::overlay::glyph_atlas_cell_width;
    const uint32_t cell_y = (glyph / saccade::overlay::glyph_atlas_columns) * saccade::overlay::glyph_atlas_cell_height;
    for (uint32_t y = 0; y < saccade::overlay::glyph_atlas_cell_height; ++y) {
        for (uint32_t x = 0; x < saccade::overlay::glyph_atlas_cell_width; ++x) {
            if (atlas.pixels[static_cast<size_t>(cell_y + y) * saccade::overlay::glyph_atlas_width + cell_x + x] != 0)
                return true;
        }
    }
    return false;
}

} // namespace

int main() {
    using namespace saccade;
    application::SettingsDocument settings = application::default_settings();
    overlay::GlyphAtlasStorage regular{};
    if (platform::windows::rasterize_glyph_atlas(settings, &regular) != SACCADE_OK ||
        regular.glyph_count != settings.hints.alphabet_count) {
        return 1;
    }
    for (uint32_t index = 0; index < regular.glyph_count; ++index) {
        if (regular.symbols[index] != settings.hints.alphabet[index] || !glyph_nonempty(regular, index)) return 2;
    }

    settings.appearance.font_weight = 900;
    std::memset(settings.appearance.font_family.data(), 0, settings.appearance.font_family.size());
    constexpr char family[] = "Consolas";
    std::memcpy(settings.appearance.font_family.data(), family, sizeof(family));
    overlay::GlyphAtlasStorage heavy{};
    if (platform::windows::rasterize_glyph_atlas(settings, &heavy) != SACCADE_OK ||
        std::memcmp(regular.pixels.data(), heavy.pixels.data(), regular.pixels.size()) == 0) {
        return 3;
    }
    return 0;
}
