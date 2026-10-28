// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: a hook on the console's video output, to draw over the game without touching Eden.
//
// RADV's WSI (inside the executable) registers its swapchain images as the video-out buffers and
// submits a flip for each frame. The linker's --wrap redirects those two calls here (see
// frontend.cmake). With the buffers' addresses known, this file can:
//   - turn the registered pixel format into its red/blue mirror, which swaps red and blue on the
//     screen (the "colour_swap" option);
//   - draw a performance HUD into the frame that is about to be flipped;
//   - draw a panel (the in-game menu) into the frame on the screen while the game is paused.
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
};

/// The settings read from settings.txt (hud, hud_size, hud_position, colour_swap), until Arm().
Settings& Pending();
/// From now on the next buffers registered are the game's. Call before the game's video output opens.
void Arm(const Settings& settings);
/// The game's buffers are known, so drawing over it works.
bool Ready();
void SetHud(bool on);
bool HudOn();
/// A panel over the middle of the screen: a title, lines (one highlighted, -1 for none) and a footer.
void ShowPanel(const std::string& title, const std::vector<std::string>& lines, int selected,
               const std::string& footer);
/// One line for the log: what the hook saw (format, size, how many buffers, flips).
std::string Describe();

} // namespace Ps4Hook

extern "C" {
int32_t __real_sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses,
                                          int32_t count, const void* attribute);
int32_t __real_sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
int32_t __wrap_sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses,
                                          int32_t count, const void* attribute);
int32_t __wrap_sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
}
