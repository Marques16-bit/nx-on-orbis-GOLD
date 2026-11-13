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

#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace Ps4Safe {

/// How many flips (pictures shown) count as "the game opened".
constexpr unsigned long long OpenedAfterFlips = 120;

/// True when the previous start left its file behind: it did not get as far as drawing.
inline bool PreviousStartDied(const std::string& path) {
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

/// Written when a game starts. `note` (the title, or "game") is for whoever reads the file.
inline bool MarkStart(const std::string& path, const std::string& note) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "%s\n", note.c_str());
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
