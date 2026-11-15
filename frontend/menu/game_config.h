// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.5: a game's own settings. `per-game/<key>.txt` in the data folder holds the same `key=value` lines as settings.txt and is
// applied on top of it, for that game only. The key is the title id when the file name has one ("[0100000000010000]"), else the file name
// without its extension, with anything but letters, digits and "-" turned into "_".
//
// Header only, no console code: the host tests (tests/menu_test.cpp) build it as it is. Written by the R2 screen of the menu; a file can also
// be written by hand.

#pragma once

#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

#include "menu/cover_loader.h"
#include "menu/settings_model.h"

namespace Menu::GameConfig {

namespace fs = std::filesystem;

/// The name of the game's file, without ".txt".
inline std::string KeyFor(const std::string& game_file) {
    const std::string id = Covers::TitleIdOf(game_file);
    if (!id.empty()) {
        return id;
    }
    const std::string stem = fs::path(game_file).stem().string();
    std::string out;
    for (const unsigned char ch : stem) {
        const char c = std::isalnum(ch) || ch == '-' ? char(ch) : '_';
        if (c == '_' && (out.empty() || out.back() == '_')) {
            continue; // no leading or doubled "_"
        }
        out += c;
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.size() > 80) {
        out.resize(80);
    }
    return out.empty() ? std::string("game") : out;
}

inline fs::path PathFor(const fs::path& dir, const std::string& game_file) {
    return dir / (KeyFor(game_file) + ".txt");
}

/// The text of the game's file; "" when there is none.
inline std::string Load(const fs::path& dir, const std::string& game_file) {
    std::ifstream in(PathFor(dir, game_file), std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// Writes the game's file. A text with no setting in it (only comments, or nothing) removes the file instead, so "no file" is the same as
/// "nothing set for this game". Returns true when the folder now holds what was asked for.
inline bool Save(const fs::path& dir, const std::string& game_file, const std::string& text) {
    const fs::path path = PathFor(dir, game_file);
    std::error_code ec;
    if (!SettingsFile(text).HasSettings()) {
        fs::remove(path, ec);
        return !fs::exists(path, ec);
    }
    fs::create_directories(dir, ec);
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        // Some file systems of the console do not replace a file by renaming: write it in place.
        fs::remove(temporary, ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            return false;
        }
    }
    // Open to everyone, like settings.txt, so the file can be edited over FTP (the process has no umask on the console).
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::group_write |
                              fs::perms::others_read | fs::perms::others_write, ec);
    return Load(dir, game_file) == text;
}

/// The keys of the files in the folder, to mark the games that have one.
inline std::set<std::string> Present(const fs::path& dir) {
    std::set<std::string> keys;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".txt" && it->is_regular_file(ec) && it->file_size(ec) > 0) {
            keys.insert(it->path().stem().string());
        }
    }
    return keys;
}

} // namespace Menu::GameConfig
