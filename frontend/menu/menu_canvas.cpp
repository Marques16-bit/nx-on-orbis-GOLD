// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: drawing for the menu. See menu_canvas.h.

#include <algorithm>
#include <cmath>

#include "menu/menu_canvas.h"
#include "menu/menu_font.h"
#include "menu/menu_font_latin.h"

namespace Menu {

namespace {

/// The next code point of a UTF-8 string (invalid bytes come out as '?').
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

float Clamp01(float v) {
    return std::min(std::max(v, 0.0f), 1.0f);
}

std::uint32_t WithAlpha(std::uint32_t color, float coverage) {
    const unsigned a = static_cast<unsigned>(((color >> 24) & 0xFF) * Clamp01(coverage) + 0.5f);
    return (color & 0x00FFFFFFu) | (a << 24);
}

} // Anonymous namespace

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

void Canvas::Ring(float cx, float cy, float outer, float inner, std::uint32_t color) {
    for (int j = int(cy - outer - 1); j <= int(cy + outer + 1); ++j) {
        for (int i = int(cx - outer - 1); i <= int(cx + outer + 1); ++i) {
            const float d = std::sqrt((i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy));
            Blend(i, j, WithAlpha(color, Clamp01(outer - d + 0.5f) * Clamp01(d - inner + 0.5f)));
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

} // namespace Menu
