// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: drawing for the menu. See menu_canvas.h.

#include <algorithm>
#include <cmath>

#include "menu/menu_canvas.h"
#include "menu/menu_font.h"
#include "menu/menu_font_latin.h"

namespace Menu {

std::uint32_t NextCodePoint(std::string_view s, size_t& i) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = byte(i++);
    if (c < 0x80) {
        return c;
    }
    int extra = 0;
    std::uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07;
    } else {
        return '?';
    }
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (byte(i) & 0xC0) != 0x80) {
            return '?';
        }
        cp = (cp << 6) | (byte(i++) & 0x3F);
    }
    return cp;
}

const std::uint8_t* GlyphFor(std::uint32_t cp) {
    if (cp >= 32 && cp <= 126) {
        return MenuFont::Glyphs[cp - 32];
    }
    for (int i = 0; i < MenuFont::LatinCount; ++i) {
        if (MenuFont::LatinCodepoints[i] == cp) {
            return MenuFont::LatinGlyphs[i];
        }
    }
    return MenuFont::Glyphs['?' - 32];
}

namespace {

float Clamp01(float v) {
    return std::min(std::max(v, 0.0f), 1.0f);
}

std::uint32_t WithAlpha(std::uint32_t color, float coverage) {
    const unsigned a = static_cast<unsigned>(((color >> 24) & 0xFF) * Clamp01(coverage) + 0.5f);
    return (color & 0x00FFFFFFu) | (a << 24);
}

std::uint32_t Hsv(float h, float s, float v) {
    h = h - std::floor(h);
    const float f = h * 6.0f - std::floor(h * 6.0f);
    const float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    float r = v, g = t, b = p;
    switch (int(std::floor(h * 6.0f)) % 6) {
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
    default: break;
    }
    const auto byte = [](float x) { return std::uint32_t(Clamp01(x) * 255.0f + 0.5f); };
    return 0xFF000000u | (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

/// One colour theme: the hue of the background and the accent, the hue of the second colour, and how
/// colourful the background is (the grey theme has almost none).
struct ThemeSpec {
    float hue, hue2, saturation;
};
// Same order as the choices of the menu_theme option in settings_model.cpp (a test keeps them in step).
constexpr ThemeSpec Themes[] = {
    {0.615f, 0.485f, 1.00f}, // blue (the original look)
    {0.380f, 0.130f, 1.00f}, // green
    {0.760f, 0.500f, 1.00f}, // purple
    {0.070f, 0.560f, 1.00f}, // orange
    {0.925f, 0.480f, 1.00f}, // pink
    {0.995f, 0.130f, 1.00f}, // red
    {0.600f, 0.130f, 0.12f}, // grey
};

// The grey theme has almost no colour, so its accent is darker: white text on it must stay readable.
std::uint32_t AccentOf(const ThemeSpec& t) {
    return t.saturation < 0.5f ? Hsv(t.hue, 0.70f * t.saturation, 0.58f) : Hsv(t.hue, 0.70f, 1.00f);
}

} // Anonymous namespace

int ThemeCount() {
    return int(sizeof(Themes) / sizeof(Themes[0]));
}

void ApplyTheme(int index) {
    if (index <= 0 || index >= ThemeCount()) {
        // the original colours exactly
        Color::BackgroundTop = 0xFF0B1020;
        Color::BackgroundBottom = 0xFF1A2442;
        Color::Panel = 0xFF1B2644;
        Color::Selected = 0xFF2B4FA6;
        Color::Accent = 0xFF4A86FF;
        Color::Teal = 0xFF2DD4C4;
        return;
    }
    const ThemeSpec& t = Themes[index];
    Color::BackgroundTop = Hsv(t.hue, 0.64f * t.saturation, 0.13f);
    Color::BackgroundBottom = Hsv(t.hue, 0.58f * t.saturation, 0.27f);
    Color::Panel = Hsv(t.hue, 0.58f * t.saturation, 0.29f);
    Color::Selected = Hsv(t.hue, 0.72f * t.saturation, t.saturation < 0.5f ? 0.40f : 0.62f);
    Color::Accent = AccentOf(t);
    Color::Teal = Hsv(t.hue2, 0.79f * (t.saturation > 0.5f ? 1.0f : 0.0f) + 0.12f, 0.85f);
}

std::uint32_t ThemeAccent(int index) {
    if (index <= 0 || index >= ThemeCount()) {
        return 0xFF4A86FF;
    }
    return AccentOf(Themes[index]);
}

int TextCells(std::string_view utf8) {
    int cells = 0;
    for (size_t i = 0; i < utf8.size();) {
        NextCodePoint(utf8, i);
        ++cells;
    }
    return cells;
}

void Canvas::Fill(int x, int y, int w, int h, std::uint32_t color) {
    const int x0 = std::max(x, 0), x1 = std::min(x + w, width);
    for (int j = std::max(y, 0); j < std::min(y + h, height) && x0 < x1; ++j) {
        std::fill(pixels + size_t(j) * width + x0, pixels + size_t(j) * width + x1, color | 0xFF000000u);
    }
}

void Canvas::FillGradient(int x, int y, int w, int h, std::uint32_t top, std::uint32_t bottom) {
    for (int j = 0; j < h; ++j) {
        const float t = h > 1 ? float(j) / float(h - 1) : 0.0f;
        std::uint32_t color = 0xFF000000u;
        for (int shift : {16, 8, 0}) {
            const float a = float((top >> shift) & 0xFF), b = float((bottom >> shift) & 0xFF);
            color |= std::uint32_t(a + (b - a) * t + 0.5f) << shift;
        }
        Fill(x, y + j, w, 1, color);
    }
}

void Canvas::Blend(int x, int y, std::uint32_t color) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    const unsigned a = color >> 24;
    if (a == 0) {
        return;
    }
    std::uint32_t& dst = pixels[size_t(y) * width + x];
    std::uint32_t out = 0xFF000000u;
    for (int shift : {16, 8, 0}) {
        const unsigned s = (color >> shift) & 0xFF, d = (dst >> shift) & 0xFF;
        out |= ((s * a + d * (255 - a) + 127) / 255) << shift;
    }
    dst = out;
}

void Canvas::RoundRect(int x, int y, int w, int h, int radius, std::uint32_t color) {
    radius = std::min({radius, w / 2, h / 2});
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            // Distance from the pixel centre to the nearest corner circle, when in a corner.
            const float cx = i < radius ? radius - 0.5f : (i >= w - radius ? w - radius - 0.5f : -1.0f);
            const float cy = j < radius ? radius - 0.5f : (j >= h - radius ? h - radius - 0.5f : -1.0f);
            float coverage = 1.0f;
            if (cx >= 0.0f && cy >= 0.0f) {
                const float d = std::sqrt((i - cx) * (i - cx) + (j - cy) * (j - cy));
                coverage = Clamp01(float(radius) - d + 0.5f);
            }
            Blend(x + i, y + j, WithAlpha(color, coverage));
        }
    }
}

namespace {
/// Coverage (0..1) of pixel (i, j) of a w x h rectangle with rounded corners of `radius`.
float CornerCoverage(int i, int j, int w, int h, int radius) {
    const float cx = i < radius ? radius - 0.5f : (i >= w - radius ? w - radius - 0.5f : -1.0f);
    const float cy = j < radius ? radius - 0.5f : (j >= h - radius ? h - radius - 0.5f : -1.0f);
    if (cx < 0.0f || cy < 0.0f) {
        return 1.0f;
    }
    const float d = std::sqrt((i - cx) * (i - cx) + (j - cy) * (j - cy));
    return Clamp01(float(radius) - d + 0.5f);
}
} // namespace

void Canvas::RoundRectGradient(int x, int y, int w, int h, int radius, std::uint32_t top, std::uint32_t bottom) {
    radius = std::min({radius, w / 2, h / 2});
    for (int j = 0; j < h; ++j) {
        const float t = h > 1 ? float(j) / float(h - 1) : 0.0f;
        const auto mix = [&](int shift) {
            const float a = float((top >> shift) & 0xFF), b = float((bottom >> shift) & 0xFF);
            return std::uint32_t(a + (b - a) * t + 0.5f) & 0xFF;
        };
        const std::uint32_t row = 0xFF000000u | (mix(16) << 16) | (mix(8) << 8) | mix(0);
        for (int i = 0; i < w; ++i) {
            Blend(x + i, y + j, WithAlpha(row, CornerCoverage(i, j, w, h, radius)));
        }
    }
}

void Canvas::DrawImage(int x, int y, const std::uint32_t* pixels, int w, int h, int radius) {
    if (pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    radius = std::min({std::max(radius, 0), w / 2, h / 2});
    for (int j = 0; j < h; ++j) {
        if (y + j < 0 || y + j >= height) {
            continue;
        }
        for (int i = 0; i < w; ++i) {
            const std::uint32_t p = pixels[std::size_t(j) * std::size_t(w) + std::size_t(i)];
            if ((p >> 24) == 0) {
                continue;
            }
            Blend(x + i, y + j, radius > 0 ? WithAlpha(p, CornerCoverage(i, j, w, h, radius)) : p);
        }
    }
}

void Canvas::Ring(float cx, float cy, float outer, float inner, std::uint32_t color) {
    for (int j = int(cy - outer - 1); j <= int(cy + outer + 1); ++j) {
        for (int i = int(cx - outer - 1); i <= int(cx + outer + 1); ++i) {
            const float d = std::sqrt((i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy));
            Blend(i, j, WithAlpha(color, Clamp01(outer - d + 0.5f) * Clamp01(d - inner + 0.5f)));
        }
    }
}

void Canvas::Line(float x0, float y0, float x1, float y1, float width, std::uint32_t color) {
    const float r = width * 0.5f;
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    const int left = int(std::floor(std::min(x0, x1) - r - 1)), right = int(std::ceil(std::max(x0, x1) + r + 1));
    const int top = int(std::floor(std::min(y0, y1) - r - 1)), bottom = int(std::ceil(std::max(y0, y1) + r + 1));
    for (int j = top; j <= bottom; ++j) {
        for (int i = left; i <= right; ++i) {
            const float px = i + 0.5f - x0, py = j + 0.5f - y0;
            const float t = len2 > 0.0f ? Clamp01((px * dx + py * dy) / len2) : 0.0f;
            const float ex = px - t * dx, ey = py - t * dy;
            const float d = std::sqrt(ex * ex + ey * ey);
            const float coverage = Clamp01(r - d + 0.5f);
            if (coverage > 0.0f) {
                Blend(i, j, WithAlpha(color, coverage));
            }
        }
    }
}

void Canvas::Disc(float cx, float cy, float radius, std::uint32_t color) {
    Ring(cx, cy, radius, 0.0f, color);
}

int Canvas::Print(int x, int y, std::string_view text, std::uint32_t color, int scale) {
    const unsigned alpha = color >> 24;
    int pen = x;
    for (size_t i = 0; i < text.size();) {
        const std::uint8_t* glyph = GlyphFor(NextCodePoint(text, i));
        for (int row = 0; row < GlyphHeight; ++row) {
            for (int col = 0; col < GlyphWidth; ++col) {
                const unsigned coverage = glyph[row * GlyphWidth + col];
                if (coverage == 0) {
                    continue;
                }
                const std::uint32_t pixel = (color & 0x00FFFFFFu) | (((coverage * alpha + 127) / 255) << 24);
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        Blend(pen + col * scale + sx, y + row * scale + sy, pixel);
                    }
                }
            }
        }
        pen += GlyphWidth * scale;
    }
    return pen - x;
}

void DrawIcon(Canvas& c, Icon icon, float cx, float cy, float size, std::uint32_t fg, std::uint32_t bg) {
    const float u = size / 40.0f; // everything below is drawn on a 40 x 40 grid
    const auto X = [&](float v) { return cx + (v - 20.0f) * u; };
    const auto Y = [&](float v) { return cy + (v - 20.0f) * u; };
    const auto box = [&](float x, float y, float w, float h, float r, std::uint32_t color) {
        c.RoundRect(int(std::lround(X(x))), int(std::lround(Y(y))), std::max(1, int(std::lround(w * u))),
                    std::max(1, int(std::lround(h * u))), std::max(0, int(std::lround(r * u))), color);
    };
    switch (icon) {
    case Icon::Gamepad:
        box(3, 11, 34, 20, 9, fg);
        box(9, 18, 10, 3.2f, 1.2f, bg); // d-pad
        box(12.4f, 14.6f, 3.2f, 10, 1.2f, bg);
        c.Disc(X(27), Y(17), 2.6f * u, bg); // buttons
        c.Disc(X(31.5f), Y(22), 2.6f * u, bg);
        break;
    case Icon::Monitor:
        box(3, 6, 34, 23, 4, fg);
        box(6, 9, 28, 17, 2, bg);
        box(15, 29, 10, 4, 0, fg); // neck
        box(10, 33, 20, 3, 1.5f, fg); // foot
        c.Line(X(11), Y(21), X(17), Y(14), 2.6f * u, fg); // a "signal" line inside the screen
        c.Line(X(17), Y(14), X(22), Y(20), 2.6f * u, fg);
        c.Line(X(22), Y(20), X(29), Y(12), 2.6f * u, fg);
        break;
    case Icon::Chip:
        for (int k = 0; k < 4; ++k) { // pins, four per side
            const float p = 10.0f + 6.0f * float(k);
            box(p - 1.4f, 3, 2.8f, 6, 1, fg);
            box(p - 1.4f, 31, 2.8f, 6, 1, fg);
            box(3, p - 1.4f, 6, 2.8f, 1, fg);
            box(31, p - 1.4f, 6, 2.8f, 1, fg);
        }
        box(8, 8, 24, 24, 4, fg);
        box(14, 14, 12, 12, 2, bg);
        break;
    case Icon::Sliders:
        for (int k = 0; k < 3; ++k) {
            const float y = 9.0f + 11.0f * float(k);
            const float knob = k == 0 ? 25.0f : (k == 1 ? 13.0f : 28.0f);
            c.Line(X(5), Y(y), X(35), Y(y), 3.2f * u, fg);
            c.Disc(X(knob), Y(y), 5.2f * u, fg);
            c.Disc(X(knob), Y(y), 2.2f * u, bg);
        }
        break;
    case Icon::Gear:
        for (int k = 0; k < 8; ++k) {
            const float a = 3.14159265f * 0.25f * float(k);
            c.Line(X(20 + 12.0f * std::cos(a)), Y(20 + 12.0f * std::sin(a)), X(20 + 16.5f * std::cos(a)),
                   Y(20 + 16.5f * std::sin(a)), 6.2f * u, fg);
        }
        c.Disc(X(20), Y(20), 13.0f * u, fg);
        c.Disc(X(20), Y(20), 5.6f * u, bg);
        break;
    case Icon::Document:
        box(7, 3, 26, 34, 4, fg);
        box(11, 9, 18, 2.6f, 1.2f, bg);
        box(11, 15, 18, 2.6f, 1.2f, bg);
        box(11, 21, 18, 2.6f, 1.2f, bg);
        box(11, 27, 11, 2.6f, 1.2f, bg);
        break;
    case Icon::Gauge:
        c.Ring(X(20), Y(23), 16.5f * u, 12.0f * u, fg);
        box(0, 23, 40, 20, 0, bg); // cut the lower part away: a half ring
        box(3.6f, 22, 33, 3.6f, 1.8f, fg); // the base line
        c.Line(X(20), Y(24), X(28.5f), Y(13.5f), 3.4f * u, fg); // the needle
        c.Disc(X(20), Y(24), 3.6f * u, fg);
        break;
    case Icon::Palette:
        c.Disc(X(20), Y(20), 17.0f * u, fg);
        c.Disc(X(13), Y(16), 3.2f * u, bg);
        c.Disc(X(20), Y(11.5f), 3.2f * u, bg);
        c.Disc(X(27), Y(16), 3.2f * u, bg);
        c.Disc(X(26), Y(26), 4.4f * u, bg);
        break;
    case Icon::Folder:
        box(3, 9, 15, 7, 3, fg);
        box(3, 13, 34, 21, 4, fg);
        break;
    case Icon::Play:
        c.Disc(X(20), Y(20), 17.0f * u, fg);
        // a triangle from three lines, in the background colour
        c.Line(X(16), Y(12.5f), X(16), Y(27.5f), 3.0f * u, bg);
        c.Line(X(16), Y(12.5f), X(28), Y(20), 3.0f * u, bg);
        c.Line(X(16), Y(27.5f), X(28), Y(20), 3.0f * u, bg);
        for (float f = 0.0f; f <= 1.0f; f += 0.08f) { // fill it
            c.Line(X(16), Y(13.0f + 14.0f * f), X(16 + 11.0f * (1.0f - std::abs(2.0f * f - 1.0f))), Y(13.0f + 14.0f * f), 2.2f * u, bg);
        }
        break;
    }
}

} // namespace Menu
