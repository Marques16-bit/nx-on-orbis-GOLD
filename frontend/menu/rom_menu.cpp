// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: the menu shown before the emulator starts. See rom_menu.h.
//
// The screens are drawn by Menu::MenuUi into an ordinary memory buffer (the video-out's memory is
// write-combined: reading it back, which blending does, is very slow) and copied to one of two
// linear framebuffers in direct memory, which are flipped with sceVideoOut. Then the video out is
// closed, so that RADV's WSI can open the display for Eden. The framebuffers' 16 MiB are never
// given back: the display may still be scanning the last one out after the close, and freeing
// memory under it is a risk this console answers with a reboot.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/types.h>

#include "menu/rom_menu.h"
#include "ps4_platform.h"

extern "C" {
int32_t sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void* param);
int32_t sceVideoOutClose(int32_t handle);
void sceVideoOutSetBufferAttribute(void* attribute, uint32_t pixel_format, uint32_t tiling,
                                   uint32_t aspect, uint32_t width, uint32_t height,
                                   uint32_t pitch);
int32_t sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses,
                                   int32_t count, const void* attribute);
int32_t sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
int32_t sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int32_t sceVideoOutIsFlipPending(int32_t handle);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int type,
                                      off_t* phys);
int32_t sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, off_t phys,
                                 size_t align);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t len);
size_t sceKernelGetDirectMemorySize(void);
}

namespace {

namespace fs = std::filesystem;

// The menu's own button bits must be the pad's.
static_assert(Menu::Pad::Up == Ps4::Button::Up && Menu::Pad::Down == Ps4::Button::Down &&
              Menu::Pad::Left == Ps4::Button::Left && Menu::Pad::Right == Ps4::Button::Right);
static_assert(Menu::Pad::Cross == Ps4::Button::Cross && Menu::Pad::Circle == Ps4::Button::Circle &&
              Menu::Pad::Triangle == Ps4::Button::Triangle);
static_assert(Menu::Pad::L1 == Ps4::Button::L1 && Menu::Pad::R1 == Ps4::Button::R1 &&
              Menu::Pad::Options == Ps4::Button::Options);

constexpr int Width = 1920;
constexpr int Height = 1080;
constexpr size_t BufferBytes = size_t(Width) * Height * 4;
constexpr size_t Align = 2u << 20;
constexpr size_t TotalBytes = (BufferBytes * 2 + Align - 1) & ~(Align - 1);
constexpr uint32_t PixelFormatA8R8G8B8Srgb = 0x80000000u; // pixels are 0xAARRGGBB
constexpr uint32_t TilingLinear = 1;
constexpr int MemoryWcGarlic = 3;
constexpr int ProtCpuRwGpuRw = 0x33;
constexpr uint32_t FlipVsync = 1;

fs::path SettingsPath() {
    return fs::path{Ps4::DataDir} / "settings.txt";
}

std::string ReadSettings() {
    std::ifstream in{SettingsPath(), std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// Writes settings.txt: the whole file to a temporary name and a rename over it, so a power cut cannot
/// leave half a file; if the rename fails, plainly over it. Then reads it back and opens its
/// permissions to everyone (FTP and file managers run as another user). True when it is there.
bool WriteSettings(const std::string& text) {
    const fs::path path = SettingsPath();
    fs::path temporary = path;
    temporary += ".tmp";
    std::error_code ec;
    bool ok = false;
    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        out << text;
        out.flush();
        ok = static_cast<bool>(out);
    }
    if (ok) {
        fs::rename(temporary, path, ec);
        ok = !ec;
    }
    if (!ok) {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out << text;
        out.flush();
        ok = static_cast<bool>(out);
        fs::remove(temporary, ec);
    }
    if (ok) {
        std::ifstream in{path, std::ios::binary};
        std::ostringstream back;
        back << in.rdbuf();
        ok = back.str() == text;
    }
    if (ok) {
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                                  fs::perms::group_write | fs::perms::others_read | fs::perms::others_write, ec);
    }
    Ps4::Log("menu: settings.txt %s (%zu bytes)", ok ? "saved" : "NOT SAVED", text.size());
    return ok;
}

} // Anonymous namespace

MenuOutcome RunMainMenu(const std::vector<Menu::GameEntry>& games, int initial,
                        const Menu::SystemInfo& info) {
    off_t phys = 0;
    if (int rc = sceKernelAllocateDirectMemory(0, off_t(sceKernelGetDirectMemorySize()),
                                               TotalBytes, Align, MemoryWcGarlic, &phys);
        rc != 0) {
        Ps4::Log("menu: direct memory for the framebuffers refused (0x%x)", unsigned(rc));
        return {};
    }
    void* memory = nullptr;
    if (int rc = sceKernelMapDirectMemory(&memory, TotalBytes, ProtCpuRwGpuRw, 0, phys, Align);
        rc != 0) {
        Ps4::Log("menu: mapping the framebuffers failed (0x%x)", unsigned(rc));
        sceKernelReleaseDirectMemory(phys, TotalBytes);
        return {};
    }
    const int handle = sceVideoOutOpen(255, 0, 0, nullptr);
    if (handle < 0) {
        Ps4::Log("menu: sceVideoOutOpen failed (0x%x)", unsigned(handle));
        munmap(memory, TotalBytes);
        sceKernelReleaseDirectMemory(phys, TotalBytes);
        return {};
    }
    void* buffers[2] = {memory, static_cast<std::uint8_t*>(memory) + BufferBytes};
    alignas(16) std::uint8_t attribute[64] = {};
    sceVideoOutSetBufferAttribute(attribute, PixelFormatA8R8G8B8Srgb, TilingLinear, 0, Width,
                                  Height, Width);
    sceVideoOutSetFlipRate(handle, 0);
    if (int rc = sceVideoOutRegisterBuffers(handle, 0, buffers, 2, attribute); rc < 0) {
        Ps4::Log("menu: sceVideoOutRegisterBuffers failed (0x%x)", unsigned(rc));
        sceVideoOutClose(handle);
        munmap(memory, TotalBytes);
        sceKernelReleaseDirectMemory(phys, TotalBytes);
        return {};
    }
    Ps4::Log("menu: %zu games listed", games.size());

    Menu::MenuUi ui{games, initial, info, ReadSettings(), WriteSettings};
    std::vector<std::uint32_t> scratch(size_t(Width) * Height);
    int back = 0;
    const auto present = [&] {
        Menu::Canvas canvas{scratch.data(), Width, Height};
        ui.Draw(canvas);
        while (sceVideoOutIsFlipPending(handle) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::memcpy(buffers[back], scratch.data(), BufferBytes);
        sceVideoOutSubmitFlip(handle, back, FlipVsync, 0);
        back ^= 1;
    };

    ui.Update(0, Ps4::NowUs()); // the first call: draw the first screen
    present();

    constexpr std::uint32_t Intercepted = 0x80000000u;
    while (ui.Result().kind == Menu::MenuResult::Kind::None) {
        const std::uint32_t now = Ps4::ReadPad().buttons;
        if (now & Intercepted) { // the PS button's own menu is up: the pad is not ours
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        if (ui.Update(now, Ps4::NowUs())) {
            present();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // Leave the display as Eden's WSI expects to find it: closed.
    {
        std::fill(scratch.begin(), scratch.end(), 0xFF000000u);
        while (sceVideoOutIsFlipPending(handle) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::memcpy(buffers[back], scratch.data(), BufferBytes);
        sceVideoOutSubmitFlip(handle, back, FlipVsync, 0);
        while (sceVideoOutIsFlipPending(handle) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const int closed = sceVideoOutClose(handle);

    MenuOutcome outcome;
    if (ui.Result().kind == Menu::MenuResult::Kind::Play) {
        outcome = {MenuChoice::Game, ui.Result().game};
        Ps4::Log("menu: game %d of %zu chosen, display closed (0x%x); framebuffers kept",
                 outcome.game + 1, games.size(), unsigned(closed));
    } else {
        outcome = {MenuChoice::Homebrew, -1};
        Ps4::Log("menu: the Homebrew Menu chosen, display closed (0x%x); framebuffers kept",
                 unsigned(closed));
    }
    return outcome;
}
