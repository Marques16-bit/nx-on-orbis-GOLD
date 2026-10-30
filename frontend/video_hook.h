// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: a hook on the console's video output, to draw over the game without touching Eden.
//
// RADV's WSI (inside the executable) registers its swapchain images as the video-out buffers and
// submits a flip for each frame. The linker's --wrap redirects those two calls here (see
// frontend.cmake). With the buffers' addresses known, this file can:
//   - turn the registered pixel format into its red/blue mirror, which swaps red and blue on the
//     screen (the "colour_swap" option);
//   - draw a performance HUD into each frame, in the short window after it was queued for the screen
//     and before it is shown (a draw made after the frame is on screen flickers: the part already
//     scanned out has no HUD, and the next frame is a different buffer);
//   - draw a panel (the in-game menu) into the frame on the screen while the game is paused;
//   - draw a small notice (the shader compile counter, like Cemu's) in the bottom left corner.
// Drawing only writes (the buffers are write-combined memory: reading them back is very slow).

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
    int draw_delay_ms = 2;      // wait after a flip is queued before drawing into it (the GPU may still write)
};

/// The settings read from settings.txt (hud, hud_size, hud_position, colour_swap), until Arm().
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
/// How long to wait after a flip was queued before drawing into its buffer (host tests use 0).
void SetDrawDelayMicros(int micros);
/// Frames on which the HUD, panel or notice were drawn before the frame was on screen / after it.
void DrawStats(std::uint64_t* hidden, std::uint64_t* late);

} // namespace Ps4Hook

extern "C" {
int32_t __real_sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses,
                                          int32_t count, const void* attribute);
int32_t __real_sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
int32_t __wrap_sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses,
                                          int32_t count, const void* attribute);
int32_t __wrap_sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
}
