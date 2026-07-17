#include "platform/windows/glyph_atlas.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace saccade::platform::windows {
namespace {

bool font_family(const application::AppearanceSettings& appearance, std::array<wchar_t, 64>* output) noexcept {
    if (std::strcmp(appearance.font_family.data(), "system-ui") == 0) {
        constexpr wchar_t system_family[] = L"Segoe UI";
        std::copy(std::begin(system_family), std::end(system_family), output->begin());
        return true;
    }

    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, appearance.font_family.data(), -1, output->data(),
                               static_cast<int>(output->size())) > 0;
}

} // namespace

SaccadeResult rasterize_glyph_atlas(const application::SettingsDocument& settings,
                                    overlay::GlyphAtlasStorage* output) noexcept {
    if (output == nullptr || settings.hints.alphabet_count < 2 ||
        settings.hints.alphabet_count > overlay::glyph_atlas_capacity) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    std::array<wchar_t, 64> family{};
    if (!font_family(settings.appearance, &family)) return SACCADE_ERROR_INVALID_ARGUMENT;

    output->pixels.fill(0);
    output->symbols.fill(0);
    output->glyph_count = 0;

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = overlay::glyph_atlas_width;
    bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(overlay::glyph_atlas_height);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* bitmap_pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmap_info, DIB_RGB_COLORS, &bitmap_pixels, nullptr, 0);
    HDC context = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-48, 0, 0, 0, static_cast<LONG>(settings.appearance.font_weight), FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, family.data());
    if (bitmap == nullptr || bitmap_pixels == nullptr || context == nullptr || font == nullptr) {
        if (font != nullptr) (void)DeleteObject(font);
        if (context != nullptr) (void)DeleteDC(context);
        if (bitmap != nullptr) (void)DeleteObject(bitmap);
        return SACCADE_ERROR_BACKEND;
    }

    HGDIOBJ previous_bitmap = SelectObject(context, bitmap);
    HGDIOBJ previous_font = SelectObject(context, font);
    (void)SetBkMode(context, TRANSPARENT);
    (void)SetTextColor(context, RGB(255, 255, 255));
    (void)SetTextAlign(context, TA_LEFT | TA_TOP);
    (void)PatBlt(context, 0, 0, overlay::glyph_atlas_width, overlay::glyph_atlas_height, BLACKNESS);

    for (uint32_t index = 0; index < settings.hints.alphabet_count; ++index) {
        const uint16_t symbol = settings.hints.alphabet[index];
        if (symbol >= 0xd800 && symbol <= 0xdfff) {
            (void)SelectObject(context, previous_font);
            (void)SelectObject(context, previous_bitmap);
            (void)DeleteObject(font);
            (void)DeleteDC(context);
            (void)DeleteObject(bitmap);
            return SACCADE_ERROR_UNSUPPORTED;
        }

        wchar_t text[2]{static_cast<wchar_t>(symbol), L'\0'};
        const uint32_t column = index % overlay::glyph_atlas_columns;
        const uint32_t row = index / overlay::glyph_atlas_columns;
        RECT bounds{static_cast<LONG>(column * overlay::glyph_atlas_cell_width),
                    static_cast<LONG>(row * overlay::glyph_atlas_cell_height),
                    static_cast<LONG>((column + 1U) * overlay::glyph_atlas_cell_width),
                    static_cast<LONG>((row + 1U) * overlay::glyph_atlas_cell_height)};
        if (DrawTextW(context, text, 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX) == 0) {
            (void)SelectObject(context, previous_font);
            (void)SelectObject(context, previous_bitmap);
            (void)DeleteObject(font);
            (void)DeleteDC(context);
            (void)DeleteObject(bitmap);
            return SACCADE_ERROR_BACKEND;
        }
        output->symbols[index] = symbol;
    }

    const auto* bgra = static_cast<const uint8_t*>(bitmap_pixels);
    for (size_t index = 0; index < overlay::glyph_atlas_bytes; ++index) {
        output->pixels[index] = std::max({bgra[index * 4U], bgra[index * 4U + 1U], bgra[index * 4U + 2U]});
    }

    (void)SelectObject(context, previous_font);
    (void)SelectObject(context, previous_bitmap);
    (void)DeleteObject(font);
    (void)DeleteDC(context);
    (void)DeleteObject(bitmap);
    output->glyph_count = settings.hints.alphabet_count;
    return SACCADE_OK;
}

} // namespace saccade::platform::windows
