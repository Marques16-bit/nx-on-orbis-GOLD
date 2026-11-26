// SPDX-License-Identifier: GPL-3.0-or-later
// 0.9.8: patches/eden-src/astc_recompress.h against Eden's own ASTC decoder and BC1/BC3 compressors (video_core/textures/astc.cpp,
// bcn.cpp, externals/stb/stb_dxt.cpp): the level converted in slabs must be the same bytes as the level converted in one piece.
// Build and run with tests/astc_recompress_real_test.sh (needs the Eden tree of scripts/fetch-eden.sh in deps/eden).
// The thread pool of Eden is replaced by tests/astc_recompress_shim/ (work items run at once): the items are independent.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../patches/eden-src/astc_recompress.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/bcn.h"

namespace {
using namespace Tegra::Texture;
int g_checks = 0, g_failed = 0;

void Check(bool ok, const char* what, int a = 0, int b = 0, int c = 0, int d = 0) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s (w=%d h=%d slices=%d detail=%d)\n", what, a, b, c, d);
    }
}

// ASTC blocks: a mix of void-extent blocks (a flat colour, with every alpha) and random bytes (whatever mode they happen to be).
void FillAstc(std::vector<std::uint8_t>& data, std::mt19937& rng) {
    for (std::size_t i = 0; i + 16 <= data.size(); i += 16) {
        if (rng() % 3 != 0) {
            data[i] = 0xFC;
            data[i + 1] = 0xFD;
            for (int k = 2; k < 8; ++k) {
                data[i + k] = 0xFF;
            }
            for (int k = 8; k < 16; ++k) {
                data[i + k] = static_cast<std::uint8_t>(rng());
            }
        } else {
            for (int k = 0; k < 16; ++k) {
                data[i + k] = static_cast<std::uint8_t>(rng());
            }
        }
    }
}

struct Size {
    std::uint32_t w, h;
};

void One(std::mt19937& rng, std::uint32_t w, std::uint32_t h, std::uint32_t slices, std::uint32_t bw, std::uint32_t bh, bool bc1,
         std::size_t slab_bytes) {
    const std::size_t cols = (w + bw - 1) / bw, rows = (h + bh - 1) / bh;
    std::vector<std::uint8_t> astc(cols * rows * 16 * slices);
    FillAstc(astc, rng);
    const std::size_t block_bytes = bc1 ? 8 : 16;
    const std::size_t out_bytes = block_bytes * ((w + 3) / 4) * ((h + 3) / 4) * slices;

    // one piece, as Eden does today
    std::vector<std::uint8_t> whole_rgba(static_cast<std::size_t>(w) * h * slices * 4);
    ASTC::Decompress(astc, w, h, slices, bw, bh, whole_rgba);
    std::vector<std::uint8_t> reference(out_bytes, 0xA5);
    (bc1 ? BCN::CompressBC1 : BCN::CompressBC3)(whole_rgba, w, h, slices, reference);

    // in slabs
    const auto plan = AstcSlabs::MakePlan(w, h, slices, bh, slab_bytes);
    std::vector<std::uint8_t> scratch(plan.scratch_bytes);
    std::vector<std::uint8_t> slabbed(out_bytes, 0xA5);
    AstcSlabs::Recompress(
        astc, w, h, slices, bw, bh, block_bytes, plan, scratch, slabbed,
        [&](std::span<const std::uint8_t> in, std::uint32_t ww, std::uint32_t hh, std::uint32_t n, std::span<std::uint8_t> out) {
            ASTC::Decompress(in, ww, hh, n, bw, bh, out);
        },
        bc1 ? BCN::CompressBC1 : BCN::CompressBC3);

    Check(slabbed == reference, bc1 ? "slabs give the bytes of one piece (BC1)" : "slabs give the bytes of one piece (BC3)", int(w),
          int(h), int(slices), int(bw * 100 + bh));
    Check(plan.scratch_bytes <= std::max<std::size_t>(slab_bytes, static_cast<std::size_t>(std::lcm(bh, 4u)) * w * 4),
          "the scratch of a slab is within the budget (or one band of block rows)", int(w), int(h), int(slices));
}
} // namespace

int main() {
    std::mt19937 rng(0x5EED0098);
    static constexpr Size kBlocks[] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                       {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    // small budgets so that groups of slices and bands of rows both happen, on sizes that are not multiples of anything
    static constexpr std::size_t kBudgets[] = {1, 64, 300, 1024, 4096, 20000};
    int runs = 0;
    for (int i = 0; i < 400; ++i) {
        const Size block = kBlocks[rng() % 14];
        const std::uint32_t w = 1 + rng() % 70, h = 1 + rng() % 70, slices = 1 + rng() % 5;
        One(rng, w, h, slices, block.w, block.h, (rng() & 1) != 0, kBudgets[rng() % 6]);
        ++runs;
    }
    // every block size once, on an image that is a multiple of nothing, with a budget that cuts it in bands
    for (const Size block : kBlocks) {
        One(rng, 67, 53, 3, block.w, block.h, true, 900);
        One(rng, 67, 53, 3, block.w, block.h, false, 900);
        runs += 2;
    }
    // the real budget: 2048 x 2048 (16 MiB a slice: cut in bands), 6 layers, BC1: 96 MiB of RGBA8 in one piece, the size of the big block of the
    // log of 19/11 (that the block was this texture is an inference from the size, not something the log says)
    {
        const auto plan = AstcSlabs::MakePlan(2048, 2048, 6, 6);
        Check(plan.scratch_bytes <= AstcSlabs::DefaultSlabBytes, "2048x2048x6: the scratch is at most one slab, not the whole level");
        Check(plan.scratch_bytes * 12 <= std::size_t(2048) * 2048 * 4 * 6, "2048x2048x6: the scratch is a twelfth of the old 96 MiB at most");
        One(rng, 2048, 2048, 2, 6, 6, true, AstcSlabs::DefaultSlabBytes);
        ++runs;
    }
    // many small layers share a slab (a 121-layer array of 256 x 256 is 30 MiB of RGBA8 in one piece; the run itself is a quick one)
    {
        const auto plan = AstcSlabs::MakePlan(256, 256, 121, 4);
        Check(plan.slices_per_slab > 1 && plan.slices_per_slab < 121, "256x256x121: layers are grouped, not all at once");
        One(rng, 256, 256, 9, 4, 4, false, AstcSlabs::DefaultSlabBytes / 8);
        ++runs;
    }
    std::printf("astc_recompress_real_test: %d images, %d checks, %d failed\n", runs, g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
