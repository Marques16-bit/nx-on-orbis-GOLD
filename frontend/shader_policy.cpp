// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: shader decisions without Eden. See shader_policy.h.

#include <cstdio>
#include <cstdlib>
#include <fstream>

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
    out.action = ParkCache(dir, nullptr) ? CacheAction::SetAside : CacheAction::Failed;
    return out;
}

bool ParkCache(const fs::path& dir, fs::path* parked_as) {
    std::error_code ec;
    const fs::path file = dir / "vulkan.bin";
    if (!fs::exists(file, ec)) {
        return false;
    }
    for (int n = 1; n < 1000; ++n) {
        const fs::path backup = dir / (n == 1 ? std::string("vulkan.bin.bak") : "vulkan.bin.bak" + std::to_string(n));
        if (fs::exists(backup, ec)) {
            continue;
        }
        ec.clear();
        fs::rename(file, backup, ec);
        if (!ec && parked_as != nullptr) {
            *parked_as = backup;
        }
        return !ec;
    }
    return false;
}

LoadMarker ReadMarker(const fs::path& file) {
    LoadMarker marker;
    std::ifstream in{file};
    if (!in) {
        return marker;
    }
    marker.present = true;
    marker.attempt = 1; // an older version's marker says only "loading"
    std::string word;
    int attempt = 0;
    if (in >> word >> attempt && word == "attempt" && attempt > 0) {
        marker.attempt = attempt;
        in >> marker.title;
    }
    return marker;
}

bool WriteMarker(const fs::path& file, int attempt, const std::string& title) {
    std::ofstream out{file};
    if (!out) {
        return false;
    }
    out << "attempt " << attempt << ' ' << title << '\n';
    return bool(out);
}

LoadMode DecideLoad(const LoadMarker& marker, const std::string& title) {
    if (!marker.present) {
        return LoadMode::Normal;
    }
    if (marker.title.empty() || marker.title != title) {
        return LoadMode::Safe;
    }
    return marker.attempt >= 2 ? LoadMode::Empty : LoadMode::Safe;
}

int NextAttempt(const LoadMarker& marker, const std::string& title) {
    return marker.present && !marker.title.empty() && marker.title == title ? marker.attempt + 1 : 1;
}

const char* LoadModeName(LoadMode mode) {
    return mode == LoadMode::Normal ? "normal" : mode == LoadMode::Safe ? "safe (one builder thread)" : "empty (cache parked)";
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
