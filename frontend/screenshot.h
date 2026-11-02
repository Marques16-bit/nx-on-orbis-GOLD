// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: 0.7.8 - saving what is on the screen as a picture, so a graphics fault (the Mario Maker 2
// themes, the HUD) can be looked at as it really is instead of through a phone camera.
//
// Header only on purpose: the checks in .github/workflows build the host tests with a fixed list of
// source files, and a workflow file cannot be changed by an update zip.
//
// The picture is a plain 24-bit BMP (no compression library needed, every viewer opens it). The
// buffers hold sRGB-coded values, which is also what the screen shows, so nothing is converted.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace Ps4Shot {

/// Which bytes of a 32-bit pixel hold red and blue. `red_low`: the word reads 0xAABBGGRR (A8B8G8R8),
/// otherwise 0xAARRGGBB (A8R8G8B8).
inline void Channels(std::uint32_t word, bool red_low, std::uint8_t* r, std::uint8_t* g, std::uint8_t* b) {
    const std::uint8_t lo = static_cast<std::uint8_t>(word & 0xFF), hi = static_cast<std::uint8_t>((word >> 16) & 0xFF);
    *g = static_cast<std::uint8_t>((word >> 8) & 0xFF);
    *r = red_low ? lo : hi;
    *b = red_low ? hi : lo;
}

/// Writes `pixels` (width x height, `width` words per row, top row first) as a 24-bit BMP. False with
/// `why` filled in when the file cannot be written.
inline bool WriteBmp(const std::string& path, const std::uint32_t* pixels, int width, int height, bool red_low,
                     std::string* why) {
    auto fail = [&](const char* text) {
        if (why != nullptr) {
            *why = text;
        }
        return false;
    };
    if (pixels == nullptr || width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return fail("no picture to save");
    }
    const std::uint32_t row_bytes = (static_cast<std::uint32_t>(width) * 3u + 3u) & ~3u;
    const std::uint32_t data_bytes = row_bytes * static_cast<std::uint32_t>(height);
    const std::uint32_t file_bytes = 54u + data_bytes;
    std::uint8_t head[54] = {};
    auto put32 = [&](int at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            head[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    };
    head[0] = 'B';
    head[1] = 'M';
    put32(2, file_bytes);
    put32(10, 54);
    put32(14, 40);                                   // BITMAPINFOHEADER
    put32(18, static_cast<std::uint32_t>(width));
    put32(22, static_cast<std::uint32_t>(height));   // positive: the bottom row comes first
    head[26] = 1;                                    // planes
    head[28] = 24;                                   // bits per pixel
    put32(34, data_bytes);
    put32(38, 2835);                                 // 72 dpi
    put32(42, 2835);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return fail("the file could not be created");
    }
    bool ok = std::fwrite(head, 1, sizeof(head), f) == sizeof(head);
    std::vector<std::uint8_t> row(row_bytes, 0);
    for (int y = height - 1; y >= 0 && ok; --y) {
        const std::uint32_t* src = pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; ++x) {
            std::uint8_t r, g, b;
            Channels(src[x], red_low, &r, &g, &b);
            row[static_cast<std::size_t>(x) * 3 + 0] = b;
            row[static_cast<std::size_t>(x) * 3 + 1] = g;
            row[static_cast<std::size_t>(x) * 3 + 2] = r;
        }
        ok = std::fwrite(row.data(), 1, row.size(), f) == row.size();
    }
    ok = (std::fflush(f) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    return ok ? true : fail("writing the file failed (disk full?)");
}

/// The first unused `dir/shot_0001.bmp`, `shot_0002.bmp`, ... (the folder is created if needed).
inline std::string NextPath(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (int n = 1; n < 10000; ++n) {
        char name[32];
        std::snprintf(name, sizeof(name), "shot_%04d.bmp", n);
        if (!std::filesystem::exists(dir / name, ec)) {
            return (dir / name).string();
        }
    }
    return (dir / "shot_9999.bmp").string();
}

} // namespace Ps4Shot
