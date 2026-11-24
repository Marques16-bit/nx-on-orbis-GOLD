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
//
// 0.9.7 fix 2: "died" is no longer "the file was left behind". The 0.9.7 logs of the .nro and SpongeBob tests were both in safe mode, because the start before
// each one was a black screen that the user ended by hand after a while: the experimental options of settings.txt (the very ones being tested) were ignored and the
// JIT cache was cut to 32 MiB, so the test measured nothing. Now the file also holds how long the start stayed alive (written every few seconds by main.cpp), and a
// crash or fatal error writes into a marker file next to it (ps4_platform.cpp, one write() first thing). A start counts as a death when its marker
// is there, or when it ended within StayedAliveSeconds of starting without having drawn (a silent kill, or an exit by hand right away). A start that stayed alive
// StayedAliveSeconds (20 since 0.9.7 fix 3; it was 60) or more without a crash marker (a hang that was ended by hand) does not. The log of the .nro black screen of Nov 22
// was in safe mode again ("the last start (SpongeBob) ended before the game drew anything (3 start(s) in a row)"): a black screen ended by hand sooner than the limit
// still counts as a death, and 60 s is a long time to look at a black screen. 20 s is enough for the menu to have run its heartbeat several times and short enough that a
// quick close is still told from a hang. A real crash is not decided by this number at all: it leaves the marker. And the TESTS tab has `safe_mode=off` for whoever is testing options on purpose.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace Ps4Safe {

/// How many flips (pictures shown) count as "the game opened".
constexpr unsigned long long OpenedAfterFlips = 120;

/// A start that was still running this many seconds after it began, and did not leave a crash marker, was ended by hand (a hang): not a death.
constexpr unsigned StayedAliveSeconds = 20;

/// What safe_start.txt says: the game of the last start that did not get as far as drawing, how many starts in a row did not, how long the last one stayed alive
/// (the third line, "alive=N"; a file from before 0.9.7 fix 2 has none: 0), and whether a crash marker lies next to it.
struct Record {
    bool present = false;
    std::string game;
    unsigned deaths = 0;
    unsigned alive_seconds = 0;
    bool crashed = false;
};

/// The file a crash writes into (ps4_platform.cpp: SetCrashMarker creates it empty, WriteCrashMarker fills it): the start file's own path with ".crash" added.
inline std::string CrashMarkerPath(const std::string& path) {
    return path + ".crash";
}

/// The marker is created empty when a game starts and written to by a crash: a file that has something in it.
inline bool FileHasContent(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    const bool content = std::fgetc(f) != EOF;
    std::fclose(f);
    return content;
}

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
    char alive[48] = {};
    if (std::fgets(alive, sizeof alive, f) != nullptr && std::strncmp(alive, "alive=", 6) == 0) {
        r.alive_seconds = static_cast<unsigned>(std::strtoul(alive + 6, nullptr, 10));
    }
    std::fclose(f);
    r.present = true;
    r.crashed = FileHasContent(CrashMarkerPath(path));
    if (r.deaths == 0) {
        r.deaths = 1; // a file from before 0.9.4 fix 2 has no count: one start died
    }
    return r;
}

/// A start that did not draw and did not leave cleanly counts as a death when it crashed (the marker), or when it was over within StayedAliveSeconds.
inline bool Died(const Record& r) {
    return r.present && (r.crashed || r.alive_seconds < StayedAliveSeconds);
}

/// True when the previous start died (Died) AND this start is its retry (the same game) or the second start in a row that follows a death (whatever the game).
inline bool PreviousStartDied(const std::string& path, const std::string& game) {
    const Record r = ReadRecord(path);
    return Died(r) && (r.game == game || r.deaths >= 2);
}

/// True when the previous start died but safe mode does not apply to this one: another game, and the first death.
inline bool PreviousStartDiedOtherGame(const std::string& path, const std::string& game) {
    const Record r = ReadRecord(path);
    return Died(r) && r.game != game && r.deaths < 2;
}

/// True when the previous start did not draw and did not leave cleanly, but it is not counted as a death: it stayed alive StayedAliveSeconds or more with no crash
/// marker (a hang that was ended by hand). main.cpp says so in boot.log.
inline bool PreviousStartWasHang(const std::string& path) {
    const Record r = ReadRecord(path);
    return r.present && !Died(r);
}

/// Written when a game starts. `note` is the game; the second line counts the starts in a row that have not drawn yet and died (this one included), the third says how
/// long this start has been alive (0 now; MarkAlive updates it). A previous start that is not counted as a death does not add to the count.
inline bool MarkStart(const std::string& path, const std::string& note) {
    const Record before = ReadRecord(path);
    std::remove(CrashMarkerPath(path).c_str()); // a marker of an earlier start must not be taken for this one's
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "%s\n%u\nalive=0\n", note.c_str(), Died(before) ? before.deaths + 1 : 1u);
    std::fclose(f);
    return true;
}

/// Called every few seconds by a start that has not drawn yet: rewrites the third line. False when there is no file (nothing to update).
inline bool MarkAlive(const std::string& path, unsigned seconds) {
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) {
        return false;
    }
    char game[1024] = {};
    char count[32] = {};
    const bool have_game = std::fgets(game, sizeof game, in) != nullptr;
    const bool have_count = std::fgets(count, sizeof count, in) != nullptr;
    std::fclose(in);
    if (!have_game) {
        return false;
    }
    std::string head = game;
    while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) {
        head.pop_back();
    }
    const unsigned deaths = have_count ? static_cast<unsigned>(std::strtoul(count, nullptr, 10)) : 1u;
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (out == nullptr) {
        return false;
    }
    std::fprintf(out, "%s\n%u\nalive=%u\n", head.c_str(), deaths == 0 ? 1u : deaths, seconds);
    std::fclose(out);
    return true;
}

/// Removed once the game has opened (or left cleanly), with its crash marker.
inline void MarkOpened(const std::string& path) {
    std::remove(path.c_str());
    std::remove(CrashMarkerPath(path).c_str());
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
        key == "jit_direct_table" || key == "arena_align") {
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
