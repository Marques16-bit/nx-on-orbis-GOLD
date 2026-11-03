// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: drawing for the menu. A Canvas is a 0xAARRGGBB framebuffer (the console's video-out,
// or a plain array in the host test); nothing here knows about the console.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Menu {

/// Colours of the menu (0xAARRGGBB).
namespace Color {
// 0.7.10: these six follow the menu's colour theme (ApplyTheme below); the starting values are the "blue" theme.
inline std::uint32_t BackgroundTop = 0xFF0B1020;
inline std::uint32_t BackgroundBottom = 0xFF1A2442;
inline std::uint32_t Panel = 0xFF1B2644;
inline std::uint32_t Selected = 0xFF2B4FA6;
inline std::uint32_t Accent = 0xFF4A86FF;
inline std::uint32_t Teal = 0xFF2DD4C4; // the second colour: marks, changed values
inline constexpr std::uint32_t Text = 0xFFEAF0FF;
inline constexpr std::uint32_t Dim = 0xFF8FA0C8;
inline constexpr std::uint32_t Good = 0xFF4ADE80;
inline constexpr std::uint32_t Warn = 0xFFFBBF24;
inline constexpr std::uint32_t Bad = 0xFFF87171;
} // namespace Color

/// 0.7.10: the colour themes of the menu (settings.txt: menu_theme=blue|green|purple|orange|pink|red|grey).
/// `ApplyTheme` sets the Color:: values above; an index out of range is the first theme (blue).
int ThemeCount();
void ApplyTheme(int index);
/// The accent colour of a theme, for a swatch in a list.
std::uint32_t ThemeAccent(int index);

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
    /// 0.7.10: a line with round ends (anti-aliased, source-over), `width` pixels thick.
    void Line(float x0, float y0, float x1, float y1, float width, std::uint32_t color);
    /// 0.7.9: rounded rectangle filled with a vertical gradient (anti-aliased corners, source-over).
    void RoundRectGradient(int x, int y, int w, int h, int radius, std::uint32_t top, std::uint32_t bottom);
    /// 0.7.9: a picture (0xAARRGGBB, `w` x `h`, row by row) at (x, y), source-over, with the corners rounded
    /// when `radius` > 0 (a game cover).
    void DrawImage(int x, int y, const std::uint32_t* pixels, int w, int h, int radius = 0);
    /// UTF-8 text in the menu font (20x40 cells; `scale` multiplies the cell). Characters the
    /// font does not have are drawn as '?'. Returns the width drawn, in pixels.
    int Print(int x, int y, std::string_view text, std::uint32_t color, int scale = 1);
};

/// 0.7.10: icons drawn with lines and shapes (no pictures needed), centred at (cx, cy) in a box of `size`
/// pixels. `fg` is the icon's colour and `bg` the colour of what is behind it (holes are drawn in it).
enum class Icon { Gamepad, Monitor, Chip, Sliders, Gear, Document, Gauge, Palette, Folder, Play };
void DrawIcon(Canvas& c, Icon icon, float cx, float cy, float size, std::uint32_t fg, std::uint32_t bg);

inline constexpr int GlyphWidth = 20;
inline constexpr int GlyphHeight = 40;

/// The next code point of a UTF-8 string, advancing `i` (an invalid byte comes out as '?').
std::uint32_t NextCodePoint(std::string_view text, size_t& i);
/// The 20x40 coverage bitmap (8 bit) of a character; '?' for one the font lacks.
const std::uint8_t* GlyphFor(std::uint32_t code_point);

/// Number of characters (code points) in a UTF-8 string.
int TextCells(std::string_view utf8);
inline int TextWidth(std::string_view utf8, int scale = 1) {
    return TextCells(utf8) * GlyphWidth * scale;
}

} // namespace Menu
