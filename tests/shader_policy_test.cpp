// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for frontend/shader_policy.cpp: the size limit of the on-disk shader cache and the text
// of the on-screen shader counter.
#include <cstdio>
#include <fstream>
#include <string>

#include "shader_policy.h"

namespace fs = std::filesystem;
static int g_failures = 0;
static void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

static void Make(const fs::path& file, std::uint64_t bytes, char fill = 'x') {
    std::ofstream out{file, std::ios::binary};
    std::string block(4096, fill);
    for (std::uint64_t done = 0; done < bytes;) {
        const auto n = std::min<std::uint64_t>(block.size(), bytes - done);
        out.write(block.data(), std::streamsize(n));
        done += n;
    }
}

int main() {
    using namespace Ps4Shader;
    const fs::path dir = fs::temp_directory_path() / "shader_policy_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    Check(LimitCache(dir, 256).action == CacheAction::NoFile, "no vulkan.bin: nothing to do");

    Make(dir / "vulkan.bin", 1024 * 1024);
    auto c = LimitCache(dir, 256); // 256 x 32 KiB = 8 MiB
    Check(c.action == CacheAction::Kept && c.size == 1024 * 1024 && c.limit == 8 * 1024 * 1024, "1 MiB under an 8 MiB limit: kept");
    Check(fs::exists(dir / "vulkan.bin") && !fs::exists(dir / "vulkan.bin.bak"), "...and left where it is");

    c = LimitCache(dir, -1);
    Check(c.action == CacheAction::Kept && c.unlimited, "all: no limit, kept");

    Make(dir / "vulkan.bin", 9 * 1024 * 1024, 'n');
    Make(dir / "vulkan.bin.bak", 100, 'o'); // an older backup is replaced
    c = LimitCache(dir, 256);
    Check(c.action == CacheAction::SetAside, "9 MiB over an 8 MiB limit: set aside");
    Check(!fs::exists(dir / "vulkan.bin") && fs::exists(dir / "vulkan.bin.bak") &&
              fs::file_size(dir / "vulkan.bin.bak") == 9 * 1024 * 1024, "...vulkan.bin.bak is the big one now, vulkan.bin is gone");

    Make(dir / "vulkan.bin", 10);
    c = LimitCache(dir, 0);
    Check(c.action == CacheAction::SetAside && c.limit == 0, "none (0): even a tiny cache is set aside");

    // The rest of the folder (the driver's own cache) is never touched.
    Make(dir / "vulkan_pipelines.bin", 5000);
    Make(dir / "vulkan.bin", 50 * 1024 * 1024);
    LimitCache(dir, 128);
    Check(fs::exists(dir / "vulkan_pipelines.bin"), "the driver's cache file is left alone");

    int last = 0, total = 0;
    CountBuilding(3, &last, &total);
    CountBuilding(7, &last, &total);
    Check(total == 7 && last == 7, "counter: 3 then 7 builds is 7 in total");
    CountBuilding(0, &last, &total);
    CountBuilding(2, &last, &total);
    Check(total == 9, "...a quiet spell resets Eden's count, the total keeps going (7 + 2)");
    CountBuilding(-5, &last, &total);
    Check(total == 9 && last == 0, "a negative number is read as 0");

    Check(NoticeText(0, 9, 2000, true).empty(), "nothing compiling and memory fine: no text");
    Check(NoticeText(4, 9, 2000, true) == "Compilando shaders: 4  (total 9)", "Portuguese counter");
    Check(NoticeText(4, 9, 2000, false) == "Compiling shaders: 4  (total 9)", "English counter");
    Check(NoticeText(0, 9, 120, false) == "LOW MEMORY: 120 MiB free", "low memory alone");
    Check(NoticeText(2, 5, 100, true).find("Compilando") == 0 && NoticeText(2, 5, 100, true).find("MEMORIA BAIXA") != std::string::npos,
          "both together: the counter first, then the warning");

    fs::remove_all(dir);
    std::printf(g_failures == 0 ? "shader policy test: PASSED\n" : "shader policy test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
