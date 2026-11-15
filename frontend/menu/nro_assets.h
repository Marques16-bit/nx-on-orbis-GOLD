// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.5: the name and the picture a homebrew (.nro) carries. After the program, an NRO may hold an "asset section": a 256x256 JPEG
// icon, the NACP (the title in 16 languages, and the author) and a RomFS. The menu reads the icon and the title from it, so a homebrew is
// listed by its own name with its own picture instead of its file name and a coloured tile.
//
// Layout (switchbrew.org/wiki/NRO): the file size of the program is the u32 at 0x18; the asset header "ASET" sits at that offset:
//   0x00 "ASET"   0x04 version   0x08 icon offset (u64)   0x10 icon size (u64)   0x18 NACP offset   0x20 NACP size   0x28 RomFS offset   0x30 RomFS size
// every offset counted from the start of the asset header. NACP: 16 titles of 0x300 bytes (a 0x200 name, a 0x100 author), UTF-8, in the order
// of the Switch's languages (0 is American English).
//
// Header only and with no console code, so tests/menu_test.cpp builds it on a PC. Every size read from the file is checked before it is used.

#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Menu::Nro {

struct Info {
    std::string name;   // the title, "" when the NACP has none
    std::string author; // "" when the NACP has none
    std::vector<std::uint8_t> icon; // the JPEG, empty when there is none
};

inline constexpr std::uint64_t MaxIconBytes = 4ull << 20;
inline constexpr std::size_t NacpTitleBytes = 0x300;
inline constexpr std::size_t NacpTitles = 16;

namespace detail {

inline std::uint32_t U32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
inline std::uint64_t U64(const std::uint8_t* p) {
    return std::uint64_t(U32(p)) | (std::uint64_t(U32(p + 4)) << 32);
}

/// The text up to its first NUL (at most `max` bytes) if it is well formed UTF-8 without control characters; "" otherwise.
inline std::string CleanText(const std::uint8_t* bytes, std::size_t max) {
    std::size_t n = 0;
    while (n < max && bytes[n] != 0) {
        ++n;
    }
    std::string out;
    for (std::size_t i = 0; i < n;) {
        const std::uint8_t c = bytes[i];
        std::size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
        if (len == 0 || i + len > n) {
            return {};
        }
        for (std::size_t k = 1; k < len; ++k) {
            if ((bytes[i + k] & 0xC0) != 0x80) {
                return {};
            }
        }
        if (len == 1 && (c < 0x20 || c == 0x7F)) {
            out += ' '; // a tab or a line break inside a title is a space
        } else {
            out.append(reinterpret_cast<const char*>(bytes + i), len);
        }
        i += len;
    }
    // trim
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    std::size_t start = 0;
    while (start < out.size() && out[start] == ' ') {
        ++start;
    }
    return out.substr(start);
}

} // namespace detail

/// The first title of the NACP that has a name (American English first), and its author. `size` is the bytes available.
inline bool ParseNacp(const std::uint8_t* data, std::size_t size, std::string* name, std::string* author) {
    if (size < NacpTitleBytes) {
        return false;
    }
    const std::size_t titles = std::min<std::size_t>(NacpTitles, size / NacpTitleBytes);
    for (std::size_t t = 0; t < titles; ++t) {
        const std::uint8_t* entry = data + t * NacpTitleBytes;
        std::string n = detail::CleanText(entry, 0x200);
        if (!n.empty()) {
            *name = std::move(n);
            *author = detail::CleanText(entry + 0x200, 0x100);
            return true;
        }
    }
    return false;
}

/// Reads the asset section of a .nro. False (with `why`) when the file is not an NRO or has no asset section; `info` then holds what was found
/// (the icon and the title are read apart: one may be there without the other).
inline bool Read(const std::filesystem::path& path, Info* info, std::string* why) {
    const auto fail = [&](const char* text) {
        if (why != nullptr) {
            *why = text;
        }
        return false;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail("cannot open the file");
    }
    in.seekg(0, std::ios::end);
    const std::uint64_t file_size = std::uint64_t(std::max<std::streamoff>(in.tellg(), 0));
    const auto read_at = [&](std::uint64_t offset, std::uint64_t bytes, std::vector<std::uint8_t>* out) {
        if (offset > file_size || bytes > file_size - offset) {
            return false;
        }
        out->assign(size_t(bytes), 0);
        in.clear();
        in.seekg(std::streamoff(offset));
        in.read(reinterpret_cast<char*>(out->data()), std::streamsize(bytes));
        return bool(in) || std::uint64_t(in.gcount()) == bytes;
    };
    std::vector<std::uint8_t> head;
    if (!read_at(0, 0x20, &head) || head[0x10] != 'N' || head[0x11] != 'R' || head[0x12] != 'O' || head[0x13] != '0') {
        return fail("not an NRO (no NRO0 header)");
    }
    const std::uint64_t aset = detail::U32(head.data() + 0x18);
    std::vector<std::uint8_t> asset;
    if (!read_at(aset, 0x38, &asset) || asset[0] != 'A' || asset[1] != 'S' || asset[2] != 'E' || asset[3] != 'T') {
        return fail("no asset section (no icon or title inside this NRO)");
    }
    const std::uint64_t icon_offset = detail::U64(asset.data() + 0x08), icon_size = detail::U64(asset.data() + 0x10);
    const std::uint64_t nacp_offset = detail::U64(asset.data() + 0x18), nacp_size = detail::U64(asset.data() + 0x20);
    bool any = false;
    if (nacp_size >= NacpTitleBytes && nacp_size <= (1u << 20) && nacp_offset <= file_size) {
        std::vector<std::uint8_t> nacp;
        // only the titles are needed: the first 0x3000 bytes
        if (read_at(aset + nacp_offset, std::min<std::uint64_t>(nacp_size, NacpTitleBytes * NacpTitles), &nacp)) {
            any = ParseNacp(nacp.data(), nacp.size(), &info->name, &info->author) || any;
        }
    }
    if (icon_size >= 16 && icon_size <= MaxIconBytes && icon_offset <= file_size) {
        std::vector<std::uint8_t> icon;
        if (read_at(aset + icon_offset, icon_size, &icon) && icon[0] == 0xFF && icon[1] == 0xD8) {
            info->icon = std::move(icon);
            any = true;
        }
    }
    return any ? true : fail("the asset section has neither a title nor a JPEG icon");
}

} // namespace Menu::Nro
