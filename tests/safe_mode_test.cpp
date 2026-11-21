// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of frontend/safe_mode.h.
//   g++ -std=c++20 -Wall -Wextra -Ifrontend tests/safe_mode_test.cpp -o safe_mode_test
//   ./safe_mode_test
#include <cstdio>
#include <string>

#include "safe_mode.h"

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

} // namespace

int main() {
    const std::string path = "safe_mode_test.tmp";
    std::remove(path.c_str());

    Check(!Ps4Safe::PreviousStartDied(path, "A"), "no file: the last start was fine");
    Check(Ps4Safe::MarkStart(path, "A"), "a start writes its file");
    Check(Ps4Safe::PreviousStartDied(path, "A"), "the file is still there and it is the same game: the retry of a death gets safe mode");
    Ps4Safe::MarkOpened(path);
    Check(!Ps4Safe::PreviousStartDied(path, "A"), "once the game drew, the file is gone");
    Ps4Safe::MarkOpened(path); // removing twice is harmless
    Check(!Ps4Safe::PreviousStartDied(path, "A"), "removing a missing file is harmless");
    Check(!Ps4Safe::MarkStart("no_such_dir/safe_start.txt", "game"), "a folder that is not there: MarkStart says so, nothing throws");

    // 0.9.4 fix 2: safe mode follows the game, and two deaths in a row.
    Ps4Safe::MarkStart(path, "SpongeBob"); // the crash of 11:41
    Check(!Ps4Safe::PreviousStartDied(path, "Mario vs. Donkey Kong") && Ps4Safe::PreviousStartDiedOtherGame(path, "Mario vs. Donkey Kong"),
          "another game after one death: NO safe mode (the 0.9.4 test lost its options this way), and the log can say why");
    Check(Ps4Safe::PreviousStartDied(path, "SpongeBob") && !Ps4Safe::PreviousStartDiedOtherGame(path, "SpongeBob"), "the same game again: safe mode");
    Ps4Safe::MarkStart(path, "Mario vs. Donkey Kong"); // that start died too: two in a row, two games
    Check(Ps4Safe::ReadRecord(path).deaths == 2 && Ps4Safe::ReadRecord(path).game == "Mario vs. Donkey Kong", "the file counts the starts in a row and names the last game");
    Check(Ps4Safe::PreviousStartDied(path, "Zelda") && !Ps4Safe::PreviousStartDiedOtherGame(path, "Zelda"),
          "two deaths in a row, whatever the games: safe mode (an option that kills every game is still caught)");
    Ps4Safe::MarkOpened(path);
    Check(!Ps4Safe::PreviousStartDied(path, "Zelda") && !Ps4Safe::PreviousStartDiedOtherGame(path, "Zelda") && !Ps4Safe::ReadRecord(path).present, "a start that drew resets all of it");
    // a file written before 0.9.4 fix 2: the game's name alone, no count (one death)
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fputs("old game\n", f);
        std::fclose(f);
    }
    Check(Ps4Safe::PreviousStartDied(path, "old game") && !Ps4Safe::PreviousStartDied(path, "other") && Ps4Safe::ReadRecord(path).deaths == 1,
          "an old file without a count means one death of that game");
    // a file saved with CRLF line ends (edited by hand on a PC): the name is cut clean
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fputs("A\r\n2\r\n", f);
        std::fclose(f);
    }
    Check(Ps4Safe::ReadRecord(path).game == "A" && Ps4Safe::ReadRecord(path).deaths == 2, "CRLF line ends are cut from the name");
    // a game name with spaces, dots and brackets survives the round trip
    Ps4Safe::MarkOpened(path);
    Ps4Safe::MarkStart(path, "/data/edenps4/roms/Mario vs. Donkey Kong [0100B99019412000][v0].nsp");
    Check(Ps4Safe::PreviousStartDied(path, "/data/edenps4/roms/Mario vs. Donkey Kong [0100B99019412000][v0].nsp"), "a real game path is compared whole");
    Ps4Safe::MarkOpened(path);

    using Ps4Safe::IsExperimental;
    Check(IsExperimental("cpu_clock", "boost") && IsExperimental("cpu_clock", "overclock") && !IsExperimental("cpu_clock", "normal"),
          "cpu_clock: only the raised steps are experimental");
    Check(IsExperimental("cpu_ticks", "4000") && !IsExperimental("cpu_ticks", "off"), "cpu_ticks: a number is experimental, off is not");
    Check(IsExperimental("async_presentation", "on") && !IsExperimental("async_presentation", "off"), "async_presentation");
    Check(IsExperimental("skip_cpu_invalidation", "on") && !IsExperimental("skip_cpu_invalidation", "off"), "skip_cpu_invalidation");
    Check(IsExperimental("auto_stub", "on") && !IsExperimental("auto_stub", "off"), "auto_stub");
    Check(IsExperimental("jit_page_reuse", "on") && !IsExperimental("jit_page_reuse", "off"), "jit_page_reuse (0.9.2): on is experimental");
    Check(IsExperimental("jit_direct_table", "on") && !IsExperimental("jit_direct_table", "off"), "jit_direct_table (0.9.3): on is experimental");
    Check(IsExperimental("arena_align", "on") && !IsExperimental("arena_align", "off"),
          "arena_align (0.9.7 fix): a line saying on is ignored in safe mode (main() switches the default off too)");
    Check(IsExperimental("jit_cache_mib", "64") && IsExperimental("jit_cache_mib", "256") && !IsExperimental("jit_cache_mib", "auto") && !IsExperimental("jit_cache_mib", "32"),
          "jit_cache_mib (0.9.4): a bigger cache is experimental (it takes direct memory up front), the default is not");
    Check(IsExperimental("jit_cache_commit", "lazy") && !IsExperimental("jit_cache_commit", "full"),
          "jit_cache_commit (0.9.7): lazy steps are experimental (a start after a death takes the memory whole, the way that was always taken)");
    Check(IsExperimental("astc", "gpu") && IsExperimental("astc", "async") && !IsExperimental("astc", "cpu"), "astc: only the CPU mode is safe");
    Check(!IsExperimental("sysflex_mib", "384") && !IsExperimental("sysflex_mib", "2048") && !IsExperimental("sysflex_mib", "auto") &&
              !IsExperimental("sysflex_mib", "off") && !IsExperimental("players", "4"),
          "sysflex_mib (not experimental since 0.9.1) and players are never ignored by safe mode");
    Check(IsExperimental("astc_recompress", "bc3") && IsExperimental("astc_recompress", "bc1") && !IsExperimental("astc_recompress", "off"),
          "astc_recompress: BC1 and BC3 are experimental, off is not");
    Check(IsExperimental("dma_accuracy", "unsafe") && !IsExperimental("dma_accuracy", "default") && !IsExperimental("dma_accuracy", "safe"),
          "dma_accuracy: only unsafe");
    Check(IsExperimental("memory_profile", "strict") && !IsExperimental("memory_profile", "tight"), "memory_profile: only strict");
    Check(IsExperimental("env", "ORBIS_VRAM_GARLIC=0") && !IsExperimental("env", "ORBIS_VRAM_GARLIC=1") &&
              !IsExperimental("env", "ORBIS_ARENA_MIB=1152"),
          "an env= line: only textures inside the arena");
    Check(IsExperimental("env:ORBIS_VRAM_GARLIC", "0") && !IsExperimental("env:ORBIS_VRAM_GARLIC", "1"), "the menu's spelling of that env line");
    Check(!IsExperimental("resolution", "0.5") && !IsExperimental("gpu_accuracy", "low") && !IsExperimental("hud", "on") &&
              !IsExperimental("covers", "off") && !IsExperimental("vsync", "immediate"),
          "options that only change quality, speed or what is shown are not experimental");
    Check(Ps4Safe::OpenedAfterFlips >= 60, "the game must have drawn at least a second of pictures");

    std::remove(path.c_str());
    std::printf(g_failures == 0 ? "safe mode test: PASSED\n" : "safe mode test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
