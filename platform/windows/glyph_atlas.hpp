#ifndef SACCADE_PLATFORM_WINDOWS_GLYPH_ATLAS_HPP
#define SACCADE_PLATFORM_WINDOWS_GLYPH_ATLAS_HPP

#include "application/settings.hpp"
#include "overlay/glyph_atlas.hpp"

namespace saccade::platform::windows {

SaccadeResult rasterize_glyph_atlas(const application::SettingsDocument&, overlay::GlyphAtlasStorage*) noexcept;

} // namespace saccade::platform::windows

#endif
