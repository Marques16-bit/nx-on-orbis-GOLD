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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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
// The same declarations as Eden's own sink (patch 0002), whose use of them is the one tested on the console.
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void* ptr);
int sceAudioOutClose(int handle);
}

namespace {

namespace fs = std::filesystem;

// The menu's own button bits must be the pad's.
static_assert(Menu::Pad::Up == Ps4::Button::Up && Menu::Pad::Down == Ps4::Button::Down &&
              Menu::Pad::Left == Ps4::Button::Left && Menu::Pad::Right == Ps4::Button::Right);
static_assert(Menu::Pad::Cross == Ps4::Button::Cross && Menu::Pad::Circle == Ps4::Button::Circle &&
              Menu::Pad::Triangle == Ps4::Button::Triangle);
static_assert(Menu::Pad::L2 == Ps4::Button::L2 && Menu::Pad::R2 == Ps4::Button::R2);
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

/// 0.9.5 (zip 5): the menu's music and sounds. One audio port (the same way Eden's own sink opens it: system user, main port, 256-frame
/// grains of 48 kHz S16 stereo), fed by a thread from Menu::Mixer, which makes everything itself. It is closed before the game starts, so
/// Eden's sink finds the port free.
///
/// It could not be tried on a console while it was written, so it is careful:
///  - with menu_audio=off nothing is opened at all;
///  - menu_audio.marker is written before the port is opened and removed after the port has taken its first grain; a marker left over
///    from an earlier run means that run died there, and the menu then stays silent (and says so in the log) until the file is deleted;
///  - the threads own a shared state, so none of them can touch freed memory if one is slow to stop, and none is ever joined (a thread
///    stuck in the driver cannot hold the menu or the game up: closing waits at most 1.5 s).
class MenuAudio {
public:
    MenuAudio() = default;
    MenuAudio(const MenuAudio&) = delete;
    MenuAudio& operator=(const MenuAudio&) = delete;
    ~MenuAudio() { Close(); }

    /// The menu's settings (every step): starts the port the first time sound is wanted, then follows the mode and the volume.
    void Apply(Menu::AudioMode mode, float volume) {
        if (mode != Menu::AudioMode::Off && !tried_) {
            tried_ = true;
            Start();
        }
        if (!state_) {
            return;
        }
        state_->mixer.SetVolume(volume);
        state_->mixer.SetEffects(mode != Menu::AudioMode::Off);
        state_->mixer.SetMusic(mode == Menu::AudioMode::All);
        if (mode == Menu::AudioMode::All && !music_requested_) {
            music_requested_ = true;
            std::shared_ptr<State> state = state_;
            std::thread{[state] {
                const std::uint64_t begin = Ps4::NowUs();
                state->mixer.SetAmbient(Menu::MakeAmbient());
                Ps4::Log("menu audio: the background loop is ready (%llu ms to make)", static_cast<unsigned long long>((Ps4::NowUs() - begin) / 1000));
            }}.detach();
        }
    }

    void Trigger(Menu::SoundId id) {
        if (state_) {
            state_->mixer.Trigger(id);
        }
    }

    bool Running() const { return state_ != nullptr; }

    /// The menu is about to close: lets the game-start chime ring (`chime`), fades everything out and gives the port back.
    void Finish(bool chime) {
        if (!state_) {
            return;
        }
        if (chime) {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }
        state_->mixer.FadeOut(0.3f);
        for (int i = 0; i < 60 && !state_->mixer.Quiet(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Close();
    }

private:
    struct State {
        Menu::Mixer mixer;
        std::atomic<bool> stop{false};
        std::atomic<bool> feeder_done{false};
        fs::path marker;
    };

    void Start() {
        const fs::path marker = fs::path{Ps4::DataDir} / "menu_audio.marker";
        std::error_code ec;
        if (fs::exists(marker, ec)) {
            Ps4::Log("menu audio: SKIPPED. The last run left %s: it did not get past opening the audio port. The menu stays silent; delete that file to try again",
                     marker.filename().string().c_str());
            return;
        }
        if (std::ofstream out{marker}; out) {
            out << "opening the menu audio port; removed once it has taken its first grain\n";
        }
        state_ = std::make_shared<State>();
        state_->marker = marker;
        std::shared_ptr<State> state = state_;
        std::thread{[state] { Feed(state); }}.detach();
    }

    static void Feed(const std::shared_ptr<State>& state) {
        std::error_code ec;
        const int init = sceAudioOutInit();
        if (init != 0 && static_cast<unsigned>(init) != 0x8026000Eu) { // 0x8026000E: already initialised
            Ps4::Log("menu audio: sceAudioOutInit failed (0x%x); no menu sound", static_cast<unsigned>(init));
            fs::remove(state->marker, ec);
            state->feeder_done.store(true);
            return;
        }
        const int handle = sceAudioOutOpen(0xFF /* system user */, 0 /* main port */, 0, GrainFrames, Menu::SampleRate, 1 /* S16 stereo */);
        if (handle < 0) {
            Ps4::Log("menu audio: sceAudioOutOpen failed (0x%x); no menu sound", static_cast<unsigned>(handle));
            fs::remove(state->marker, ec);
            state->feeder_done.store(true);
            return;
        }
        Ps4::Log("menu audio: port %d open (%d Hz, %u frames)", handle, Menu::SampleRate, GrainFrames);
        std::array<std::int16_t, GrainFrames * 2> grain{};
        bool first = true;
        while (!state->stop.load()) {
            state->mixer.Render(grain.data(), int(GrainFrames));
            const int rc = sceAudioOutOutput(handle, grain.data()); // blocks until the port has room: this paces the loop
            if (first) {
                first = false;
                fs::remove(state->marker, ec);
                Ps4::Log("menu audio: the first grain was taken (0x%x)", static_cast<unsigned>(rc));
            }
            if (rc < 0) {
                Ps4::Log("menu audio: sceAudioOutOutput failed (0x%x); the menu sound stops", static_cast<unsigned>(rc));
                break;
            }
        }
        sceAudioOutOutput(handle, nullptr); // waits for the last grain
        sceAudioOutClose(handle);
        Ps4::Log("menu audio: port closed");
        state->feeder_done.store(true);
    }

    void Close() {
        if (!state_) {
            return;
        }
        state_->stop.store(true);
        for (int i = 0; i < 150 && !state_->feeder_done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!state_->feeder_done.load()) {
            Ps4::Log("menu audio: the audio thread did not stop in 1.5 s; the port may stay open (the game may have no sound)");
        }
        state_.reset(); // the threads keep the state alive until they are done
    }

    static constexpr unsigned GrainFrames = 256;
    std::shared_ptr<State> state_;
    bool tried_ = false;
    bool music_requested_ = false;
};

} // Anonymous namespace

MenuOutcome RunMainMenu(std::vector<Menu::GameEntry> games, int initial,
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
    const size_t game_count = games.size(); // `games` is moved into the menu below
    Ps4::Log("menu: %zu games listed", game_count);

    Menu::MenuUi ui{std::move(games), initial, info, ReadSettings(), WriteSettings}; // moved: the covers are not copied
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

    MenuAudio audio;
    const auto service_audio = [&] {
        audio.Apply(ui.Audio(), ui.Volume()); // the volume first, so a tick that came from changing it is heard at the new level
        for (const Menu::SoundId id : ui.TakeSounds()) {
            audio.Trigger(id);
        }
    };

    ui.Update(0, Ps4::NowUs()); // the first call: draw the first screen
    service_audio();
    present();

    constexpr std::uint32_t Intercepted = 0x80000000u;
    while (ui.Result().kind == Menu::MenuResult::Kind::None) {
        const std::uint32_t now = Ps4::ReadPad().buttons;
        if (now & Intercepted) { // the PS button's own menu is up: the pad is not ours
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        if (ui.Tab() == Menu::MenuUi::TabControls) {
            // 0.9.5: the CONTROLS tab shows which controllers are connected. Opening them is cheap (a scan every 2 s at most),
            // and a pad opened here is kept for the game.
            Ps4::OpenExtraPads(4);
            bool connected[4] = {true, false, false, false};
            for (int i = 1; i < 4; ++i) {
                connected[i] = Ps4::ReadPad(i).connected;
            }
            ui.SetPadsConnected(connected);
        }
        const bool redraw = ui.Update(now, Ps4::NowUs());
        service_audio();
        if (redraw) {
            present();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    // The chime of a game starting rings out and the sound fades; the audio port is given back before the display is closed.
    audio.Finish(ui.Result().kind != Menu::MenuResult::Kind::None);

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
                 outcome.game + 1, game_count, unsigned(closed));
    } else {
        outcome = {MenuChoice::Homebrew, -1};
        Ps4::Log("menu: the Homebrew Menu chosen, display closed (0x%x); framebuffers kept",
                 unsigned(closed));
    }
    return outcome;
}
