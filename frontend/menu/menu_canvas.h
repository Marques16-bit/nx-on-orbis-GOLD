// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: drawing for the menu. A Canvas is a 0xAARRGGBB framebuffer (the console's video-out,
// or a plain array in the host test); nothing here knows about the console.

#pragma once

#include <cstdint>
#include <string_view>

namespace Menu {

/// Colours of the menu (0xAARRGGBB).
namespace Color {
inline constexpr std::uint32_t BackgroundTop = 0xFF0B1020;
inline constexpr std::uint32_t BackgroundBottom = 0xFF1A2442;
inline constexpr std::uint32_t Panel = 0xFF1B2644;
inline constexpr std::uint32_t Selected = 0xFF2B4FA6;
inline constexpr std::uint32_t Accent = 0xFF4A86FF;
inline constexpr std::uint32_t Teal = 0xFF2DD4C4;
inline constexpr std::uint32_t Text = 0xFFEAF0FF;
inline constexpr std::uint32_t Dim = 0xFF8FA0C8;
inline constexpr std::uint32_t Good = 0xFF4ADE80;
inline constexpr std::uint32_t Warn = 0xFFFBBF24;
inline constexpr std::uint32_t Bad = 0xFFF87171;
} // namespace Color

struct Canvas {
    std::uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;

    /// Opaque rectangle, clipped.
    void Fill(int x, int y, int w, int h, std::uint32_t color);
    /// Opaque vertical gradient, clipped.
    void FillGradient(int x, int y, int w, int h, std::uint32_t top, std::uint32_t bottom);
    /// One pixel, source-over: the alpha byte of `color` is its opacity.
    void Blend(int x, int y, std::uint32_t color);
    /// Rectangle with rounded corners, anti-aliased, source-over.
    void RoundRect(int x, int y, int w, int h, int radius, std::uint32_t color);
    /// Ring between the two radii, anti-aliased, source-over.
    void Ring(float cx, float cy, float outer, float inner, std::uint32_t color);
    void Disc(float cx, float cy, float radius, std::uint32_t color);
    /// UTF-8 text in the menu font (20x40 cells; `scale` multiplies the cell). Characters the
    /// font does not have are drawn as '?'. Returns the width drawn, in pixels.
    int Print(int x, int y, std::string_view text, std::uint32_t color, int scale = 1);
};

inline constexpr int GlyphWidth = 20;
inline constexpr int GlyphHeight = 40;

/// Number of characters (code points) in a UTF-8 string.
int TextCells(std::string_view utf8);
inline int TextWidth(std::string_view utf8, int scale = 1) {
    return TextCells(utf8) * GlyphWidth * scale;
}

} // namespace Menu
