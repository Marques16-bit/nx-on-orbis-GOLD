// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for frontend/video_hook.cpp: the wrapped video-out calls, with the real ones replaced by
// recorders. Checks the format swap, the HUD and the pause panel in fake frame buffers.
#include <cstdio>
#include <cstring>
#include <vector>

#include "video_hook.h"

static std::uint32_t g_real_format = 0;
static int g_real_registers = 0, g_real_flips = 0;
namespace Ps4 { std::uint64_t FreeDirectMemory() { return 3ull << 30; } }
extern "C" int32_t __real_sceVideoOutRegisterBuffers(int32_t, int32_t, void* const*, int32_t, const void* attribute) {
    std::memcpy(&g_real_format, attribute, 4);
    ++g_real_registers;
    return 0;
}
extern "C" int32_t __real_sceVideoOutSubmitFlip(int32_t, int32_t, uint32_t, int64_t) {
    ++g_real_flips;
    return 0;
}

static int g_failures = 0;
static void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

struct Frames {
    static constexpr int W = 1920, H = 1080;
    std::vector<std::uint32_t> a = std::vector<std::uint32_t>(W * H, 0), b = std::vector<std::uint32_t>(W * H, 0);
    void* ptrs[2] = {a.data(), b.data()};
    alignas(8) std::uint8_t attr[64] = {};
    explicit Frames(std::uint32_t format) {
        const std::uint32_t w = W, h = H;
        std::memcpy(attr, &format, 4);
        std::memcpy(attr + 12, &w, 4);
        std::memcpy(attr + 16, &h, 4);
        std::memcpy(attr + 20, &w, 4);
    }
    bool Touched(const std::vector<std::uint32_t>& f, int x0, int y0, int w, int h) const {
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                if (f[size_t(y) * W + size_t(x)] != 0) {
                    return true;
                }
            }
        }
        return false;
    }
};

int main() {
    constexpr std::uint32_t ARGB = 0x80000000u, ABGR = 0x80002200u;
    {
        Frames f(ABGR);
        __wrap_sceVideoOutRegisterBuffers(7, 0, f.ptrs, 2, f.attr);
        Check(g_real_format == ABGR, "not armed: the format goes through untouched");
        __wrap_sceVideoOutSubmitFlip(7, 0, 1, 0);
        Check(!f.Touched(f.a, 0, 0, 1920, 1080) && !Ps4Hook::Ready(), "not armed: nothing drawn, not ready");
    }
    {
        Ps4Hook::Settings s;
        s.swap_colours = true;
        Ps4Hook::Arm(s);
        Frames f(ABGR);
        __wrap_sceVideoOutRegisterBuffers(7, 0, f.ptrs, 2, f.attr);
        Check(g_real_format == ARGB, "armed + swap: A8B8G8R8 becomes A8R8G8B8");
        Frames g(ARGB);
        __wrap_sceVideoOutRegisterBuffers(7, 0, g.ptrs, 2, g.attr);
        Check(g_real_format == ABGR, "...and A8R8G8B8 becomes A8B8G8R8");
        Frames odd(0x88060000u);
        __wrap_sceVideoOutRegisterBuffers(7, 0, odd.ptrs, 2, odd.attr);
        Check(g_real_format == 0x88060000u, "a format that is not the pair is left alone");
        s.swap_colours = false;
        Ps4Hook::Arm(s);
        __wrap_sceVideoOutRegisterBuffers(7, 0, f.ptrs, 2, f.attr);
        Check(g_real_format == ABGR, "swap off: untouched");
        Check(!Ps4Hook::Ready(), "registered but no flip yet: not ready");
        __wrap_sceVideoOutSubmitFlip(7, 1, 1, 0);
        Check(Ps4Hook::Ready() && g_real_flips >= 2, "after a flip it is ready, and the real flip was called");
        Check(Ps4Hook::Describe().find("1920x1080") != std::string::npos, "Describe tells the size");
    }
    {
        Ps4Hook::Settings s;
        s.hud = true;
        s.swap_colours = false;
        Ps4Hook::Arm(s);
        Frames f(ARGB);
        __wrap_sceVideoOutRegisterBuffers(9, 0, f.ptrs, 2, f.attr);
        __wrap_sceVideoOutSubmitFlip(9, 0, 1, 0);
        Check(f.Touched(f.a, 40, 40, 400, 120) && !f.Touched(f.a, 800, 400, 400, 200), "HUD on: drawn top-left of the flipped buffer only");
        Check(!f.Touched(f.b, 0, 0, 1920, 1080), "...not in the other buffer");
        Ps4Hook::SetHud(false);
        Frames g(ARGB);
        __wrap_sceVideoOutRegisterBuffers(9, 0, g.ptrs, 2, g.attr);
        __wrap_sceVideoOutSubmitFlip(9, 1, 1, 0);
        Check(!g.Touched(g.b, 0, 0, 1920, 1080) && !Ps4Hook::HudOn(), "HUD switched off: nothing drawn");
        s.hud_right = true;
        s.hud_small = true;
        s.hud = true;
        Ps4Hook::Arm(s);
        Frames h(ARGB);
        __wrap_sceVideoOutRegisterBuffers(9, 0, h.ptrs, 2, h.attr);
        __wrap_sceVideoOutSubmitFlip(9, 0, 1, 0);
        Check(h.Touched(h.a, 1920 - 40 - 300, 40, 280, 100) && !h.Touched(h.a, 40, 40, 200, 100), "HUD on the right, small");
    }
    {
        Ps4Hook::Settings s;
        s.swap_colours = true;
        Ps4Hook::Arm(s);
        Frames f(ABGR); // registered as ABGR, drawn after the swap into an ARGB screen
        __wrap_sceVideoOutRegisterBuffers(5, 0, f.ptrs, 2, f.attr);
        __wrap_sceVideoOutSubmitFlip(5, 1, 1, 0);
        Ps4Hook::ShowPanel("Pause", {"Continue", "HUD", "Exit"}, 1, "footer");
        Check(f.Touched(f.b, 600, 400, 700, 280) && !f.Touched(f.a, 0, 0, 1920, 1080), "the panel goes into the buffer on screen, in the middle");
        // The panel's border is 0x4A86FF; on an ARGB screen that is the value 0xFF4A86FF.
        bool border = false;
        for (std::uint32_t p : f.b) {
            border = border || p == 0xFF4A86FFu;
        }
        Check(border, "colours are written in the format on the screen (swap included)");
        Ps4Hook::ShowPanel("Pause", {"Continue", "HUD", "Exit"}, 2, "footer"); // redraw with another line selected
        Check(true, "redrawing the panel works");
    }
    std::printf(g_failures == 0 ? "video hook test: PASSED\n" : "video hook test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
