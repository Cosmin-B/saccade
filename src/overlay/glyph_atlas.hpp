#ifndef SACCADE_OVERLAY_GLYPH_ATLAS_HPP
#define SACCADE_OVERLAY_GLYPH_ATLAS_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace saccade::overlay {

constexpr uint32_t glyph_atlas_capacity = 32;
constexpr uint32_t glyph_atlas_columns = 8;
constexpr uint32_t glyph_atlas_rows = 4;
constexpr uint32_t glyph_atlas_cell_width = 64;
constexpr uint32_t glyph_atlas_cell_height = 64;
constexpr uint32_t glyph_atlas_width = glyph_atlas_columns * glyph_atlas_cell_width;
constexpr uint32_t glyph_atlas_height = glyph_atlas_rows * glyph_atlas_cell_height;
constexpr size_t glyph_atlas_bytes = static_cast<size_t>(glyph_atlas_width) * glyph_atlas_height;

struct GlyphAtlasView {
    const uint8_t* pixels = nullptr;
    const uint16_t* symbols = nullptr;
    uint32_t glyph_count = 0;
};

struct GlyphAtlasStorage {
    std::array<uint8_t, glyph_atlas_bytes> pixels{};
    std::array<uint16_t, glyph_atlas_capacity> symbols{};
    uint32_t glyph_count = 0;

    [[nodiscard]] GlyphAtlasView view() const noexcept { return {pixels.data(), symbols.data(), glyph_count}; }
};

inline bool glyph_atlas_valid(GlyphAtlasView atlas) noexcept {
    return atlas.pixels != nullptr && atlas.symbols != nullptr && atlas.glyph_count >= 2 &&
           atlas.glyph_count <= glyph_atlas_capacity;
}

static_assert(glyph_atlas_capacity == glyph_atlas_columns * glyph_atlas_rows);
static_assert(glyph_atlas_bytes == 128U * 1024U);

} // namespace saccade::overlay

#endif
