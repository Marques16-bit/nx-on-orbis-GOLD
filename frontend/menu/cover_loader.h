// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: 0.7.9 - the covers of the Games tab, and the helpers that read a dump's file name.
//
// Header only on purpose (the host checks in .github/workflows use a fixed list of source files, and a
// workflow cannot be changed by an update zip). The files are read from /data/edenps4/covers/:
//   <title id>.png          01009B90006DC000.png     (the 16 hex digits in the dump's [brackets])
//   <file name>.png         Super Mario Maker 2 [01009B90006DC000][v0].png   (the dump's name, no extension)
//   <title>.png             Super Mario Maker 2.png  (the dump's name without [tags] and (tags))
// .png, .jpg, .jpeg and .bmp are read (see image_decode.h for what each decoder supports). Matching
// ignores upper/lower case. Every limit below exists so a wrong folder (a 40 GB one, a 200 MB "picture")
// cannot stall or exhaust the console before the menu opens.

#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "menu/menu_ui.h"

namespace Menu::Covers {

/// The title in a dump's file name: no extension, no [tags] or (tags) ("Super Mario Maker 2
/// [01009B90006DC000][v0] (US).nsp" -> "Super Mario Maker 2"), '_' as spaces, spaces tidied.
inline std::string DisplayName(const std::string& file) {
    std::string name = file;
    if (const auto dot = name.find_last_of('.'); dot != std::string::npos && dot > 0) {
        name.erase(dot);
    }
    std::string title;
    int depth = 0;
    for (const char ch : name) {
        if (ch == '[' || ch == '(') {
            ++depth;
        } else if ((ch == ']' || ch == ')') && depth > 0) {
            --depth;
        } else if (depth == 0) {
            title += ch == '_' ? ' ' : ch;
        }
    }
    std::string tidy;
    for (const char ch : title) {
        if (!(ch == ' ' && (tidy.empty() || tidy.back() == ' '))) {
            tidy += ch;
        }
    }
    while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == '-')) {
        tidy.pop_back();
    }
    return tidy.empty() ? name : tidy;
}

/// The title id in a dump's file name: the 16 hex digits in [brackets] ("...[01009B90006DC000][v0]..."), or "".
inline std::string TitleIdOf(const std::string& file) {
    for (size_t open = file.find('['); open != std::string::npos; open = file.find('[', open + 1)) {
        const size_t close = file.find(']', open);
        if (close != std::string::npos && close - open == 17) {
            bool hex = true;
            for (size_t i = open + 1; i < close; ++i) {
                hex = hex && std::isxdigit(static_cast<unsigned char>(file[i])) != 0;
            }
            if (hex) {
                std::string id = file.substr(open + 1, 16);
                for (char& ch : id) {
                    ch = char(std::toupper(static_cast<unsigned char>(ch)));
                }
                return id;
            }
        }
    }
    return {};
}

inline std::string Lower(std::string text) {
    for (char& ch : text) {
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

struct Options {
    std::uint64_t max_file_bytes = 12ull << 20;   // a bigger picture file is skipped
    std::uint64_t max_cover_bytes = 64ull << 20;  // pixels kept for the big covers; past this, thumbnails only
    int budget_ms = 8000;                         // decoding stops after this long (the rest get tiles)
    int max_dir_files = 4096;                     // directory entries looked at
};

struct Report {
    bool dir_found = false;
    int dir_pictures = 0;   // picture files seen in the folder
    int found = 0;          // games that got a cover
    int failed = 0;         // matched a file that could not be read or decoded
    int thumbs_only = 0;    // got a thumbnail but no big cover (memory cap)
    int skipped_time = 0;   // not tried: out of time
    std::uint64_t cover_bytes = 0;
    long long ms = 0;
    std::string first_error; // "file: reason" of the first failure
};

inline Report Load(const std::filesystem::path& dir, std::vector<GameEntry>& games, const Options& opt = {}) {
    using Clock = std::chrono::steady_clock;
    const auto began = Clock::now();
    Report report;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return report;
    }
    report.dir_found = true;
    // key (lower case stem) -> path; png wins over jpg over jpeg over bmp when a name exists in several
    std::map<std::string, std::pair<int, std::filesystem::path>> files;
    int seen = 0;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && seen < opt.max_dir_files; it.increment(ec), ++seen) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec)) {
            continue;
        }
        const std::string ext = Lower(it->path().extension().string());
        const int rank = ext == ".png" ? 0 : ext == ".jpg" ? 1 : ext == ".jpeg" ? 2 : ext == ".bmp" ? 3 : -1;
        if (rank < 0) {
            continue;
        }
        ++report.dir_pictures;
        const std::string key = Lower(it->path().stem().string());
        const auto found = files.find(key);
        if (found == files.end() || rank < found->second.first) {
            files[key] = {rank, it->path()};
        }
    }
    for (GameEntry& game : games) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count() > opt.budget_ms) {
            ++report.skipped_time;
            continue;
        }
        const std::string id = TitleIdOf(game.name);
        const std::string stem = std::filesystem::path(game.name).stem().string();
        const std::string keys[3] = {Lower(id), Lower(stem), Lower(DisplayName(game.name))};
        const std::filesystem::path* path = nullptr;
        for (const std::string& key : keys) {
            const auto found = key.empty() ? files.end() : files.find(key);
            if (found != files.end()) {
                path = &found->second.second;
                break;
            }
        }
        if (path == nullptr) {
            continue;
        }
        auto fail = [&](const std::string& why) {
            ++report.failed;
            if (report.first_error.empty()) {
                report.first_error = path->filename().string() + ": " + why;
            }
        };
        const std::uint64_t size = std::filesystem::file_size(*path, ec);
        if (ec || size == 0 || size > opt.max_file_bytes) {
            fail(ec ? "cannot be read" : size == 0 ? "empty file" : "file too big");
            continue;
        }
        std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
        {
            std::ifstream in(*path, std::ios::binary);
            if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) {
                fail("cannot be read");
                continue;
            }
        }
        Image::Pixels full;
        std::string why;
        if (!Image::Decode(bytes.data(), bytes.size(), &full, &why)) {
            fail(why.empty() ? "unknown image format" : why);
            continue;
        }
        Image::Pixels big = Image::Fit(full, CoverSize, CoverSize, true);
        game.thumb = Image::Fit(big.Ok() ? big : full, ThumbSize, ThumbSize);
        const std::uint64_t cost = big.Ok() ? std::uint64_t(big.px.size()) * 4 : 0;
        if (big.Ok() && report.cover_bytes + cost <= opt.max_cover_bytes) {
            game.cover = std::move(big);
            report.cover_bytes += cost;
        } else {
            ++report.thumbs_only;
        }
        ++report.found;
    }
    report.ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
    return report;
}

} // namespace Menu::Covers
