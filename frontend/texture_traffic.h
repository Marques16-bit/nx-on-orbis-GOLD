// SPDX-License-Identifier: GPL-3.0-or-later
// 0.9.7: how much texture data the CPU moves, from the counters Eden patch 0035 keeps.
//
// Two places in the emulated GPU move texture data on the CPU: the texture cache uploads an image whole when the CPU wrote to it
// (or when it is new), and the DMA engine swizzles/unswizzles on the CPU the copies its GPU path declines. A game whose textures blink
// (Super Mario Maker 2, classic theme) shows in boot.log whether it is these that move the data, and how much, every ten seconds.
// The numbers are diagnostics only: nothing in the emulator reads them to decide anything.
//
// The counters are weak definitions: the Eden patch declares them extern, this file defines them, and a program that does not link the patch
// (the tests) sees zeros. Host-testable: tests/jit_cache_test.cpp.
#pragma once

#include <cstdio>
#include <string>

extern "C" {
__attribute__((weak)) unsigned long long ps4_tex_uploads = 0;           // images uploaded whole by the texture cache
__attribute__((weak)) unsigned long long ps4_tex_upload_bytes = 0;      // their bytes
__attribute__((weak)) unsigned long long ps4_tex_upload_largest = 0;    // bytes of the largest single upload
__attribute__((weak)) unsigned long long ps4_dma_cpu_swizzles = 0;      // pitch -> block-linear copies the CPU did
__attribute__((weak)) unsigned long long ps4_dma_cpu_swizzle_bytes = 0;
__attribute__((weak)) unsigned long long ps4_dma_cpu_unswizzles = 0;    // block-linear -> pitch copies the CPU did
__attribute__((weak)) unsigned long long ps4_dma_cpu_unswizzle_bytes = 0;
__attribute__((weak)) unsigned long long ps4_dma_cpu_bl_copies = 0;     // block-linear -> block-linear copies the CPU did
__attribute__((weak)) unsigned long long ps4_dma_cpu_bl_copy_bytes = 0;
}

namespace Ps4TextureTraffic {

struct Counts {
    unsigned long long uploads = 0;
    unsigned long long upload_bytes = 0;
    unsigned long long largest = 0;
    unsigned long long swizzles = 0;
    unsigned long long swizzle_bytes = 0;
    unsigned long long unswizzles = 0;
    unsigned long long unswizzle_bytes = 0;
    unsigned long long bl_copies = 0;
    unsigned long long bl_copy_bytes = 0;
    bool operator==(const Counts& o) const {
        return uploads == o.uploads && upload_bytes == o.upload_bytes && largest == o.largest && swizzles == o.swizzles && swizzle_bytes == o.swizzle_bytes &&
               unswizzles == o.unswizzles && unswizzle_bytes == o.unswizzle_bytes && bl_copies == o.bl_copies && bl_copy_bytes == o.bl_copy_bytes;
    }
    bool operator!=(const Counts& o) const { return !(*this == o); }
};

inline Counts Read() {
    return {ps4_tex_uploads,        ps4_tex_upload_bytes,   ps4_tex_upload_largest,    ps4_dma_cpu_swizzles, ps4_dma_cpu_swizzle_bytes,
            ps4_dma_cpu_unswizzles, ps4_dma_cpu_unswizzle_bytes, ps4_dma_cpu_bl_copies, ps4_dma_cpu_bl_copy_bytes};
}

/// "12.5 MiB", "300 KiB" or "0 B": bytes the way the line shows them.
inline std::string Size(unsigned long long bytes) {
    char text[32];
    if (bytes >= (1ull << 20)) {
        std::snprintf(text, sizeof text, "%.1f MiB", static_cast<double>(bytes) / (1 << 20));
    } else if (bytes >= (1ull << 10)) {
        std::snprintf(text, sizeof text, "%.0f KiB", static_cast<double>(bytes) / (1 << 10));
    } else {
        std::snprintf(text, sizeof text, "%llu B", bytes);
    }
    return text;
}

namespace Detail {
// A counter that went backwards (it never does) gives 0, not a number around 2^64.
inline unsigned long long Delta(unsigned long long before, unsigned long long now) {
    return now >= before ? now - before : 0;
}
} // namespace Detail

/// The boot.log line for the last 'seconds' seconds; empty when nothing moved (the status block then says nothing).
inline std::string Describe(const Counts& before, const Counts& now, unsigned seconds) {
    const unsigned long long uploads = Detail::Delta(before.uploads, now.uploads);
    const unsigned long long swizzles = Detail::Delta(before.swizzles, now.swizzles);
    const unsigned long long unswizzles = Detail::Delta(before.unswizzles, now.unswizzles);
    const unsigned long long bl_copies = Detail::Delta(before.bl_copies, now.bl_copies);
    if (uploads + swizzles + unswizzles + bl_copies == 0) {
        return {};
    }
    char line[512];
    std::snprintf(line, sizeof line,
                  "texture traffic, last %u s: %llu image upload(s), %s (the largest so far %s); copies the GPU path declined and the CPU did: %llu pitch->block-linear (%s), "
                  "%llu block-linear->pitch (%s), %llu block-linear->block-linear (%s)",
                  seconds, uploads, Size(Detail::Delta(before.upload_bytes, now.upload_bytes)).c_str(), Size(now.largest).c_str(), swizzles,
                  Size(Detail::Delta(before.swizzle_bytes, now.swizzle_bytes)).c_str(), unswizzles,
                  Size(Detail::Delta(before.unswizzle_bytes, now.unswizzle_bytes)).c_str(), bl_copies,
                  Size(Detail::Delta(before.bl_copy_bytes, now.bl_copy_bytes)).c_str());
    return line;
}

/// The same for the whole game, for the end of boot.log; empty when nothing was ever counted.
inline std::string DescribeTotals(const Counts& now) {
    if (now == Counts{}) {
        return {};
    }
    char line[512];
    std::snprintf(line, sizeof line,
                  "texture traffic, whole game: %llu image upload(s), %s (the largest %s); CPU copies: %llu pitch->block-linear (%s), %llu block-linear->pitch (%s), "
                  "%llu block-linear->block-linear (%s)",
                  now.uploads, Size(now.upload_bytes).c_str(), Size(now.largest).c_str(), now.swizzles, Size(now.swizzle_bytes).c_str(), now.unswizzles,
                  Size(now.unswizzle_bytes).c_str(), now.bl_copies, Size(now.bl_copy_bytes).c_str());
    return line;
}

} // namespace Ps4TextureTraffic
