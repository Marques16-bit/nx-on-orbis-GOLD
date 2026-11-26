// SPDX-License-Identifier: GPL-3.0-or-later
// 0.9.8: checks of patches/eden-src/astc_recompress.h (ASTC -> BC1/BC3 in slabs, Eden patch 0036). Included by tests/safe_mode_test.cpp (a host test the CI
// workflow already builds: a new test file would not be run there, the workflow cannot be edited from the update zip), like cpu_clock_checks.h.
//
// What runs here (CI and any PC): the slab logic against stand-ins for Eden's decoder and compressor that have the same *shape*: a decoded texel depends on its
// block and its place in the block, a compressed block on its 4x4 texels, a slice on nothing but itself. So a band that starts in the middle of an ASTC block or of a
// BC block, a slice whose output goes to the wrong place, or a slab that reads the wrong blocks, changes the bytes and is caught.
// What does not run in CI: the same comparison with Eden's own ASTC::Decompress and BCN::CompressBC1/3 (tests/astc_recompress_real_test.sh needs the Eden tree);
// it was run on the PC that wrote the patch (see CHANGELOG 0.9.8).

#pragma once

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "../patches/eden-src/astc_recompress.h"

namespace AstcRecompressChecks {

using CheckFn = void (*)(bool, const char*);
namespace Slabs = Tegra::Texture::AstcSlabs;

inline std::uint32_t Mix(std::uint32_t a, std::uint32_t b) {
    a ^= b + 0x9E3779B9u + (a << 6) + (a >> 2);
    return a * 2654435761u;
}

/// Stand-in decoder: every 16-byte block gives a texel by (block bytes, place in the block). Slices are packed one after the other.
struct StandInDecode {
    std::uint32_t bw, bh;
    void operator()(std::span<const std::uint8_t> in, std::uint32_t w, std::uint32_t h, std::uint32_t slices, std::span<std::uint8_t> out) const {
        const std::uint32_t cols = (w + bw - 1) / bw, rows = (h + bh - 1) / bh;
        for (std::uint32_t z = 0; z < slices; ++z) {
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::uint8_t* block = in.data() + (static_cast<std::size_t>(z) * rows * cols + (y / bh) * cols + x / bw) * 16;
                    std::uint32_t v = Mix(x % bw, y % bh);
                    for (int k = 0; k < 16; ++k) {
                        v = Mix(v, block[k]);
                    }
                    std::uint8_t* texel = out.data() + ((static_cast<std::size_t>(z) * h + y) * w + x) * 4;
                    for (int c = 0; c < 4; ++c) {
                        texel[c] = static_cast<std::uint8_t>(v >> (c * 8));
                    }
                }
            }
        }
    }
};

/// Stand-in compressor: a block of 4x4 texels (zero outside the image, as Eden's) gives `block_bytes` bytes. Rows of blocks are packed, slice after slice.
struct StandInCompress {
    std::size_t block_bytes;
    void operator()(std::span<const std::uint8_t> in, std::uint32_t w, std::uint32_t h, std::uint32_t slices, std::span<std::uint8_t> out) const {
        const std::uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
        for (std::uint32_t z = 0; z < slices; ++z) {
            for (std::uint32_t y = 0; y < by; ++y) {
                for (std::uint32_t x = 0; x < bx; ++x) {
                    std::uint32_t v = 17;
                    for (std::uint32_t j = 0; j < 4; ++j) {
                        for (std::uint32_t i = 0; i < 4; ++i) {
                            const std::uint32_t px = x * 4 + i, py = y * 4 + j;
                            for (int c = 0; c < 4; ++c) {
                                const std::uint8_t t = (px < w && py < h) ? in[((static_cast<std::size_t>(z) * h + py) * w + px) * 4 + c] : 0;
                                v = Mix(v, t);
                            }
                        }
                    }
                    std::uint8_t* dst = out.data() + ((static_cast<std::size_t>(z) * by + y) * bx + x) * block_bytes;
                    for (std::size_t k = 0; k < block_bytes; ++k) {
                        dst[k] = static_cast<std::uint8_t>(Mix(v, static_cast<std::uint32_t>(k)) >> 11);
                    }
                }
            }
        }
    }
};

inline bool SameBytes(std::uint32_t w, std::uint32_t h, std::uint32_t slices, std::uint32_t bw, std::uint32_t bh, bool bc1, std::size_t budget,
                      std::mt19937& rng, Slabs::Plan* plan_out = nullptr) {
    const std::size_t cols = (w + bw - 1) / bw, rows = (h + bh - 1) / bh;
    std::vector<std::uint8_t> astc(cols * rows * 16 * slices);
    for (auto& b : astc) {
        b = static_cast<std::uint8_t>(rng());
    }
    const std::size_t block_bytes = bc1 ? 8 : 16;
    const std::size_t out_bytes = block_bytes * ((w + 3) / 4) * ((h + 3) / 4) * slices;
    const StandInDecode decode{bw, bh};
    const StandInCompress compress{block_bytes};

    std::vector<std::uint8_t> whole(static_cast<std::size_t>(w) * h * slices * 4), reference(out_bytes, 0xA5);
    decode(astc, w, h, slices, whole);
    compress(whole, w, h, slices, reference);

    const Slabs::Plan plan = Slabs::MakePlan(w, h, slices, bh, budget);
    std::vector<std::uint8_t> scratch(plan.scratch_bytes), slabbed(out_bytes, 0xA5);
    Slabs::Recompress(astc, w, h, slices, bw, bh, block_bytes, plan, scratch, slabbed, decode, compress);
    if (plan_out != nullptr) {
        *plan_out = plan;
    }
    return slabbed == reference;
}

/// The Eden patch adds the master copy as a new file: its text, from the "+" lines of the patch.
inline std::string FileInPatch(const std::string& patch_path, const std::string& file) {
    std::ifstream in(patch_path);
    std::string line, text;
    bool inside = false, found = false;
    while (std::getline(in, line)) {
        if (line.rfind("diff --git ", 0) == 0) {
            inside = line.find(" b/" + file) != std::string::npos;
            found = found || inside;
            continue;
        }
        if (inside && line.size() >= 1 && line[0] == '+' && line.rfind("+++ ", 0) != 0) {
            text += line.substr(1);
            text += '\n';
        }
    }
    return found ? text : std::string();
}

inline void Run(CheckFn Check) {
    std::printf("ASTC to BC in slabs (0.9.8)\n");
    std::mt19937 rng(0x0098);
    static constexpr std::uint32_t kBlocks[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                                   {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    static constexpr std::size_t kBudgets[] = {1, 64, 300, 1024, 4096, 20000};

    // 1. the same bytes as one piece, on sizes that are multiples of nothing, with budgets that make groups of slices and bands of rows
    {
        bool all = true, cut_in_bands = false, grouped = false;
        for (int i = 0; i < 300; ++i) {
            const auto& b = kBlocks[rng() % 14];
            const std::uint32_t w = 1 + rng() % 40, h = 1 + rng() % 40, slices = 1 + rng() % 4;
            Slabs::Plan plan;
            all = SameBytes(w, h, slices, b[0], b[1], (rng() & 1) != 0, kBudgets[rng() % 6], rng, &plan) && all;
            cut_in_bands = cut_in_bands || plan.rows_per_band < h;
            grouped = grouped || (plan.slices_per_slab > 1 && plan.slices_per_slab < slices);
        }
        Check(all, "slabs give the bytes of one piece (300 images, every ASTC block size, BC1 and BC3)");
        Check(cut_in_bands, "the budgets used did cut slices in bands");
        Check(grouped, "the budgets used did group some slices and not all");
    }
    // 2. every block size on one image that is a multiple of nothing
    {
        bool all = true;
        for (const auto& b : kBlocks) {
            all = SameBytes(37, 29, 2, b[0], b[1], true, 700, rng) && all;
            all = SameBytes(37, 29, 2, b[0], b[1], false, 700, rng) && all;
        }
        Check(all, "every ASTC block size, BC1 and BC3, a 37 x 29 image of 2 layers cut in bands");
    }
    // 3. the plan: the case of the log of 19/11 (a 96 MiB block) and a 121-layer array
    {
        const Slabs::Plan big = Slabs::MakePlan(2048, 2048, 6, 6);
        Check(big.scratch_bytes <= Slabs::DefaultSlabBytes, "2048 x 2048 x 6: the scratch is at most one slab (8 MiB), not 96 MiB");
        Check(big.slices_per_slab == 1 && big.rows_per_band < 2048 && big.rows_per_band % 12 == 0,
              "2048 x 2048: a slice is cut in bands of whole block rows (a multiple of 12 for 6x6 blocks)");
        const Slabs::Plan array = Slabs::MakePlan(256, 256, 121, 4);
        Check(array.slices_per_slab > 1 && array.slices_per_slab < 121 && array.scratch_bytes <= Slabs::DefaultSlabBytes,
              "256 x 256 x 121: layers are grouped up to one slab, not all at once");
        const Slabs::Plan tiny = Slabs::MakePlan(8, 8, 1, 8);
        Check(tiny.scratch_bytes == 8 * 8 * 4 && tiny.rows_per_band == 8, "a small image is one slab of its own size");
        const Slabs::Plan odd = Slabs::MakePlan(16384, 4096, 1, 10, 1);
        Check(odd.rows_per_band == 20 && odd.scratch_bytes == std::size_t(16384) * 4 * 20,
              "a budget smaller than one band still gives one band (20 rows for 10-high blocks: lcm(10, 4))");
        const Slabs::Plan none = Slabs::MakePlan(0, 5, 3, 4);
        Check(none.scratch_bytes == 0, "an empty image needs no scratch");
    }
    // 4. the copy that the Eden patch adds is the master copy, byte for byte
    {
        std::ifstream master("patches/eden-src/astc_recompress.h");
        const std::string master_text((std::istreambuf_iterator<char>(master)), std::istreambuf_iterator<char>());
        std::string patch_text;
        for (const char* name : {"patches/eden/0036-PS4-recompress-ASTC-in-slabs.patch"}) {
            patch_text = FileInPatch(name, "src/video_core/textures/astc_recompress.h");
        }
        Check(!master_text.empty(), "patches/eden-src/astc_recompress.h can be read (the test runs from the top of the repository)");
        Check(!patch_text.empty() && patch_text == master_text, "patch 0036 adds the master copy of astc_recompress.h to Eden, the same text");
    }
}

} // namespace AstcRecompressChecks
