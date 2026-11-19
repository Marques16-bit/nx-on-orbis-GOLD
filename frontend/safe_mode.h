// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.8.2: safe mode. Pure logic (the file path comes in as a parameter), so it is tested on a PC
// (tests/safe_mode_test.cpp).
//
// A file, safe_start.txt, is written when a game starts and removed as soon as the game has shown its
// first pictures. A start that finds the file there knows the last one died before the game drew anything
// (a crash while loading is the case this is for), and ignores the EXPERIMENTAL lines of settings.txt for
// that one start, so one wrong option cannot keep the app from opening again and again. settings.txt itself
// is never changed: the next start that gets past the first pictures clears the file and the lines apply again.
//
// What it cannot tell apart: a start that was ended by hand (power button, PS button) before the first
// picture looks the same as a crash. The cost of that mistake is one start without the experimental lines.
//
// 0.9.4 fix 2: the file now names the game and counts the starts in a row that died. The 0.9.4 test showed why: a crash of one game (SpongeBob,
// 11:41) turned safe mode on for the NEXT start, nine hours later and another game (Mario vs. Donkey Kong), which then ran without the JIT options,
// the 256 MiB JIT cache and the CPU clock, so a test of those options measured nothing. Safe mode now applies when the start that died was
// of the SAME game (the retry of a crash), or when two starts in a row died whatever the games (so a wrong option that kills every game is still caught).

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace Ps4Safe {

/// How many flips (pictures shown) count as "the game opened".
constexpr unsigned long long OpenedAfterFlips = 120;

/// What safe_start.txt says: the game of the last start that did not get as far as drawing, and how many starts in a row did not.
struct Record {
    bool present = false;
    std::string game;
    unsigned deaths = 0;
};

inline Record ReadRecord(const std::string& path) {
    Record r;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return r;
    }
    char line[1024] = {};
    if (std::fgets(line, sizeof line, f) != nullptr) {
        r.game = line;
        while (!r.game.empty() && (r.game.back() == '\n' || r.game.back() == '\r')) {
            r.game.pop_back();
        }
    }
    char count[32] = {};
    if (std::fgets(count, sizeof count, f) != nullptr) {
        r.deaths = static_cast<unsigned>(std::strtoul(count, nullptr, 10));
    }
    std::fclose(f);
    r.present = true;
    if (r.deaths == 0) {
        r.deaths = 1; // a file from before 0.9.4 fix 2 has no count: one start died
    }
    return r;
}

/// True when the previous start left its file behind (it did not get as far as drawing) AND this start is its retry (the same game) or the
/// second start in a row that follows a death (whatever the game).
inline bool PreviousStartDied(const std::string& path, const std::string& game) {
    const Record r = ReadRecord(path);
    return r.present && (r.game == game || r.deaths >= 2);
}

/// True when the previous start died but safe mode does not apply to this one: another game, and the first death.
inline bool PreviousStartDiedOtherGame(const std::string& path, const std::string& game) {
    const Record r = ReadRecord(path);
    return r.present && r.game != game && r.deaths < 2;
}

/// Written when a game starts. `note` is the game; the second line counts the starts in a row that have not drawn yet (this one included).
inline bool MarkStart(const std::string& path, const std::string& note) {
    const Record before = ReadRecord(path);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "%s\n%u\n", note.c_str(), before.present ? before.deaths + 1 : 1u);
    std::fclose(f);
    return true;
}

/// Removed once the game has opened (or left cleanly).
inline void MarkOpened(const std::string& path) {
    std::remove(path.c_str());
}

/// The lines safe mode ignores: the ones that can crash a start or hang it, not the ones that only change
/// quality or speed. `key` is settings.txt's own spelling ("env:NAME" for an env= line, as the menu writes it).
inline bool IsExperimental(std::string_view key, std::string_view value) {
    if (key == "cpu_clock") {
        return value != "normal";
    }
    if (key == "cpu_ticks") {
        return value != "off";
    }
    // sysflex_mib (0.9.0: experimental) is not one any more: it worked on the console in the 0.9.0 test. Safe mode still
    // limits it to the amount that test proved (main.cpp), instead of ignoring the line.
    if (key == "jit_cache_commit") {
        return value != "full"; // 0.9.7: the code caches that take their memory as they grow are what the start after a death does without
    }
    if (key == "jit_cache_mib") {
        return value != "auto" && value != "32"; // a bigger cache takes direct memory up front: not on a start after a death
    }
    if (key == "async_presentation" || key == "skip_cpu_invalidation" || key == "auto_stub" || key == "jit_page_reuse" ||
        key == "jit_direct_table") {
        return value == "on";
    }
    if (key == "astc") {
        return value != "cpu";
    }
    if (key == "astc_recompress") {
        return value != "off";
    }
    if (key == "dma_accuracy") {
        return value == "unsafe";
    }
    if (key == "memory_profile") {
        return value == "strict";
    }
    if (key == "env") {
        // The raw env=NAME=VALUE line as ApplySettingsFile sees it.
        return value == "ORBIS_VRAM_GARLIC=0";
    }
    if (key == "env:ORBIS_VRAM_GARLIC") {
        return value == "0";
    }
    return false;
}

} // namespace Ps4Safe
