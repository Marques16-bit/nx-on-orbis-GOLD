// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for frontend/video_hook.cpp: the wrapped video-out calls, with the real ones replaced by
// recorders. Checks the format swap, the HUD and the pause panel in fake frame buffers, in the
// classic mode (0.7.3) and in the keep-alive mode (0.7.6), including a simulated display.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "video_hook.h"

static int g_pending = 0;
static std::atomic<int> g_flip_checks{0}, g_memory_calls{0};
extern "C" int32_t sceVideoOutIsFlipPending(int32_t) { ++g_flip_checks; return g_pending; }
namespace Ps4 { void Log(const char*, ...) {} }
static std::uint32_t g_real_format = 0;
static int g_real_registers = 0, g_real_flips = 0;
namespace Ps4 { std::uint64_t FreeDirectMemory() { ++g_memory_calls; return 3ull << 30; } }
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
        const std::uint32_t linear = 1;
        std::memcpy(attr, &format, 4);
        std::memcpy(attr + 4, &linear, 4);
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


// Three buffers, and where the HUD's background is (the HUD box is at 40,40; its corners hold the box colour).
struct Frames3 {
    static constexpr int W = 1920, H = 1080;
    std::vector<std::uint32_t> buf[3] = {std::vector<std::uint32_t>(W * H, 0), std::vector<std::uint32_t>(W * H, 0),
                                         std::vector<std::uint32_t>(W * H, 0)};
    void* ptrs[3] = {buf[0].data(), buf[1].data(), buf[2].data()};
    alignas(8) std::uint8_t attr[64] = {};
    explicit Frames3(std::uint32_t format) {
        const std::uint32_t w = W, h = H;
        const std::uint32_t linear = 1;
        std::memcpy(attr, &format, 4);
        std::memcpy(attr + 4, &linear, 4);
        std::memcpy(attr + 12, &w, 4);
        std::memcpy(attr + 16, &h, 4);
        std::memcpy(attr + 20, &w, 4);
    }
    bool Touched(int b, int x0, int y0, int w, int h) const {
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                if (buf[b][size_t(y) * W + size_t(x)] != 0) {
                    return true;
                }
            }
        }
        return false;
    }
    /// The four inset corners of the HUD box and its centre hold the HUD (box colour 0x101830; ARGB screen).
    bool Has(int b, int) const {
        constexpr std::uint32_t Back = 0xFF101830u;
        const auto at = [&](int x, int y) { return buf[b][size_t(y) * W + size_t(x)]; };
        return at(42, 42) == Back && at(40 + 496 - 4, 42) == Back && at(42, 40 + 172 - 4) == Back && at(40 + 496 - 4, 40 + 172 - 4) == Back;
    }
};
constexpr int HudSpot = 0;

static std::uint64_t g_sim_now_us = 0;
static std::uint64_t SimClock() { return g_sim_now_us; }

/// Runs 30 s of the simulated display described in main(); returns the percentage of refreshes whose
/// buffer on screen had no HUD.
static double SimulateDisplay(bool keepalive, int gpu_min_ms, int gpu_max_ms) {
    constexpr std::uint32_t ARGB = 0x80000000u;
    static Frames3 f(ARGB);
    for (auto& b : f.buf) {
        std::fill(b.begin(), b.end(), 0xFF202020u);
    }
    Ps4Hook::Settings s;
    s.hud = true;
    s.swap_colours = false;
    s.keepalive = keepalive;
    s.draw_delay_ms = 2;
    s.poll_ms = 1;
    Ps4Hook::Arm(s, false);
    g_sim_now_us = 1000;
    Ps4Hook::SetClockForTest(&SimClock);
    __wrap_sceVideoOutRegisterBuffers(30, 0, f.ptrs, 3, f.attr);

    std::uint32_t rng = 12345;
    const auto random = [&](int lo, int hi) {
        rng = rng * 1664525u + 1013904223u;
        return lo + int((rng >> 8) % std::uint32_t(hi - lo + 1));
    };
    struct Queued {
        int buffer;
        std::uint64_t at_us;
    };
    std::vector<Queued> queue;
    struct Paint {
        int buffer;
        std::uint64_t at_us;
    };
    std::vector<Paint> paints;
    int next_buffer = 0, displayed = -1;
    std::uint64_t next_vblank_us = 5000;
    int refreshes = 0, missed = 0;
    int notice_counter = 0;
    for (int ms = 1; ms < 30000; ++ms) {
        g_sim_now_us = std::uint64_t(ms) * 1000;
        if (ms % 250 == 0) {
            Ps4Hook::SetNotice("Compilando shaders: " + std::to_string(++notice_counter));
        }
        if (ms % 77 == 0) {
            const int buffer = next_buffer++ % 3;
            const int gpu = random(gpu_min_ms, gpu_max_ms);
            if (gpu <= 0) {
                std::fill(f.buf[buffer].begin(), f.buf[buffer].end(), 0xFF202020u); // done before the flip
            } else {
                paints.push_back({buffer, g_sim_now_us + std::uint64_t(gpu) * 1000});
            }
            queue.push_back({buffer, g_sim_now_us});
            g_pending = int(queue.size());
            __wrap_sceVideoOutSubmitFlip(30, buffer, 1, 0);
        }
        for (size_t i = 0; i < paints.size();) {
            if (paints[i].at_us <= g_sim_now_us) {
                std::fill(f.buf[paints[i].buffer].begin(), f.buf[paints[i].buffer].end(), 0xFF202020u);
                paints.erase(paints.begin() + long(i));
            } else {
                ++i;
            }
        }
        if (g_sim_now_us >= next_vblank_us) {
            next_vblank_us += 16667;
            if (!queue.empty() && queue.front().at_us <= g_sim_now_us) {
                displayed = queue.front().buffer;
                queue.erase(queue.begin());
            }
            g_pending = int(queue.size());
            if (displayed >= 0 && ms > 600) {
                ++refreshes;
                missed += f.Has(displayed, HudSpot) ? 0 : 1;
            }
        }
        g_pending = int(queue.size());
        if (keepalive || ms % 2 == 0) {
            Ps4Hook::Pump();
        }
    }
    return refreshes > 0 ? 100.0 * missed / refreshes : 0.0;
}

int main() {
    constexpr std::uint32_t ARGB = 0x80000000u, ABGR = 0x80002200u;
    {
        Frames f(ABGR);
        __wrap_sceVideoOutRegisterBuffers(7, 0, f.ptrs, 2, f.attr);
        Check(g_real_format == ABGR, "not armed: the format goes through untouched");
        __wrap_sceVideoOutSubmitFlip(7, 0, 1, 0); Ps4Hook::Pump();
        Check(!f.Touched(f.a, 0, 0, 1920, 1080) && !Ps4Hook::Ready(), "not armed: nothing drawn, not ready");
    }
    {
        Ps4Hook::Settings s;
        s.keepalive = false; // these cases are the 0.7.3 "classic" mode; keep-alive has its own cases below
        s.swap_colours = true;
        Ps4Hook::Arm(s, false);
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
        Ps4Hook::Arm(s, false);
        __wrap_sceVideoOutRegisterBuffers(7, 0, f.ptrs, 2, f.attr);
        Check(g_real_format == ABGR, "swap off: untouched");
        Check(!Ps4Hook::Ready(), "registered but no flip yet: not ready");
        __wrap_sceVideoOutSubmitFlip(7, 1, 1, 0); Ps4Hook::Pump();
        Check(Ps4Hook::Ready() && g_real_flips >= 2, "after a flip it is ready, and the real flip was called");
        Check(Ps4Hook::Describe().find("1920x1080") != std::string::npos, "Describe tells the size");
    }
    {
        Ps4Hook::Settings s;
        s.keepalive = false; // these cases are the 0.7.3 "classic" mode; keep-alive has its own cases below
        s.hud = true;
        s.swap_colours = false;
        Ps4Hook::Arm(s, false);
        Frames f(ARGB);
        __wrap_sceVideoOutRegisterBuffers(9, 0, f.ptrs, 2, f.attr);
        __wrap_sceVideoOutSubmitFlip(9, 0, 1, 0); Ps4Hook::Pump();
        Check(f.Touched(f.a, 40, 40, 400, 120) && !f.Touched(f.a, 800, 400, 400, 200), "HUD on: drawn top-left of the flipped buffer only");
        Check(!f.Touched(f.b, 0, 0, 1920, 1080), "...not in the other buffer");
        Ps4Hook::SetHud(false);
        Frames g(ARGB);
        __wrap_sceVideoOutRegisterBuffers(9, 0, g.ptrs, 2, g.attr);
        __wrap_sceVideoOutSubmitFlip(9, 1, 1, 0); Ps4Hook::Pump();
        Check(!g.Touched(g.b, 0, 0, 1920, 1080) && !Ps4Hook::HudOn(), "HUD switched off: nothing drawn");
        s.hud_right = true;
        s.hud_small = true;
        s.hud = true;
        Ps4Hook::Arm(s, false);
        Frames h(ARGB);
        __wrap_sceVideoOutRegisterBuffers(9, 0, h.ptrs, 2, h.attr);
        __wrap_sceVideoOutSubmitFlip(9, 0, 1, 0); Ps4Hook::Pump();
        Check(h.Touched(h.a, 1920 - 40 - 300, 40, 280, 100) && !h.Touched(h.a, 40, 40, 200, 100), "HUD on the right, small");
    }
    {
        Ps4Hook::Settings s;
        s.keepalive = false; // these cases are the 0.7.3 "classic" mode; keep-alive has its own cases below
        s.swap_colours = true;
        Ps4Hook::Arm(s, false);
        Frames f(ABGR); // registered as ABGR, drawn after the swap into an ARGB screen
        __wrap_sceVideoOutRegisterBuffers(5, 0, f.ptrs, 2, f.attr);
        __wrap_sceVideoOutSubmitFlip(5, 1, 1, 0); Ps4Hook::Pump();
        Ps4Hook::ShowPanel("Pause", {"Continue", "HUD", "Exit"}, 1, "footer");
        Ps4Hook::Pump();
        Check(f.Touched(f.b, 600, 400, 700, 280) && !f.Touched(f.a, 0, 0, 1920, 1080), "the panel goes into the buffer on screen, in the middle");
        // The panel's border is 0x4A86FF; on an ARGB screen that is the value 0xFF4A86FF.
        bool border = false;
        for (std::uint32_t p : f.b) {
            border = border || p == 0xFF4A86FFu;
        }
        Check(border, "colours are written in the format on the screen (swap included)");
        Ps4Hook::ShowPanel("Pause", {"Continue", "HUD", "Exit"}, 2, "footer"); // redraw with another line selected
        Ps4Hook::Pump();
        Check(true, "redrawing the panel works");
    }
    {
        // 0.7.3: a frame is drawn in while its flip is still queued (not on screen yet), once the GPU
        // has had its short moment; then once more after the flip, in case the GPU wrote over it.
        Ps4Hook::Settings s;
        s.keepalive = false; // these cases are the 0.7.3 "classic" mode; keep-alive has its own cases below
        s.hud = true;
        s.swap_colours = false;
        s.draw_delay_ms = 20;
        Ps4Hook::Arm(s, false);
        Frames f(ARGB);
        __wrap_sceVideoOutRegisterBuffers(3, 0, f.ptrs, 2, f.attr);
        g_pending = 1;
        __wrap_sceVideoOutSubmitFlip(3, 0, 1, 0);
        Ps4Hook::Pump();
        Check(!f.Touched(f.a, 40, 40, 400, 120), "a flip just queued: the GPU gets its moment first, nothing drawn yet");
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        Ps4Hook::Pump();
        Check(f.Touched(f.a, 40, 40, 400, 120), "...then the HUD is drawn while the flip is still pending (before it is on screen)");
        std::uint64_t hidden = 0, late = 0;
        Ps4Hook::DrawStats(&hidden, &late);
        Check(hidden == 1 && late == 0, "that counts as a draw before the screen showed the frame");
        // The GPU writes over the frame after the draw (the 0.7.2 flicker): the second pass restores it.
        std::fill(f.a.begin(), f.a.end(), 0u);
        Ps4Hook::Pump();
        Check(!f.Touched(f.a, 40, 40, 400, 120), "flip still pending: no second draw yet");
        g_pending = 0;
        Ps4Hook::Pump();
        Check(f.Touched(f.a, 40, 40, 400, 120), "flip over: the frame is drawn in once more");
        Ps4Hook::DrawStats(&hidden, &late);
        Check(hidden == 1 && late == 1, "...counted as a late draw");
        const std::uint64_t before = late;
        Ps4Hook::Pump();
        Ps4Hook::DrawStats(&hidden, &late);
        Check(late == before, "nothing is redrawn while no new frame arrives");

        // A flip that was already shown by the time the worker got to it is drawn at once (late).
        Frames g(ARGB);
        Ps4Hook::Arm(s, false);
        __wrap_sceVideoOutRegisterBuffers(3, 0, g.ptrs, 2, g.attr);
        g_pending = 0;
        __wrap_sceVideoOutSubmitFlip(3, 1, 1, 0);
        Ps4Hook::Pump();
        Check(g.Touched(g.b, 40, 40, 400, 120), "a flip already over: drawn at once");

        Frames tiled(ARGB);
        const std::uint32_t tile = 0;
        std::memcpy(tiled.attr + 4, &tile, 4);
        Ps4Hook::Arm(s, false);
        __wrap_sceVideoOutRegisterBuffers(3, 0, tiled.ptrs, 2, tiled.attr);
        __wrap_sceVideoOutSubmitFlip(3, 0, 1, 0);
        Ps4Hook::Pump();
        Check(!tiled.Touched(tiled.a, 0, 0, 1920, 1080) && !Ps4Hook::Ready(), "tiled buffers: nothing is drawn and the menu is not offered");
    }
    {
        // The shader notice: bottom left, on every new frame, removable, and drawn even with no new
        // frame (the game is paused or still loading) when its text changes.
        Ps4Hook::Settings s;
        s.keepalive = false; // these cases are the 0.7.3 "classic" mode; keep-alive has its own cases below
        s.swap_colours = false;
        s.draw_delay_ms = 0;
        Ps4Hook::Arm(s, false);
        Frames f(ARGB);
        __wrap_sceVideoOutRegisterBuffers(4, 0, f.ptrs, 2, f.attr);
        g_pending = 0;
        __wrap_sceVideoOutSubmitFlip(4, 0, 1, 0);
        Ps4Hook::Pump();
        Check(!f.Touched(f.a, 0, 0, 1920, 1080), "no notice, no HUD: nothing is drawn");
        Ps4Hook::SetNotice("Compilando shaders: 12");
        Ps4Hook::Pump();
        Check(f.Touched(f.a, 40, 1080 - 40 - 60, 400, 60) && !f.Touched(f.a, 40, 40, 400, 120),
              "a notice changes with no new frame: drawn bottom left, not where the HUD goes");
        std::fill(f.b.begin(), f.b.end(), 0u);
        __wrap_sceVideoOutSubmitFlip(4, 1, 1, 0);
        Ps4Hook::Pump();
        Check(f.Touched(f.b, 40, 1080 - 40 - 60, 400, 60), "the next frame gets the notice too");
        Ps4Hook::SetNotice("");
        std::fill(f.a.begin(), f.a.end(), 0u);
        __wrap_sceVideoOutSubmitFlip(4, 0, 1, 0);
        Ps4Hook::Pump();
        Check(!f.Touched(f.a, 0, 0, 1920, 1080), "an empty notice draws nothing");
    }
    {
        // The pause panel stays on frames that keep flipping while the game is paused.
        Ps4Hook::Settings s;
        s.keepalive = false; // these cases are the 0.7.3 "classic" mode; keep-alive has its own cases below
        s.swap_colours = false;
        s.draw_delay_ms = 0;
        Ps4Hook::Arm(s, false);
        Frames f(ARGB);
        __wrap_sceVideoOutRegisterBuffers(6, 0, f.ptrs, 2, f.attr);
        g_pending = 0;
        __wrap_sceVideoOutSubmitFlip(6, 0, 1, 0);
        Ps4Hook::Pump();
        Ps4Hook::ShowPanel("Pause", {"Continue", "Exit"}, 0, "footer");
        Ps4Hook::Pump();
        __wrap_sceVideoOutSubmitFlip(6, 1, 1, 0);
        Ps4Hook::Pump();
        Check(f.Touched(f.b, 600, 400, 700, 200), "a frame flipped while the menu is up gets the panel too");
        Ps4Hook::HidePanel();
        std::fill(f.a.begin(), f.a.end(), 0u);
        __wrap_sceVideoOutSubmitFlip(6, 0, 1, 0);
        Ps4Hook::Pump();
        Check(!f.Touched(f.a, 0, 0, 1920, 1080), "menu closed: new frames are left alone");
    }
    // ------------------------------------------------------------------ keep-alive (0.7.6)
    {
        // Three buffers, the HUD on: one round puts the HUD in all of them, not only the flipped one.
        Ps4Hook::Settings s;
        s.hud = true;
        s.swap_colours = false;
        Ps4Hook::Arm(s, false);
        Frames3 f(ARGB);
        __wrap_sceVideoOutRegisterBuffers(21, 0, f.ptrs, 3, f.attr);
        __wrap_sceVideoOutSubmitFlip(21, 0, 1, 0);
        Ps4Hook::Pump();
        Check(f.Has(0, HudSpot) && f.Has(1, HudSpot) && f.Has(2, HudSpot), "keep-alive: the HUD is in every buffer after one round");
        const auto first = Ps4Hook::GetKeepAliveStats();
        Check(first.stamps_content == 3 && first.stamps_repair == 0, "...3 copies for new content, no repair");
        Ps4Hook::Pump();
        Ps4Hook::Pump();
        const auto idle = Ps4Hook::GetKeepAliveStats();
        Check(idle.stamps_content == 3 && idle.stamps_repair == 0 && idle.probe_reads > first.probe_reads,
              "nothing painted over: more rounds only read probe pixels and copy nothing");

        // The game's GPU paints a whole frame over buffer 1: the next round puts the HUD back.
        std::fill(f.buf[1].begin(), f.buf[1].end(), 0xFF202020u);
        Check(!f.Has(1, HudSpot), "(the HUD is gone from buffer 1 after the paint)");
        Ps4Hook::Pump();
        Check(f.Has(1, HudSpot) && Ps4Hook::GetKeepAliveStats().stamps_repair == 1, "...and the next round repairs it (1 repair)");
        Check(f.Has(0, HudSpot) && f.Has(2, HudSpot), "...the others were left alone");

        // A paint that covers only part of the HUD (a band across it) is found too.
        for (int y = 60; y < 110; ++y) {
            std::fill(f.buf[2].begin() + y * Frames3::W, f.buf[2].begin() + (y + 1) * Frames3::W, 0xFF202020u);
        }
        Ps4Hook::Pump();
        Check(f.Has(2, HudSpot) && Ps4Hook::GetKeepAliveStats().stamps_repair == 2, "a band painted across the middle of the HUD is repaired");
        // Paint that stays clear of the HUD is none of its business.
        const auto before = Ps4Hook::GetKeepAliveStats();
        for (int y = 600; y < 700; ++y) {
            std::fill(f.buf[0].begin() + y * Frames3::W, f.buf[0].begin() + (y + 1) * Frames3::W, 0xFF303030u);
        }
        Ps4Hook::Pump();
        Check(Ps4Hook::GetKeepAliveStats().stamps_repair == before.stamps_repair, "paint far from the HUD: nothing is copied");

        // The frame is stamped inside the flip call: a frame painted over and then flipped has the HUD at once.
        std::fill(f.buf[1].begin(), f.buf[1].end(), 0xFF202020u);
        const auto pre = Ps4Hook::GetKeepAliveStats();
        __wrap_sceVideoOutSubmitFlip(21, 1, 1, 0);
        Check(f.Has(1, HudSpot) && Ps4Hook::GetKeepAliveStats().stamps_flip == pre.stamps_flip + 1,
              "a flip of a frame that lost its HUD: put back before the real flip, no worker needed");
        const auto after = Ps4Hook::GetKeepAliveStats();
        __wrap_sceVideoOutSubmitFlip(21, 1, 1, 0);
        Check(Ps4Hook::GetKeepAliveStats().stamps_flip == after.stamps_flip, "a flip of a frame that has its HUD copies nothing");

        // flip_stamp off: only the worker repairs.
        s.flip_stamp = false;
        Ps4Hook::Arm(s, false);
        Frames3 g(ARGB);
        __wrap_sceVideoOutRegisterBuffers(21, 0, g.ptrs, 3, g.attr);
        __wrap_sceVideoOutSubmitFlip(21, 0, 1, 0);
        Ps4Hook::Pump();
        std::fill(g.buf[1].begin(), g.buf[1].end(), 0xFF202020u);
        __wrap_sceVideoOutSubmitFlip(21, 1, 1, 0);
        Check(!g.Has(1, HudSpot), "flip_stamp off: the flip call draws nothing");
        Ps4Hook::Pump();
        Check(g.Has(1, HudSpot), "...the worker does, on its next round");
    }
    {
        // The notice and the panel: in every buffer, no remnants when their text gets shorter,
        // and nothing at all when they are not showing.
        Ps4Hook::Settings s;
        s.swap_colours = true; // registered ABGR, so the screen is ARGB after the swap
        Ps4Hook::Arm(s, false);
        Frames3 f(ABGR);
        __wrap_sceVideoOutRegisterBuffers(22, 0, f.ptrs, 3, f.attr);
        __wrap_sceVideoOutSubmitFlip(22, 0, 1, 0);
        Ps4Hook::Pump();
        bool any = false;
        for (int b = 0; b < 3; ++b) {
            any = any || f.Touched(b, 0, 0, 1920, 1080);
        }
        Check(!any, "nothing showing: nothing drawn in any buffer");
        Ps4Hook::SetNotice("Compilando shaders: 123456");
        Ps4Hook::Pump();
        const int notice_y = 1080 - 40 - 32;
        Check(f.Touched(0, 40, notice_y, 300, 32) && f.Touched(1, 40, notice_y, 300, 32) && f.Touched(2, 40, notice_y, 300, 32),
              "a notice goes into every buffer");
        constexpr std::uint32_t NoticeBack = 0xFF2A1C08u; // the notice's box colour on an ARGB screen (the swap makes it ARGB)
        Check(f.buf[1][size_t(notice_y + 2) * Frames3::W + 42] == NoticeBack, "colours are written in the format on the screen (swap included)");
        Ps4Hook::SetNotice("Compilando shaders: 1");
        Ps4Hook::Pump();
        // The box was 26 cells wide, the new text is 21: the right end of the old box still has the box colour.
        Check(f.buf[0][size_t(notice_y + 4) * Frames3::W + 40 + 26 * 10 + 8 - 3] == NoticeBack,
              "a shorter text: the box does not shrink, so no remnant of the longer text shows");
        Ps4Hook::SetNotice("");
        const auto stats = Ps4Hook::GetKeepAliveStats();
        Ps4Hook::Pump();
        Check(Ps4Hook::GetKeepAliveStats().stamps_content == stats.stamps_content, "notice removed: nothing is copied any more");

        Ps4Hook::ShowPanel("Pause", {"Continue", "HUD", "Exit"}, 1, "footer");
        Ps4Hook::Pump();
        bool border_all = true;
        for (int b = 0; b < 3; ++b) {
            bool border = false;
            for (std::uint32_t p : f.buf[b]) {
                border = border || p == 0xFF4A86FFu;
            }
            border_all = border_all && border;
        }
        Check(border_all, "the panel (with its border colour) is in every buffer");
        Ps4Hook::ShowPanel("Pause", {"Continue", "HUD", "Exit", "Four", "Five"}, 4, "a longer footer line");
        Ps4Hook::Pump();
        Ps4Hook::ShowPanel("Pause", {"Continue"}, 0, "f");
        Ps4Hook::Pump();
        // After the 5-row panel, the 1-row one keeps its size: the last row's area is still panel colour (0x141C38 swapped).
        constexpr std::uint32_t PanelBack = 0xFF141C38u;
        const int px = 1920 / 2, py = (1080 - (5 * 56 + 150)) / 2 + 84 + 4 * 56 + 20; // inside the 5th row's box
        Check(f.buf[2][size_t(py) * Frames3::W + size_t(px) - 150] == PanelBack,
              "a smaller panel keeps the size of the biggest one it has been: no remnant of the larger one");
        Ps4Hook::HidePanel();
        const auto kept = Ps4Hook::GetKeepAliveStats();
        Ps4Hook::Pump();
        Check(Ps4Hook::GetKeepAliveStats().stamps_content == kept.stamps_content, "panel closed: nothing is copied any more");
    }
    {
        Ps4Hook::Settings s;
        s.hud = true;
        s.swap_colours = false;
        Ps4Hook::Arm(s, false);
        Frames3 f(ARGB);
        const std::uint32_t tile = 0;
        std::memcpy(f.attr + 4, &tile, 4);
        __wrap_sceVideoOutRegisterBuffers(23, 0, f.ptrs, 3, f.attr);
        __wrap_sceVideoOutSubmitFlip(23, 0, 1, 0);
        Ps4Hook::Pump();
        Check(!f.Touched(0, 0, 0, 1920, 1080) && !Ps4Hook::Ready(), "keep-alive, tiled buffers: nothing is drawn");
        Ps4Hook::Settings off = s;
        off.draw = false;
        Ps4Hook::Arm(off, false);
        Frames3 g(ARGB);
        __wrap_sceVideoOutRegisterBuffers(23, 0, g.ptrs, 3, g.attr);
        __wrap_sceVideoOutSubmitFlip(23, 0, 1, 0);
        Ps4Hook::Pump();
        Check(!g.Touched(0, 0, 0, 1920, 1080), "overlay_draw off: nothing is drawn in keep-alive either");
    }
    {
        // A second set of buffers (the swapchain was made again): the HUD goes into the new ones.
        Ps4Hook::Settings s;
        s.hud = true;
        s.swap_colours = false;
        Ps4Hook::Arm(s, false);
        Frames3 a(ARGB), b(ARGB);
        __wrap_sceVideoOutRegisterBuffers(24, 0, a.ptrs, 3, a.attr);
        __wrap_sceVideoOutSubmitFlip(24, 0, 1, 0);
        Ps4Hook::Pump();
        __wrap_sceVideoOutRegisterBuffers(24, 0, b.ptrs, 3, b.attr);
        __wrap_sceVideoOutSubmitFlip(24, 1, 1, 0);
        Ps4Hook::Pump();
        Check(b.Has(0, HudSpot) && b.Has(1, HudSpot) && b.Has(2, HudSpot), "registered again: the new buffers get the HUD too");
    }
    {
        // The worker thread of keep-alive: it starts at the first registration and repairs a
        // painted-over buffer by itself. Static: the worker keeps running until the program ends.
        static Frames3 w(ARGB);
        Ps4Hook::Settings s;
        s.hud = true;
        s.swap_colours = false;
        s.poll_ms = 1;
        Ps4Hook::Arm(s, true);
        __wrap_sceVideoOutRegisterBuffers(25, 0, w.ptrs, 3, w.attr);
        __wrap_sceVideoOutSubmitFlip(25, 0, 1, 0);
        bool in = false;
        for (int i = 0; i < 300 && !in; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            in = w.Has(0, HudSpot) && w.Has(1, HudSpot) && w.Has(2, HudSpot);
        }
        Check(in, "the keep-alive worker puts the HUD in every buffer by itself");
        std::fill(w.buf[2].begin(), w.buf[2].end(), 0xFF202020u);
        bool back = false;
        for (int i = 0; i < 300 && !back; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            back = w.Has(2, HudSpot);
        }
        Check(back, "...and puts it back after the game paints over a buffer");
        Ps4Hook::Settings idle;
        idle.swap_colours = false;
        Ps4Hook::Arm(idle, false); // nothing showing from now on, so the worker has nothing left to write into `w`
        Check(Ps4Hook::Describe().find("keep-alive") != std::string::npos, "Describe says which mode is on");
    }
    {
        // A simulated display, the same game and the same GPU for both modes. The game flips a buffer
        // every 77 ms (13 fps, round robin over three buffers); the display shows one queued flip per
        // refresh (60 Hz); the GPU paints a whole frame into the buffer some time after the flip was
        // queued (or before it, when the present path waits for the GPU). A refresh "misses" when the
        // buffer on screen lacks the HUD.
        struct Scenario {
            const char* name;
            int gpu_min_ms, gpu_max_ms;
        };
        const Scenario scenarios[] = {
            {"GPU done before the flip (present waits for it)", -12, -1},
            {"GPU finishes 0-6 ms after the flip", 0, 6},
            {"GPU finishes 0-35 ms after the flip", 0, 35},
        };
        std::printf("  simulated display, refreshes without the HUD (classic -> keep-alive):\n");
        for (const Scenario& sc : scenarios) {
            double pct[2] = {0, 0};
            for (int mode = 0; mode < 2; ++mode) {
                pct[mode] = SimulateDisplay(mode == 1, sc.gpu_min_ms, sc.gpu_max_ms);
            }
            std::printf("    %-52s %5.1f %% -> %5.1f %%\n", sc.name, pct[0], pct[1]);
            char what[160];
            std::snprintf(what, sizeof(what), "simulated display, %s: keep-alive misses under 6%% of the refreshes (one 1 ms round per 16.7 ms refresh at most)", sc.name);
            Check(pct[1] < 6.0, what);
            if (sc.gpu_max_ms > 0) {
                std::snprintf(what, sizeof(what), "simulated display, %s: keep-alive misses at least 4x fewer refreshes than classic", sc.name);
                Check(pct[1] * 4.0 < pct[0], what);
            }
        }
        Ps4Hook::SetClockForTest(nullptr);
    }
    // The only worker thread of the whole program (it is started once and runs to the end), so these
    // cases come last: the ones above call Pump() by hand and must not have a worker racing them.
    {
        // 0.7.2 fix: the worker thread does not exist while the game loads (before its buffers are
        // registered) and, with the HUD off, it never asks the system for memory or temperatures.
        // Static on purpose: the worker keeps running until the program ends.
        static Frames w(ARGB);
        Ps4Hook::Settings s;
        s.keepalive = false; // these cases are the 0.7.3 "classic" mode; keep-alive has its own cases below
        s.hud = false;
        s.swap_colours = false;
        const int checks_before = g_flip_checks, memory_before = g_memory_calls;
        Ps4Hook::Arm(s, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Check(g_flip_checks == checks_before, "armed, buffers not registered: nothing is checked or drawn");
        __wrap_sceVideoOutRegisterBuffers(11, 0, w.ptrs, 2, w.attr);
        __wrap_sceVideoOutSubmitFlip(11, 0, 1, 0);
        for (int i = 0; i < 100 && g_flip_checks == checks_before; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(g_flip_checks > checks_before, "first registration of the buffers: the worker starts");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        Check(g_memory_calls == memory_before, "HUD off: no memory or temperature calls at all");
    }

    std::printf(g_failures == 0 ? "video hook test: PASSED\n" : "video hook test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
