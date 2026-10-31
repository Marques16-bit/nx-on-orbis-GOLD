// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: a hook on the console's video output, to draw over the game without touching Eden.
//
// RADV's WSI (inside the executable) registers its swapchain images as the video-out buffers and
// submits a flip for each frame. The linker's --wrap redirects those two calls here (see
// frontend.cmake). With the buffers' addresses known, this file can:
//   - turn the registered pixel format into its red/blue mirror, which swaps red and blue on the
//     screen (the "colour_swap" option);
//   - draw a performance HUD, a panel (the in-game menu while the game is paused) and a small notice
//     (the shader compile counter, memory warnings) over the game;
// Drawing only writes (the buffers are write-combined memory: reading them back is very slow; the
// only reads are a few probe pixels).
//
// Why the overlay used to flicker (0.7.0 - 0.7.5): the game's GPU rewrites a whole video-out buffer
// for every frame, and the overlay was written into it only once or twice around the flip. When the
// GPU finished after that, the frame was shown without the overlay for one screen refresh, and the
// next frame (another buffer) did it again: a flicker at the game's frame rate.
//
// 0.7.6 "keep-alive" drawing: the overlay is built once as a small bitmap in ordinary memory, and the
// worker thread keeps it in EVERY video-out buffer: it looks at a few probe pixels of each buffer
// (one read each) and copies the bitmap again the moment the game has painted over it. The frame is
// also stamped inside the wrapped flip call, so a frame whose GPU work is already done never shows
// without it. Elements are copied row by row with their final pixels (no "clear, then draw" step
// that a screen refresh could catch half way). The 0.7.3 behaviour stays available as the
// "classic" mode (overlay_mode=classic).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Ps4Hook {

struct Settings {
    bool hud = false;
    bool hud_small = false;
    bool hud_right = false;
    bool swap_colours = true;
    bool overlay = true;        // arm the hook at all (the HUD, the pause menu and the colour swap need it)
    bool draw = true;           // draw the HUD and the pause menu (off: only the colour swap stays)
    int shader_preload = 256;   // size class of the disk cache loaded at start: 0 none, -1 all (see main.cpp)
    bool shader_notice = true;  // the shader compile counter over the game
    int draw_delay_ms = 2;      // classic mode: wait after a flip is queued before drawing into it
    bool keepalive = true;      // 0.7.6: keep the overlay alive in every buffer (false: the 0.7.3 "classic" mode)
    bool flip_stamp = true;     // keep-alive: also stamp the frame inside the wrapped flip call
    int poll_ms = 1;            // keep-alive: how often the worker checks the buffers, in milliseconds (1 to 8)
};

/// The settings read from settings.txt (hud, hud_size, hud_position, colour_swap, ...), until Arm().
Settings& Pending();
/// From now on the next buffers registered are the game's. Call before the game's video output opens.
void Arm(const Settings& settings, bool start_worker = true);
/// One step of what the worker thread does (the host test calls it by hand).
void Pump();
/// The game's buffers are known and linear, so drawing over it works.
bool Ready();
void SetHud(bool on);
bool HudOn();
/// A panel over the middle of the screen: a title, lines (one highlighted, -1 for none) and a footer.
void ShowPanel(const std::string& title, const std::vector<std::string>& lines, int selected,
               const std::string& footer);
/// One line for the log: what the hook saw (format, size, how many buffers, flips).
std::string Describe();
/// The pause menu is closed: stop drawing its panel.
void HidePanel();
/// The notice in the bottom left corner (the shader counter). An empty text removes it.
void SetNotice(const std::string& text);
/// How long to wait after a flip was queued before drawing into its buffer (classic mode; host tests use 0).
void SetDrawDelayMicros(int micros);
/// Classic mode: frames on which the HUD, panel or notice were drawn before the frame was on screen / after it.
void DrawStats(std::uint64_t* hidden, std::uint64_t* late);

/// Keep-alive mode counters, for boot.log and the host tests.
struct KeepAliveStats {
    std::uint64_t stamps_flip = 0;    // copied inside the flip call (the frame was queued without it)
    std::uint64_t stamps_repair = 0;  // the worker found the overlay painted over by the game and put it back
    std::uint64_t stamps_content = 0; // copied because the overlay itself changed (HUD numbers, panel, notice)
    std::uint64_t probe_reads = 0;    // single-pixel reads of video memory
};
KeepAliveStats GetKeepAliveStats();

/// Test seam: replaces the clock (microseconds); nullptr puts the real one back.
void SetClockForTest(std::uint64_t (*now_micros)());

} // namespace Ps4Hook

extern "C" {
int32_t __real_sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses,
                                          int32_t count, const void* attribute);
int32_t __real_sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
int32_t __wrap_sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses,
                                          int32_t count, const void* attribute);
int32_t __wrap_sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
}
