// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: shader decisions without Eden. See shader_policy.h.

#include <cstdio>

#include "shader_policy.h"

namespace fs = std::filesystem;

namespace Ps4Shader {

CacheCheck LimitCache(const fs::path& dir, int cap) {
    CacheCheck out;
    std::error_code ec;
    const fs::path file = dir / "vulkan.bin";
    const auto size = fs::file_size(file, ec);
    if (ec) {
        out.action = CacheAction::NoFile;
        return out;
    }
    out.size = size;
    if (cap < 0) {
        out.unlimited = true;
        out.action = CacheAction::Kept;
        return out;
    }
    out.limit = static_cast<std::uint64_t>(cap) * BytesPerPipeline;
    if (size <= out.limit) {
        out.action = CacheAction::Kept;
        return out;
    }
    const fs::path backup = dir / "vulkan.bin.bak";
    fs::remove(backup, ec);
    ec.clear();
    fs::rename(file, backup, ec);
    out.action = ec ? CacheAction::Failed : CacheAction::SetAside;
    return out;
}

void CountBuilding(int building, int* last, int* total) {
    if (building < 0) {
        building = 0;
    }
    if (building >= *last) {
        *total += building - *last;
    } else {
        *total += building; // the count started again after a quiet spell
    }
    *last = building;
}

std::string NoticeText(int building, int total, std::uint64_t free_mib, bool portuguese) {
    std::string text;
    char line[96];
    if (building > 0) {
        std::snprintf(line, sizeof(line), portuguese ? "Compilando shaders: %d  (total %d)" : "Compiling shaders: %d  (total %d)",
                      building, total);
        text = line;
    }
    if (free_mib < LowMemoryMiB) {
        std::snprintf(line, sizeof(line), portuguese ? "MEMORIA BAIXA: %llu MiB livres" : "LOW MEMORY: %llu MiB free",
                      static_cast<unsigned long long>(free_mib));
        if (!text.empty()) {
            text += "   ";
        }
        text += line;
    }
    return text;
}

} // namespace Ps4Shader
