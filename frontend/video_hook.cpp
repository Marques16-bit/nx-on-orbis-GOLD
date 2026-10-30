// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: a hook on the console's video output. See video_hook.h.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "menu/menu_canvas.h"
#include "video_hook.h"

#include "ps4_platform.h"

extern "C" int32_t sceVideoOutIsFlipPending(int32_t handle);

#ifdef EDEN_PS4_HUD_TEMPERATURE
extern "C" {
// Strong references on purpose: a missing one is a link error that CI sees, not a program that
// fails to load on the console (the build retries without them, see ci/stage-eden.sh).
int sceKernelGetCpuTemperature(int* temperature);
int sceKernelGetSocSensorTemperature(int sensor, int* temperature);
}
#endif

namespace Ps4Hook {

namespace {

constexpr int MaxBuffers = 16;
constexpr std::uint32_t FormatA8R8G8B8 = 0x80000000u; // bytes B G R A: the value reads 0xAARRGGBB
constexpr std::uint32_t FormatA8B8G8R8 = 0x80002200u; // bytes R G B A

struct State {
    std::mutex mutex;
    bool armed = false;
    Settings settings;
    bool hud = false;
    int handle = -1;
    std::uint32_t* address[MaxBuffers] = {};
    int count = 0;
    std::uint32_t width = 0, height = 0, pitch = 0;
    std::uint32_t format_seen = 0, format_used = 0;
    std::uint32_t tiling = 0;
    bool linear = false;
    int last_index = -1;
    std::uint64_t flips = 0;
    // A frame is drawn in twice: once while its flip is still queued (nothing of it is on screen
    // yet, so no flicker) and once more when the flip is over (in case the GPU wrote over it late).
    std::uint64_t hidden_flips = 0, shown_flips = 0;
    std::uint64_t submit_us = 0;       // when the last flip was queued
    std::uint64_t draws_hidden = 0, draws_late = 0;
    int delay_us = 2000;               // the GPU gets this long to finish a frame before it is drawn in
    // The notice in the bottom left corner (the shader counter).
    std::string notice;
    bool notice_dirty = false;
    // The pause menu's panel, drawn by the worker over each new frame while it is up.
    bool panel_active = false, panel_dirty = false;
    std::string panel_title, panel_footer;
    std::vector<std::string> panel_lines;
    int panel_selected = -1;
    std::thread worker;
    std::atomic<bool> stop_worker{false};
    // The worker is not started by Arm(): it starts when the game's buffers are first registered, so
    // nothing of the hook runs while the game loads and the shader cache compiles.
    bool worker_wanted = false, worker_started = false;
    // The HUD's numbers, refreshed once a second.
    double fps = 0, cores = 0;
    long cpu_temp = -1, soc_temp = -1;
    std::uint64_t free_mib = 0;
    double window_start = 0, cpu_start = 0;
    std::uint64_t window_flips = 0;
};

State& S() {
    static State state;
    return state;
}

double Now(clockid_t clock) {
    timespec ts{};
    clock_gettime(clock, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

std::uint64_t NowMicros() {
    return std::uint64_t(Now(CLOCK_MONOTONIC) * 1e6);
}

/// A pixel in the format on the screen: `rgb` is 0x00RRGGBB.
std::uint32_t Pixel(const State& s, std::uint32_t rgb) {
    if (s.format_used == FormatA8B8G8R8) {
        rgb = (rgb & 0x0000FF00u) | ((rgb & 0xFF) << 16) | ((rgb >> 16) & 0xFF);
    }
    return 0xFF000000u | rgb;
}

std::uint32_t Mix(std::uint32_t bg, std::uint32_t fg, unsigned coverage) {
    std::uint32_t out = 0;
    for (int shift : {16, 8, 0}) {
        const unsigned b = (bg >> shift) & 0xFF, f = (fg >> shift) & 0xFF;
        out |= ((b * (255 - coverage) + f * coverage + 127) / 255) << shift;
    }
    return out;
}

void FillRect(const State& s, int buffer, int x, int y, int w, int h, std::uint32_t rgb) {
    std::uint32_t* base = s.address[buffer];
    const std::uint32_t pixel = Pixel(s, rgb);
    for (int j = std::max(y, 0); j < std::min<int>(y + h, int(s.height)); ++j) {
        for (int i = std::max(x, 0); i < std::min<int>(x + w, int(s.width)); ++i) {
            base[size_t(j) * s.pitch + size_t(i)] = pixel;
        }
    }
}

/// Text on a flat background colour (so nothing has to be read back); `half` draws it at half size.
int DrawText(const State& s, int buffer, int x, int y, std::string_view text, std::uint32_t fg,
             std::uint32_t bg, bool half) {
    std::uint32_t* base = s.address[buffer];
    const int cell_w = half ? Menu::GlyphWidth / 2 : Menu::GlyphWidth;
    const int cell_h = half ? Menu::GlyphHeight / 2 : Menu::GlyphHeight;
    int pen = x;
    for (size_t i = 0; i < text.size();) {
        const std::uint8_t* glyph = Menu::GlyphFor(Menu::NextCodePoint(text, i));
        for (int row = 0; row < cell_h; ++row) {
            const int py = y + row;
            if (py < 0 || py >= int(s.height)) {
                continue;
            }
            for (int col = 0; col < cell_w; ++col) {
                const int px = pen + col;
                if (px < 0 || px >= int(s.width)) {
                    continue;
                }
                unsigned coverage;
                if (half) {
                    const int g = row * 2 * Menu::GlyphWidth + col * 2;
                    coverage = (glyph[g] + glyph[g + 1] + glyph[g + Menu::GlyphWidth] + glyph[g + Menu::GlyphWidth + 1]) / 4;
                } else {
                    coverage = glyph[row * Menu::GlyphWidth + col];
                }
                base[size_t(py) * s.pitch + size_t(px)] = Pixel(s, Mix(bg, fg, coverage));
            }
        }
        pen += cell_w;
    }
    return pen - x;
}

void ReadTemperatures(State& s) {
#ifdef EDEN_PS4_HUD_TEMPERATURE
    int t = 0;
    s.cpu_temp = sceKernelGetCpuTemperature(&t) == 0 ? t : -1;
    s.soc_temp = sceKernelGetSocSensorTemperature(0, &t) == 0 ? t : -1;
#else
    s.cpu_temp = s.soc_temp = -1;
#endif
}

/// Once a second: the HUD's numbers. The calls to the system happen without the lock, on the worker's
/// thread, so the thread that presents frames never waits for them.
void RefreshStats() {
    State& s = S();
    static double window_start = 0, cpu_start = 0;
    static std::uint64_t window_flips = 0;
    std::uint64_t flips;
    bool hud;
    {
        std::lock_guard lock{s.mutex};
        flips = s.flips;
        hud = s.hud;
    }
    if (!hud) {
        // HUD off: no call to the system at all (clocks, temperatures, memory). The window starts
        // again when the HUD is turned on.
        window_start = 0;
        return;
    }
    const double wall = Now(CLOCK_MONOTONIC);
    const double cpu = Now(CLOCK_PROCESS_CPUTIME_ID);
    if (window_start == 0) {
        window_start = wall;
        cpu_start = cpu;
        window_flips = flips;
        return;
    }
    if (wall - window_start < 1.0) {
        return;
    }
    const double fps = double(flips - window_flips) / (wall - window_start);
    const double cores = (cpu - cpu_start) / (wall - window_start);
    window_start = wall;
    cpu_start = cpu;
    window_flips = flips;
    State scratch;
    ReadTemperatures(scratch);
    const std::uint64_t free_mib = Ps4::FreeDirectMemory() >> 20;
    std::lock_guard lock{s.mutex};
    s.fps = fps;
    s.cores = cores;
    s.cpu_temp = scratch.cpu_temp;
    s.soc_temp = scratch.soc_temp;
    s.free_mib = free_mib;
}

void DrawHud(State& s, int buffer) {
    char line[4][48];
    std::snprintf(line[0], sizeof(line[0]), "FPS %.1f  (%.0f ms)", s.fps, s.fps > 0 ? 1000.0 / s.fps : 0.0);
    std::snprintf(line[1], sizeof(line[1]), "CPU %.1f nucleos de 8", s.cores);
    std::snprintf(line[2], sizeof(line[2]), "MEM livre %llu MiB", static_cast<unsigned long long>(s.free_mib));
    if (s.cpu_temp >= 0 || s.soc_temp >= 0) {
        std::snprintf(line[3], sizeof(line[3]), "TEMP CPU %ld  SoC %ld C", s.cpu_temp, s.soc_temp);
    } else {
        std::snprintf(line[3], sizeof(line[3]), "TEMP --");
    }
    const bool half = s.settings.hud_small;
    const int cell_w = half ? Menu::GlyphWidth / 2 : Menu::GlyphWidth;
    const int cell_h = half ? Menu::GlyphHeight / 2 : Menu::GlyphHeight;
    const int box_w = 24 * cell_w + 16, box_h = 4 * cell_h + 12;
    const int x = s.settings.hud_right ? int(s.width) - box_w - 40 : 40;
    const int y = 40;
    constexpr std::uint32_t Back = 0x101830, Fore = 0xE8F0FF;
    FillRect(s, buffer, x, y, box_w, box_h, Back);
    for (int i = 0; i < 4; ++i) {
        DrawText(s, buffer, x + 8, y + 6 + i * cell_h, line[i], i == 3 && s.cpu_temp > 80 ? 0xFF6060 : Fore, Back, half);
    }
}

/// The shader counter: small text on a dark strip, bottom left, clear of the HUD at the top.
void DrawNotice(State& s, int buffer) {
    if (s.notice.empty()) {
        return;
    }
    const int cell_w = Menu::GlyphWidth / 2, cell_h = Menu::GlyphHeight / 2;
    const int box_w = Menu::TextCells(s.notice) * cell_w + 16, box_h = cell_h + 12;
    const int x = 40, y = int(s.height) - 40 - box_h;
    constexpr std::uint32_t Back = 0x2A1C08, Fore = 0xFFD27A;
    FillRect(s, buffer, x, y, box_w, box_h, Back);
    DrawText(s, buffer, x + 8, y + 6, s.notice, Fore, Back, true);
}

void DrawPanel(State& s, int b) {
    const std::string& title = s.panel_title;
    const std::vector<std::string>& lines = s.panel_lines;
    const int selected = s.panel_selected;
    const std::string& footer = s.panel_footer;
    int cells = Menu::TextCells(title);
    for (const std::string& l : lines) {
        cells = std::max(cells, Menu::TextCells(l) + 4);
    }
    cells = std::max(cells, Menu::TextCells(footer));
    const int w = std::min<int>(cells * Menu::GlyphWidth + 80, int(s.width) - 80);
    const int h = int(lines.size()) * 56 + 150;
    const int x = (int(s.width) - w) / 2, y = (int(s.height) - h) / 2;
    constexpr std::uint32_t Back = 0x141C38, Edge = 0x4A86FF, Sel = 0x2B4FA6, Text = 0xEAF0FF, Dim = 0x8FA0C8;
    FillRect(s, b, x - 4, y - 4, w + 8, h + 8, Edge);
    FillRect(s, b, x, y, w, h, Back);
    DrawText(s, b, x + 40, y + 20, title, 0x2DD4C4, Back, false);
    for (size_t i = 0; i < lines.size(); ++i) {
        const int ly = y + 84 + int(i) * 56;
        const bool on = int(i) == selected;
        FillRect(s, b, x + 20, ly - 4, w - 40, 52, on ? Sel : Back);
        DrawText(s, b, x + 40, ly, lines[i], on ? 0xFFFFFF : Text, on ? Sel : Back, false);
    }
    DrawText(s, b, x + 40, y + h - 52, footer, Dim, Back, false);
}

} // Anonymous namespace

Settings& Pending() {
    static Settings settings;
    return settings;
}

void Pump() {
    State& s = S();
    RefreshStats();
    int index;
    std::int32_t handle;
    bool hidden_pass, late_pass, panel, notice;
    std::uint64_t submit_us, delay_us;
    {
        std::lock_guard lock{s.mutex};
        if (!s.armed || !s.settings.draw || !s.linear || s.handle < 0 || s.last_index < 0 ||
            s.address[s.last_index] == nullptr) {
            return;
        }
        index = s.last_index;
        handle = s.handle;
        submit_us = s.submit_us;
        delay_us = std::uint64_t(std::max(s.delay_us, 0));
        hidden_pass = s.flips != s.hidden_flips;                      // a new frame, not drawn in yet
        late_pass = !hidden_pass && s.flips != s.shown_flips;         // drawn in while queued: check again once shown
        panel = s.panel_active && s.panel_dirty;
        notice = s.notice_dirty;
        if (!hidden_pass && !late_pass && !panel && !notice) {
            return;
        }
    }
    const bool pending = sceVideoOutIsFlipPending(handle) > 0;
    if (hidden_pass && pending && NowMicros() - submit_us < delay_us) {
        return; // the GPU may still be writing this frame: give it a moment (it is not on screen yet)
    }
    if (late_pass && pending) {
        return; // the queued frame is still hidden and already has its HUD: wait for the flip to finish
    }
    std::lock_guard lock{s.mutex};
    if (index != s.last_index) {
        return; // another flip was queued while the lock was free: the next round draws that one
    }
    const bool draw_all = hidden_pass || late_pass;
    if (draw_all && s.pitch != 0) {
        if (s.hud) {
            DrawHud(s, index);
        }
        DrawNotice(s, index);
        if (s.panel_active) {
            DrawPanel(s, index); // a frame the game flips while the menu is up gets the panel too
            s.panel_dirty = false;
        }
        if (hidden_pass) {
            ++(pending ? s.draws_hidden : s.draws_late);
            s.hidden_flips = s.flips;
            if (!pending) {
                s.shown_flips = s.flips; // already on screen: nothing left to check
            }
        } else {
            ++s.draws_late;
            s.shown_flips = s.flips;
        }
        s.notice_dirty = false;
        return;
    }
    if (s.pitch != 0) {
        if (panel) {
            DrawPanel(s, index);
            s.panel_dirty = false;
        }
        if (notice) {
            DrawNotice(s, index);
            s.notice_dirty = false;
        }
    }
}

void Arm(const Settings& settings, bool start_worker) {
    State& s = S();
    {
        std::lock_guard lock{s.mutex};
        s.armed = true;
        s.settings = settings;
        s.hud = settings.hud;
        s.count = 0;
        s.handle = -1;
        s.last_index = -1;
        s.panel_active = false;
        s.hidden_flips = s.shown_flips = s.flips;
        s.draws_hidden = s.draws_late = 0;
        s.delay_us = std::max(0, settings.draw_delay_ms) * 1000;
        s.notice.clear();
        s.notice_dirty = false;
        s.worker_wanted = start_worker && settings.draw;
    }
}

bool Ready() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    return s.armed && s.settings.draw && s.linear && s.handle >= 0 && s.count > 0 && s.last_index >= 0;
}

void SetHud(bool on) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    s.hud = on;
}

bool HudOn() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    return s.hud;
}

void ShowPanel(const std::string& title, const std::vector<std::string>& lines, int selected,
               const std::string& footer) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    s.panel_active = true;
    s.panel_dirty = true;
    s.panel_title = title;
    s.panel_lines = lines;
    s.panel_selected = selected;
    s.panel_footer = footer;
}

void HidePanel() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    s.panel_active = false;
}

void SetNotice(const std::string& text) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    if (s.notice != text) {
        s.notice = text;
        s.notice_dirty = true;
    }
}

void SetDrawDelayMicros(int micros) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    s.delay_us = std::max(0, micros);
}

void DrawStats(std::uint64_t* hidden, std::uint64_t* late) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    if (hidden != nullptr) {
        *hidden = s.draws_hidden;
    }
    if (late != nullptr) {
        *late = s.draws_late;
    }
}

std::string Describe() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    char text[260];
    std::snprintf(text, sizeof(text), "video hook: armed=%d handle=%d buffers=%d %ux%u pitch %u tiling %u (%s), format seen 0x%08x used 0x%08x, %llu flips, drawn %llu hidden / %llu late",
                  s.armed, s.handle, s.count, s.width, s.height, s.pitch, s.tiling, s.linear ? "linear: drawing on" : "NOT linear: drawing off", s.format_seen, s.format_used,
                  static_cast<unsigned long long>(s.flips), static_cast<unsigned long long>(s.draws_hidden),
                  static_cast<unsigned long long>(s.draws_late));
    return text;
}

} // namespace Ps4Hook

extern "C" int32_t __wrap_sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses,
                                                     int32_t count, const void* attribute) {
    using namespace Ps4Hook;
    State& s = S();
    alignas(8) std::uint8_t attr[64];
    const void* use = attribute;
    char note[200] = {};
    bool start_worker = false;
    {
        std::lock_guard lock{s.mutex};
        if (s.armed && attribute != nullptr && addresses != nullptr && count > 0) {
            std::memcpy(attr, attribute, sizeof(attr));
            std::uint32_t format = 0, tiling = 0;
            std::memcpy(&format, attr, 4);
            std::memcpy(&tiling, attr + 4, 4);
            const bool again = s.handle == handle && s.count > 0;
            s.handle = handle;
            s.format_seen = format;
            s.tiling = tiling;
            s.linear = tiling == 1; // SCE_VIDEO_OUT_TILING_MODE_LINEAR
            if (s.settings.swap_colours && (format == FormatA8R8G8B8 || format == FormatA8B8G8R8)) {
                format = format == FormatA8R8G8B8 ? FormatA8B8G8R8 : FormatA8R8G8B8;
                std::memcpy(attr, &format, 4);
                use = attr;
            }
            s.format_used = format;
            std::memcpy(&s.width, attr + 12, 4);
            std::memcpy(&s.height, attr + 16, 4);
            std::memcpy(&s.pitch, attr + 20, 4);
            for (int i = 0; i < count && start + i < MaxBuffers; ++i) {
                s.address[start + i] = static_cast<std::uint32_t*>(addresses[i]);
            }
            s.count = std::max(s.count, start + count);
            s.last_index = -1; // a new set of buffers: nothing is on screen from it yet
            std::snprintf(note, sizeof(note), "video hook: %s %d buffers at index %d, %ux%u pitch %u, tiling %u (%s), format 0x%08x -> 0x%08x",
                          again ? "registered AGAIN:" : "registered:", count, start, s.width, s.height, s.pitch, tiling,
                          s.linear ? "linear" : "NOT linear, so no drawing over the game", s.format_seen, s.format_used);
            if (s.worker_wanted && !s.worker_started) {
                s.worker_started = true;
                start_worker = true;
            }
        }
    }
    if (note[0] != 0) {
        Ps4::Log("%s", note);
    }
    if (start_worker) {
        // First registration of the game's buffers: only now does the worker thread exist. If the
        // thread cannot be made the game still runs, just without the HUD and the menu.
        try {
            s.stop_worker = false;
            s.worker = std::thread([] {
                while (!S().stop_worker) {
                    Pump();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });
            s.worker.detach();
            Ps4::Log("video hook: worker started");
        } catch (...) {
            Ps4::Log("video hook: the worker thread could not be started (no HUD, no pause menu)");
        }
    }
    return __real_sceVideoOutRegisterBuffers(handle, start, addresses, count, use);
}

extern "C" int32_t __wrap_sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg) {
    using namespace Ps4Hook;
    State& s = S();
    {
        // Only a note of which buffer went on screen: nothing here may slow the thread that presents.
        std::lock_guard lock{s.mutex};
        if (s.armed && handle == s.handle && index >= 0 && index < s.count && s.address[index] != nullptr) {
            s.last_index = index;
            ++s.flips;
            s.submit_us = NowMicros();
        }
    }
    return __real_sceVideoOutSubmitFlip(handle, index, mode, arg);
}
