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

    Check(!Ps4Safe::PreviousStartDied(path), "no file: the last start was fine");
    Check(Ps4Safe::MarkStart(path, "game"), "a start writes its file");
    Check(Ps4Safe::PreviousStartDied(path), "the file is still there: the next start sees a death");
    Ps4Safe::MarkOpened(path);
    Check(!Ps4Safe::PreviousStartDied(path), "once the game drew, the file is gone");
    Ps4Safe::MarkOpened(path); // removing twice is harmless
    Check(!Ps4Safe::PreviousStartDied(path), "removing a missing file is harmless");
    Check(!Ps4Safe::MarkStart("no_such_dir/safe_start.txt", "game"), "a folder that is not there: MarkStart says so, nothing throws");

    using Ps4Safe::IsExperimental;
    Check(IsExperimental("cpu_clock", "boost") && IsExperimental("cpu_clock", "overclock") && !IsExperimental("cpu_clock", "normal"),
          "cpu_clock: only the raised steps are experimental");
    Check(IsExperimental("cpu_ticks", "4000") && !IsExperimental("cpu_ticks", "off"), "cpu_ticks: a number is experimental, off is not");
    Check(IsExperimental("async_presentation", "on") && !IsExperimental("async_presentation", "off"), "async_presentation");
    Check(IsExperimental("skip_cpu_invalidation", "on") && !IsExperimental("skip_cpu_invalidation", "off"), "skip_cpu_invalidation");
    Check(IsExperimental("auto_stub", "on") && !IsExperimental("auto_stub", "off"), "auto_stub");
    Check(IsExperimental("jit_page_reuse", "on") && !IsExperimental("jit_page_reuse", "off"), "jit_page_reuse (0.9.2): on is experimental");
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
