// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: a hook on the console's video output. See video_hook.h.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>

#if defined(__SSE__)
#include <xmmintrin.h>
#include <emmintrin.h>
#endif

#include "menu/menu_canvas.h"
#include "screenshot.h"
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

/// What is drawn over the game. Each one is a small bitmap kept in ordinary memory.
enum Element_ : int { ElHud = 0, ElNotice = 1, ElPanel = 2, ElCount = 3 };

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Element {
    Rect rect;
    std::vector<std::uint32_t> px;  // rect.w * rect.h, in the pixel format of the screen
    bool built = false;             // px shows what the element should show now
    std::uint64_t version = 0;      // grows with every rebuild
};

/// Where the text and rectangles of an element are drawn: a bitmap whose top left corner is at
/// (ox, oy) on the screen. Drawing code works in screen coordinates.
struct Surface {
    std::uint32_t* base;
    int pitch, w, h, ox, oy;
};

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
    // Classic mode: a frame is drawn in twice: once while its flip is still queued (nothing of it is
    // on screen yet, so no flicker) and once more when the flip is over (in case the GPU wrote over it late).
    std::uint64_t hidden_flips = 0, shown_flips = 0;
    std::uint64_t submit_us = 0;       // when the last flip was queued
    Timeline tl;                       // 0.7.9: flip intervals since the last TakeTimeline()
    std::uint64_t tl_prev_us = 0;      // when the flip before was queued
    Stall stall;                       // 0.8.0: the last freeze of the picture (StallGapUs or longer)
    std::uint64_t draws_hidden = 0, draws_late = 0;
    int delay_us = 2000;               // classic: the GPU gets this long to finish a frame before it is drawn in
    // The notice in the bottom left corner (the shader counter, memory warnings).
    std::string notice;
    bool notice_dirty = false;
    int notice_cells = 0;              // widest the notice has been: the box never shrinks, so no remnant is left
    // The pause menu's panel, drawn by the worker over each new frame while it is up.
    bool panel_active = false, panel_dirty = false;
    std::string panel_title, panel_footer;
    std::vector<std::string> panel_lines;
    int panel_selected = -1;
    int panel_cells = 0, panel_rows = 0; // widest / tallest while this panel has been up
    // Keep-alive mode.
    Element element[ElCount];
    std::uint64_t stamped[MaxBuffers][ElCount] = {}; // the version of each element last copied into each buffer
    KeepAliveStats ka;
    std::thread worker;
    std::atomic<bool> stop_worker{false};
    // The worker is not started by Arm(): it starts when the game's buffers are first registered, so
    // nothing of the hook runs while the game loads and the shader cache compiles.
    bool worker_wanted = false, worker_started = false;
    // The HUD's numbers, refreshed once a second.
    double fps = 0, cores = 0;
    long cpu_temp = -1, soc_temp = -1;
    std::uint64_t free_mib = 0;
    // 0.9.5 zip 5: the HUD's graph (one value per second, oldest first) and the longest flip gap of the last second.
    std::vector<float> fps_history;
    double worst_ms = 0;
    std::uint64_t gap_max_window_us = 0;
    bool stats_frozen = false;         // the host test set the numbers by hand
    double window_start = 0, cpu_start = 0;
    std::uint64_t window_flips = 0;
    // 0.7.7 fix: the buffer that was just flipped is the one on screen; it is written again for a while.
    std::uint64_t looked_flip = ~std::uint64_t{0}, restamp_us = 0;
};

State& S() {
    static State state;
    return state;
}

std::uint64_t (*g_clock)() = nullptr;

double Now(clockid_t clock) {
    timespec ts{};
    clock_gettime(clock, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

std::uint64_t NowMicros() {
    if (g_clock != nullptr) {
        return g_clock();
    }
    return std::uint64_t(Now(CLOCK_MONOTONIC) * 1e6);
}

/// Pushes the write-combining buffers out, so what was just copied reaches the screen's memory now
/// and not whenever the CPU gets round to it (the last lines of a copy could sit there a while).
inline void FlushWrites() {
#if defined(__SSE__)
    _mm_sfence();
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
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

void FillRect(const State& s, Surface& sf, int x, int y, int w, int h, std::uint32_t rgb) {
    const std::uint32_t pixel = Pixel(s, rgb);
    for (int j = std::max(y - sf.oy, 0); j < std::min(y - sf.oy + h, sf.h); ++j) {
        for (int i = std::max(x - sf.ox, 0); i < std::min(x - sf.ox + w, sf.w); ++i) {
            sf.base[size_t(j) * size_t(sf.pitch) + size_t(i)] = pixel;
        }
    }
}

/// A colour (0x00RRGGBB) read back from a pixel of the screen format.
std::uint32_t Rgb(const State& s, std::uint32_t pixel) {
    if (s.format_used == FormatA8B8G8R8) {
        pixel = (pixel & 0x0000FF00u) | ((pixel & 0xFF) << 16) | ((pixel >> 16) & 0xFF);
    }
    return pixel & 0x00FFFFFFu;
}

/// As the background of DrawText: draw over what is already in the bitmap (a gradient, a bar).
constexpr std::uint32_t OverBackground = 0xFF000000u;

/// Text on a flat background colour (or OverBackground); `half` draws it at half size.
int DrawText(const State& s, Surface& sf, int x, int y, std::string_view text, std::uint32_t fg,
             std::uint32_t bg, bool half) {
    const int cell_w = half ? Menu::GlyphWidth / 2 : Menu::GlyphWidth;
    const int cell_h = half ? Menu::GlyphHeight / 2 : Menu::GlyphHeight;
    int pen = x;
    for (size_t i = 0; i < text.size();) {
        const std::uint8_t* glyph = Menu::GlyphFor(Menu::NextCodePoint(text, i));
        for (int row = 0; row < cell_h; ++row) {
            const int py = y + row - sf.oy;
            if (py < 0 || py >= sf.h) {
                continue;
            }
            for (int col = 0; col < cell_w; ++col) {
                const int px = pen + col - sf.ox;
                if (px < 0 || px >= sf.w) {
                    continue;
                }
                unsigned coverage;
                if (half) {
                    const int g = row * 2 * Menu::GlyphWidth + col * 2;
                    coverage = (glyph[g] + glyph[g + 1] + glyph[g + Menu::GlyphWidth] + glyph[g + Menu::GlyphWidth + 1]) / 4;
                } else {
                    coverage = glyph[row * Menu::GlyphWidth + col];
                }
                std::uint32_t& dst = sf.base[size_t(py) * size_t(sf.pitch) + size_t(px)];
                if ((bg & OverBackground) != 0) {
                    if (coverage != 0) {
                        dst = Pixel(s, Mix(Rgb(s, dst), fg, coverage));
                    }
                } else {
                    dst = Pixel(s, Mix(bg, fg, coverage));
                }
            }
        }
        pen += cell_w;
    }
    return pen - x;
}

/// The text cut or padded with spaces to exactly `cells` characters, so a redraw covers what an earlier,
/// longer text left behind.
std::string FitCells(const std::string& text, int cells) {
    std::string out;
    int count = 0;
    for (size_t i = 0; i < text.size() && count < cells;) {
        const size_t start = i;
        Menu::NextCodePoint(text, i);
        out.append(text, start, i - start);
        ++count;
    }
    out.append(size_t(std::max(cells - count, 0)), ' ');
    return out;
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
        hud = s.hud && !s.stats_frozen;
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
    s.worst_ms = double(s.gap_max_window_us) / 1000.0;
    s.gap_max_window_us = 0;
    s.fps_history.push_back(float(fps));
    if (s.fps_history.size() > 60) {
        s.fps_history.erase(s.fps_history.begin(), s.fps_history.end() - 60);
    }
    s.element[ElHud].built = false; // new numbers: the bitmap is made again
}

// ---------------------------------------------------------------- the elements

std::uint32_t Lerp(std::uint32_t a, std::uint32_t b, int t, int n) {
    std::uint32_t out = 0;
    for (int shift : {16, 8, 0}) {
        const int x = int((a >> shift) & 0xFF), y = int((b >> shift) & 0xFF);
        out |= std::uint32_t(x + (y - x) * t / std::max(n, 1)) << shift;
    }
    return out;
}

/// A rectangle that goes from colour `a` (left) to `b` (right).
void FillGradientH(const State& s, Surface& sf, int x, int y, int w, int h, std::uint32_t a, std::uint32_t b) {
    for (int i = 0; i < w; ++i) {
        FillRect(s, sf, x + i, y, 1, h, Lerp(a, b, i, std::max(w - 1, 1)));
    }
}

/// A rectangle that goes from colour `a` (top) to `b` (bottom).
void FillGradientV(const State& s, Surface& sf, int x, int y, int w, int h, std::uint32_t a, std::uint32_t b) {
    for (int j = 0; j < h; ++j) {
        FillRect(s, sf, x, y + j, w, 1, Lerp(a, b, j, std::max(h - 1, 1)));
    }
}

/// Width in pixels of text drawn at full or half size.
int TextPixels(std::string_view text, bool half) {
    return Menu::TextCells(std::string(text)) * (half ? Menu::GlyphWidth / 2 : Menu::GlyphWidth);
}

constexpr std::uint32_t HudGood = 0x4CD964, HudWarn = 0xFFC83D, HudBad = 0xFF5A52;

/// The colour of a rate against what the game aims at (30 or 60).
std::uint32_t RateColour(double fps, double target) {
    const double ratio = target > 0 ? fps / target : 1.0;
    return ratio >= 0.92 ? HudGood : ratio >= 0.6 ? HudWarn : HudBad;
}

/// What the game aims at, from the best second seen: 60 for a game that ever gets near it, else 30, else 20.
double TargetRate(const std::vector<float>& history, double now) {
    double peak = now;
    for (float v : history) {
        peak = std::max(peak, double(v));
    }
    return peak > 40 ? 60.0 : peak > 20 ? 30.0 : 20.0;
}

/// The HUD (0.9.5 zip 5): the rate in a colour that says whether the game holds its target, the time of a frame
/// and the worst gap of the last second, a graph of the last minute, the load of the cores, the memory left and
/// the temperatures. 400 x 200 pixels, or 300 x 124 in the small size.
void BuildHud(State& s) {
    const bool small = s.settings.hud_small;
    const bool pt = s.settings.portuguese;
    const int th = Menu::GlyphHeight / 2; // the small text is 10 x 20
    const int box_w = small ? 300 : 400;
    const int pad = 12;
    const int bar_w = small ? 4 : 6, bars = 60;
    const int graph_h = small ? 36 : 64;
    const int top_h = small ? th : Menu::GlyphHeight;
    const int box_h = (small ? 8 : 10) + top_h + (small ? 4 : 6) + graph_h + (small ? 6 : 8) + (small ? 2 : 3) * 22 + 6;
    const int x = s.settings.hud_right ? int(s.width) - box_w - 40 : 40;
    const int y = 40;
    constexpr std::uint32_t Back = 0x101830, Fore = 0xE8F0FF, Dim = 0x8FA0C8, Well = 0x0A1020, Guide = 0x2A3C70;

    const double target = TargetRate(s.fps_history, s.fps);
    const std::uint32_t status = RateColour(s.fps, target);

    Element& e = s.element[ElHud];
    e.rect = {x, y, box_w, box_h};
    e.px.assign(size_t(box_w) * size_t(box_h), Pixel(s, Back));
    Surface sf{e.px.data(), box_w, box_w, box_h, x, y};
    FillRect(s, sf, x, y, 4, box_h, status); // the bar at the left says how it is going

    const int left = x + 4 + pad;
    int cy = y + (small ? 8 : 10);
    char text[64];
    const double frame_ms = s.fps > 0 ? 1000.0 / s.fps : 0.0;
    if (small) {
        std::snprintf(text, sizeof(text), "%.1f", s.fps);
        int pen = left + DrawText(s, sf, left, cy, text, status, Back, true);
        pen += DrawText(s, sf, pen, cy, " FPS ", Dim, Back, true);
        std::snprintf(text, sizeof(text), "%.1f ms", frame_ms);
        pen += DrawText(s, sf, pen, cy, text, Fore, Back, true);
        std::snprintf(text, sizeof(text), pt ? "  pior %.0f" : "  worst %.0f", s.worst_ms);
        DrawText(s, sf, pen, cy, text, s.worst_ms > 60 ? HudBad : s.worst_ms > 40 ? HudWarn : Dim, Back, true);
    } else {
        std::snprintf(text, sizeof(text), s.fps >= 99.5 ? "%.0f" : "%.1f", s.fps);
        const int used = DrawText(s, sf, left, cy, text, status, Back, false);
        DrawText(s, sf, left + used + 6, cy + 18, "FPS", Dim, Back, true);
        std::snprintf(text, sizeof(text), "%.1f ms", frame_ms);
        DrawText(s, sf, x + box_w - pad - TextPixels(text, true), cy, text, Fore, Back, true);
        std::snprintf(text, sizeof(text), pt ? "pior %.0f ms" : "worst %.0f ms", s.worst_ms);
        DrawText(s, sf, x + box_w - pad - TextPixels(text, true), cy + 20, text,
                 s.worst_ms > 60 ? HudBad : s.worst_ms > 40 ? HudWarn : Dim, Back, true);
    }
    cy += top_h + (small ? 4 : 6);

    // The graph: one bar per second, the newest at the right, the target drawn as a line.
    const int graph_w = bars * bar_w;
    const int gx = left;
    FillRect(s, sf, gx - 2, cy, graph_w + 4, graph_h, Well);
    double scale = std::max(target * 1.25, 1.0);
    for (float v : s.fps_history) {
        scale = std::max(scale, double(v));
    }
    const int guide_y = cy + graph_h - 1 - int(std::min(target / scale, 1.0) * (graph_h - 2));
    FillRect(s, sf, gx - 2, guide_y, graph_w + 4, 1, Guide);
    const int have = std::min<int>(int(s.fps_history.size()), bars);
    for (int i = 0; i < have; ++i) {
        const double v = double(s.fps_history[s.fps_history.size() - size_t(have) + size_t(i)]);
        const int bh = std::clamp(int(v / scale * (graph_h - 2) + 0.5), v > 0 ? 1 : 0, graph_h - 2);
        const int bx = gx + (bars - have + i) * bar_w;
        FillRect(s, sf, bx, cy + graph_h - 1 - bh, bar_w - 1, bh, RateColour(v, target));
    }
    cy += graph_h + (small ? 6 : 8);

    // CPU load: a bar and the number of busy cores.
    const double load = std::clamp(s.cores / 8.0, 0.0, 1.0);
    const std::uint32_t load_colour = load < 0.6 ? HudGood : load < 0.85 ? HudWarn : HudBad;
    const auto draw_load = [&](int lx, int ly, int bar_len) {
        int pen = lx + DrawText(s, sf, lx, ly, "CPU ", Dim, Back, true);
        FillRect(s, sf, pen, ly + 5, bar_len, 10, Well);
        FillRect(s, sf, pen, ly + 5, int(load * bar_len + 0.5), 10, load_colour);
        pen += bar_len + 6;
        std::snprintf(text, sizeof(text), "%.1f/8", s.cores);
        return pen + DrawText(s, sf, pen, ly, text, Fore, Back, true);
    };
    const std::uint32_t mem_colour = s.free_mib >= 600 ? Fore : s.free_mib >= 300 ? HudWarn : HudBad;
    const auto mem_text = [&](bool short_form) {
        std::snprintf(text, sizeof(text), short_form ? "RAM %llu MiB" : (pt ? "RAM %llu MiB livre" : "RAM %llu MiB free"),
                      static_cast<unsigned long long>(s.free_mib));
    };
    if (small) {
        const int end = draw_load(left, cy, 40);
        mem_text(true);
        DrawText(s, sf, end + 10, cy, text, mem_colour, Back, true);
        cy += 22;
    } else {
        draw_load(left, cy, 150);
        cy += 22;
        mem_text(false);
        DrawText(s, sf, left, cy, text, mem_colour, Back, true);
        cy += 22;
    }
    const long hottest = std::max(s.cpu_temp, s.soc_temp);
    const std::uint32_t temp_colour = hottest > 85 ? HudBad : hottest > 75 ? HudWarn : Fore;
    if (s.cpu_temp >= 0 || s.soc_temp >= 0) {
        std::snprintf(text, sizeof(text), "TEMP  CPU %ld C   SoC %ld C", s.cpu_temp, s.soc_temp);
    } else {
        std::snprintf(text, sizeof(text), "TEMP  --");
    }
    DrawText(s, sf, left, cy, text, temp_colour, Back, true);
    e.built = true;
    ++e.version;
}

/// The notice: small text on a dark strip, bottom left, clear of the HUD at the top.
void BuildNotice(State& s) {
    const int cell_w = Menu::GlyphWidth / 2, cell_h = Menu::GlyphHeight / 2;
    s.notice_cells = std::max(s.notice_cells, Menu::TextCells(s.notice));
    const int box_w = s.notice_cells * cell_w + 16, box_h = cell_h + 12;
    const int x = 40, y = int(s.height) - 40 - box_h;
    constexpr std::uint32_t Back = 0x2A1C08, Fore = 0xFFD27A;
    Element& e = s.element[ElNotice];
    e.rect = {x, y, box_w, box_h};
    e.px.assign(size_t(box_w) * size_t(box_h), Pixel(s, Back));
    Surface sf{e.px.data(), box_w, box_w, box_h, x, y};
    DrawText(s, sf, x + 8, y + 6, FitCells(s.notice, s.notice_cells), Fore, Back, true);
    e.built = true;
    ++e.version;
}

/// The pause menu (0.9.5 zip 5): a framed card in the middle of the screen. A title band with the name at the left and a
/// note at the right, one row per line (the chosen row has a bar and a lighter background; a "label<TAB>value" row has its
/// value in a pill at the right), and key caps at the bottom. See video_hook.h for the text conventions.
void BuildPanel(State& s) {
    std::string title = s.panel_title, note;
    if (const size_t tab = title.find('\t'); tab != std::string::npos) {
        note = title.substr(tab + 1);
        title.resize(tab);
    }
    struct Row {
        std::string label, value;
        int kind = 0; // 0 plain, 1 on, 2 off
    };
    std::vector<Row> rows;
    int cells = Menu::TextCells(title) + (note.empty() ? 0 : (TextPixels(note, true) + 19) / Menu::GlyphWidth + 3);
    for (const std::string& l : s.panel_lines) {
        Row row;
        if (const size_t tab = l.find('\t'); tab != std::string::npos) {
            row.label = l.substr(0, tab);
            row.value = l.substr(tab + 1);
            if (!row.value.empty() && (row.value[0] == PanelValueOn || row.value[0] == PanelValueOff)) {
                row.kind = row.value[0] == PanelValueOn ? 1 : 2;
                row.value.erase(0, 1);
            }
        } else {
            row.label = l;
        }
        cells = std::max(cells, Menu::TextCells(row.label) + (row.value.empty() ? 0 : Menu::TextCells(row.value) + 5) + 4);
        rows.push_back(std::move(row));
    }
    // The footer: parts separated by three spaces, "KEY: words" drawn as a key cap and its words.
    struct Part {
        std::string key, words;
    };
    std::vector<Part> parts;
    for (size_t i = 0; i < s.panel_footer.size();) {
        size_t end = s.panel_footer.find("   ", i);
        if (end == std::string::npos) {
            end = s.panel_footer.size();
        }
        const std::string text = s.panel_footer.substr(i, end - i);
        Part part;
        if (const size_t colon = text.find(": "); colon != std::string::npos && colon <= 12) {
            part.key = text.substr(0, colon);
            part.words = text.substr(colon + 2);
        } else {
            part.words = text;
        }
        if (!part.key.empty() || !part.words.empty()) {
            parts.push_back(std::move(part));
        }
        i = end;
        while (i < s.panel_footer.size() && s.panel_footer[i] == ' ') {
            ++i;
        }
    }
    const int half_w = Menu::GlyphWidth / 2, half_h = Menu::GlyphHeight / 2;
    int footer_px = 0;
    for (const Part& part : parts) {
        footer_px += (part.key.empty() ? 0 : TextPixels(part.key, true) + 16 + 8) + TextPixels(part.words, true) + 24;
    }
    cells = std::max(cells, (std::max(footer_px - 24, 0) + Menu::GlyphWidth - 1) / Menu::GlyphWidth);
    s.panel_cells = std::max(s.panel_cells, cells);
    s.panel_rows = std::max(s.panel_rows, int(rows.size()));
    const int w = std::min<int>(s.panel_cells * Menu::GlyphWidth + 80, int(s.width) - 80);
    const int h = s.panel_rows * 56 + 150;
    const int x = (int(s.width) - w) / 2, y = (int(s.height) - h) / 2;
    // The accent colours follow the menu's colour theme (settings.txt: menu_theme; the blue theme is 0x4A86FF and 0x2DD4C4).
    constexpr std::uint32_t Back = 0x141C38, Text = 0xEAF0FF, Dim = 0x8FA0C8;
    const std::uint32_t Edge = Menu::Color::Accent & 0x00FFFFFFu, Teal = Menu::Color::Teal & 0x00FFFFFFu;
    const std::uint32_t Chosen = Menu::Color::Selected & 0x00FFFFFFu, ChosenEnd = Lerp(Chosen, Back, 4, 10);
    Element& e = s.element[ElPanel];
    e.rect = {x - 4, y - 4, w + 8, h + 8};
    e.px.assign(size_t(e.rect.w) * size_t(e.rect.h), Pixel(s, Edge));
    Surface sf{e.px.data(), e.rect.w, e.rect.w, e.rect.h, e.rect.x, e.rect.y};
    FillRect(s, sf, x, y, w, h, Back);

    // The title band.
    FillGradientV(s, sf, x, y, w, 72, Lerp(Back, Edge, 2, 10), Back);
    FillRect(s, sf, x + 40, y + 66, 120, 3, Teal);
    FillRect(s, sf, x + 160, y + 66, 60, 3, Edge);
    DrawText(s, sf, x + 40, y + 14, title, Teal, OverBackground, false);
    if (!note.empty()) {
        DrawText(s, sf, x + w - 40 - TextPixels(note, true), y + 30, note, Dim, OverBackground, true);
    }

    // The rows.
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const int ly = y + 84 + int(i) * 56;
        const bool on = int(i) == s.panel_selected;
        if (on) {
            FillGradientH(s, sf, x + 20, ly - 4, w - 40, 52, Chosen, ChosenEnd);
            FillRect(s, sf, x + 20, ly - 4, 6, 52, Teal);
        }
        DrawText(s, sf, x + 44, ly, row.label, on ? 0xFFFFFF : Text, OverBackground, false);
        if (!row.value.empty()) {
            const int vw = TextPixels(row.value, false);
            const int px = x + w - 40 - vw - 12;
            std::uint32_t pill = on ? Lerp(Chosen, 0xFFFFFF, 1, 8) : Lerp(Back, Chosen, 1, 2), ink = Text;
            if (row.kind == 1) {
                pill = on ? 0x2AA85F : 0x1F7A45;
                ink = 0xFFFFFF;
            } else if (row.kind == 2) {
                pill = on ? 0x58607E : 0x3A3F55;
                ink = 0xC8D0E8;
            }
            FillRect(s, sf, px, ly + 1, vw + 24, 42, pill);
            DrawText(s, sf, px + 12, ly + 2, row.value, ink, pill, false);
        }
    }

    // The key caps.
    FillRect(s, sf, x + 40, y + h - 62, w - 80, 1, 0x2A3C70);
    int fx = x + 40;
    const int fy = y + h - 44;
    for (const Part& part : parts) {
        if (!part.key.empty()) {
            const int cap = TextPixels(part.key, true) + 16;
            FillRect(s, sf, fx, fy, cap, half_h + 8, 0x7C8FC8);
            FillRect(s, sf, fx + 1, fy + 1, cap - 2, half_h + 6, 0x2E3A66);
            DrawText(s, sf, fx + 8, fy + 4, part.key, 0xFFFFFF, 0x2E3A66, true);
            fx += cap + 8;
        }
        fx += DrawText(s, sf, fx, fy + 4, part.words, Dim, Back, true) + 24;
    }
    (void)half_w;
    e.built = true;
    ++e.version;
}

bool Active(const State& s, int element) {
    switch (element) {
    case ElHud:
        return s.hud;
    case ElNotice:
        return !s.notice.empty();
    default:
        return s.panel_active;
    }
}

/// Makes the bitmaps of the elements that are showing and changed since they were built.
void EnsureBuilt(State& s) {
    if (s.pitch == 0) {
        return;
    }
    if (s.hud && !s.element[ElHud].built) {
        BuildHud(s);
    }
    if (!s.notice.empty() && !s.element[ElNotice].built) {
        BuildNotice(s);
    }
    if (s.panel_active && !s.element[ElPanel].built) {
        BuildPanel(s);
    }
}

/// Every bitmap is made again (a new set of buffers: the format may differ) and no buffer is known
/// to hold any of them.
void InvalidateElements(State& s) {
    for (Element& e : s.element) {
        e.built = false;
    }
    std::memset(s.stamped, 0, sizeof(s.stamped));
}

/// The element's rows, copied into the buffer: every pixel is written once, with its final value.
void Blit(const State& s, int buffer, const Element& e) {
    std::uint32_t* base = s.address[buffer];
    const int x0 = std::max(e.rect.x, 0), x1 = std::min(e.rect.x + e.rect.w, int(s.width));
    if (x1 <= x0) {
        return;
    }
    for (int j = 0; j < e.rect.h; ++j) {
        const int y = e.rect.y + j;
        if (y < 0 || y >= int(s.height)) {
            continue;
        }
        std::uint32_t* const row = base + size_t(y) * s.pitch + size_t(x0);
        const size_t bytes = size_t(x1 - x0) * sizeof(std::uint32_t);
        std::memcpy(row, e.px.data() + size_t(j) * size_t(e.rect.w) + size_t(x0 - e.rect.x), bytes);
#if defined(__SSE2__)
        // 0.7.7 fix: the buffers may be ordinary cached memory (ORBIS_VRAM_GARLIC=0 puts every image on the
        // cache-coherent bus), and the display reads memory, not the CPU's cache: push the lines just written
        // out, or the overlay shows up whenever the cache happens to evict them. Harmless on write-combined memory.
        const char* line = reinterpret_cast<const char*>(reinterpret_cast<std::uintptr_t>(row) & ~std::uintptr_t{63});
        for (const char* end = reinterpret_cast<const char*>(row) + bytes; line < end; line += 64) {
            _mm_clflush(line);
        }
#endif
    }
}

/// Is the element still in the buffer? Nine pixels spread over it are read and compared with the
/// bitmap: when the game paints a frame over it, a horizontal or vertical edge of that paint cannot
/// pass through the element without leaving probes on both sides of it. Reads of video memory are
/// slow, so it is only these nine.
bool Intact(State& s, int buffer, const Element& e) {
    const volatile std::uint32_t* base = s.address[buffer];
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            const int lx = (e.rect.w - 1) * (1 + 3 * i) / 8; // 1/8, 4/8, 7/8 of the width
            const int ly = (e.rect.h - 1) * (1 + 3 * j) / 8;
            const int x = e.rect.x + lx, y = e.rect.y + ly;
            if (x < 0 || y < 0 || x >= int(s.width) || y >= int(s.height)) {
                continue;
            }
            ++s.ka.probe_reads;
            if (base[size_t(y) * s.pitch + size_t(x)] != e.px[size_t(ly) * size_t(e.rect.w) + size_t(lx)]) {
                return false;
            }
        }
    }
    return true;
}

/// Keep-alive: puts every showing element into the buffer where the buffer lacks it (never copied
/// there, changed since, or painted over by the game). Returns how many were copied.
int StampBuffer(State& s, int buffer, std::uint64_t* changed, std::uint64_t* painted_over) {
    if (buffer < 0 || buffer >= s.count || s.address[buffer] == nullptr) {
        return 0;
    }
    int copied = 0;
    for (int id = 0; id < ElCount; ++id) {
        Element& e = s.element[id];
        if (!Active(s, id) || !e.built) {
            continue;
        }
        const bool stale = s.stamped[buffer][id] != e.version;
        if (!stale && Intact(s, buffer, e)) {
            continue;
        }
        Blit(s, buffer, e);
        s.stamped[buffer][id] = e.version;
        ++(stale ? *changed : *painted_over);
        ++copied;
    }
    if (copied > 0) {
        FlushWrites();
    }
    return copied;
}

/// 0.7.7 fix, at the flip: the game's GPU is done with the frame, so this is the last write before the display
/// takes it. The small elements (HUD, notice) are written whatever the nine probes say: a probe can miss a thin
/// band the game left over part of the HUD, a write over unchanged pixels costs nothing on screen. The panel
/// (large) is only written where the probes find it missing.
void FlipStamp(State& s, int buffer) {
    if (buffer < 0 || buffer >= s.count || s.address[buffer] == nullptr) {
        return;
    }
    int copied = 0;
    for (int id = 0; id < ElCount; ++id) {
        Element& e = s.element[id];
        if (!Active(s, id) || !e.built) {
            continue;
        }
        const bool stale = s.stamped[buffer][id] != e.version;
        const bool intact = !stale && Intact(s, buffer, e);
        if (intact && id == ElPanel) {
            continue;
        }
        if (!intact) {
            ++s.ka.stamps_flip; // the frame was about to go out without it
        }
        Blit(s, buffer, e);
        s.stamped[buffer][id] = e.version;
        ++copied;
    }
    if (copied > 0) {
        FlushWrites();
        ++s.ka.blind_flips;
    }
}

/// Keep-alive, after a flip: the flipped buffer is the one on screen for the next 40 ms. First look (before the
/// repairs): is the overlay still in it? The count tells whether anything paints over a buffer that is being shown.
void LookAtShown(State& s) {
    if (!s.settings.flip_stamp || s.last_index < 0 || s.last_index >= s.count || s.address[s.last_index] == nullptr || s.submit_us == 0) {
        return;
    }
    if (s.looked_flip == s.flips) {
        return;
    }
    s.looked_flip = s.flips;
    s.restamp_us = 0;
    for (int id : {int(ElHud), int(ElNotice)}) {
        Element& e = s.element[id];
        if (Active(s, id) && e.built && s.stamped[s.last_index][id] == e.version && !Intact(s, s.last_index, e)) {
            ++s.ka.erased_after_flip;
            break;
        }
    }
}

/// ...and then the small elements are written again every 4 ms for those 40 ms, in case something did.
void RestampShown(State& s) {
    if (!s.settings.flip_stamp || s.last_index < 0 || s.last_index >= s.count || s.address[s.last_index] == nullptr || s.submit_us == 0) {
        return;
    }
    const std::uint64_t now = NowMicros();
    if (now < s.submit_us || now - s.submit_us > 40000) {
        return;
    }
    if (s.restamp_us != 0 && now - s.restamp_us < 4000) {
        return;
    }
    int copied = 0;
    for (int id : {int(ElHud), int(ElNotice)}) {
        Element& e = s.element[id];
        if (Active(s, id) && e.built) {
            Blit(s, s.last_index, e);
            s.stamped[s.last_index][id] = e.version;
            ++copied;
        }
    }
    if (copied > 0) {
        FlushWrites();
        ++s.ka.restamps;
    }
    s.restamp_us = now;
}

bool Drawable(const State& s) {
    return s.armed && s.settings.draw && s.linear && s.handle >= 0 && s.count > 0 && s.pitch != 0;
}

/// Classic mode: everything that is showing, into one buffer.
void DrawAllClassic(State& s, int buffer) {
    EnsureBuilt(s);
    for (int id = 0; id < ElCount; ++id) {
        if (Active(s, id) && s.element[id].built) {
            Blit(s, buffer, s.element[id]);
        }
    }
    FlushWrites();
}

void PumpKeepAlive(State& s) {
    std::lock_guard lock{s.mutex};
    if (!Drawable(s)) {
        return;
    }
    EnsureBuilt(s);
    LookAtShown(s); // before the repairs, so it can count what was erased
    for (int b = 0; b < s.count; ++b) {
        StampBuffer(s, b, &s.ka.stamps_content, &s.ka.stamps_repair);
    }
    RestampShown(s);
    s.notice_dirty = s.panel_dirty = false;
}

void PumpClassic(State& s) {
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
        DrawAllClassic(s, index); // a frame the game flips while the menu is up gets the panel too
        if (s.panel_active) {
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
        EnsureBuilt(s);
        if (panel && s.element[ElPanel].built) {
            Blit(s, index, s.element[ElPanel]);
            s.panel_dirty = false;
        }
        if (notice && Active(s, ElNotice) && s.element[ElNotice].built) {
            Blit(s, index, s.element[ElNotice]);
            s.notice_dirty = false;
        }
        FlushWrites();
    }
}

/// How long the worker sleeps between two rounds. Keep-alive: the configured period, and a short one
/// for a moment after each flip, which is when the game's GPU is most likely to paint over the overlay.
std::uint64_t WorkerSleepMicros() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    if (!s.settings.keepalive) {
        return 2000;
    }
    const std::uint64_t base = std::uint64_t(std::clamp(s.settings.poll_ms, 1, 8)) * 1000;
    const bool hot = s.submit_us != 0 && NowMicros() - s.submit_us < 25000;
    return hot ? std::min<std::uint64_t>(base, 300) : base;
}

} // Anonymous namespace

Settings& Pending() {
    static Settings settings;
    return settings;
}

void Pump() {
    State& s = S();
    RefreshStats();
    bool keepalive;
    {
        std::lock_guard lock{s.mutex};
        keepalive = s.settings.keepalive;
    }
    if (keepalive) {
        PumpKeepAlive(s);
    } else {
        PumpClassic(s);
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
        s.notice_cells = 0;
        s.panel_cells = s.panel_rows = 0;
        s.ka = KeepAliveStats{};
        s.stats_frozen = false;
        s.submit_us = 0;
        s.looked_flip = ~std::uint64_t{0};
        s.restamp_us = 0;
        InvalidateElements(s);
        s.worker_wanted = start_worker && settings.draw;
    }
}

bool Ready() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    return s.armed && s.settings.draw && s.linear && s.handle >= 0 && s.count > 0 && s.last_index >= 0;
}

bool SaveScreenshot(const std::string& path, std::string* why) {
    State& s = S();
    std::vector<std::uint32_t> copy;
    bool red_low = false;
    int width = 0, height = 0, index = -1;
    {
        // Only the copy is done under the lock; the file is written after it, so the worker and the
        // flip call are never held up by the disk.
        std::lock_guard lock{s.mutex};
        if (!s.armed || !s.linear || s.handle < 0 || s.count <= 0 || s.last_index < 0 || s.last_index >= s.count ||
            s.address[s.last_index] == nullptr || s.width == 0 || s.height == 0) {
            if (why != nullptr) {
                *why = "no frame on screen yet (the video buffers are not known or not linear)";
            }
            return false;
        }
        width = static_cast<int>(s.width);
        height = static_cast<int>(s.height);
        index = s.last_index;
        red_low = s.format_used == FormatA8B8G8R8;
        copy.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        const std::uint32_t* src = s.address[index];
        // Row by row and in order: the buffers may be write-combined memory, which is slow to read
        // but fine when it is read front to back.
        for (int y = 0; y < height; ++y) {
            std::memcpy(copy.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width),
                        src + static_cast<std::size_t>(y) * s.pitch, static_cast<std::size_t>(width) * sizeof(std::uint32_t));
        }
    }
    return Ps4Shot::WriteBmp(path, copy.data(), width, height, red_low, why);
}

void SetHudStatsForTest(const HudStats& stats) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    s.stats_frozen = true;
    s.fps = stats.fps;
    s.worst_ms = stats.worst_ms;
    s.cores = stats.cores;
    s.cpu_temp = stats.cpu_temp;
    s.soc_temp = stats.soc_temp;
    s.free_mib = stats.free_mib;
    s.fps_history = stats.history;
    s.element[ElHud].built = false;
}

void SetHud(bool on) {
    State& s = S();
    std::lock_guard lock{s.mutex};
    s.hud = on;
    s.element[ElHud].built = false;
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
    if (!s.panel_active) {
        s.panel_cells = s.panel_rows = 0; // a new panel: its size starts again
    }
    s.panel_active = true;
    s.panel_dirty = true;
    s.panel_title = title;
    s.panel_lines = lines;
    s.panel_selected = selected;
    s.panel_footer = footer;
    s.element[ElPanel].built = false;
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
        s.element[ElNotice].built = false;
        if (text.empty()) {
            s.notice_cells = 0;
        }
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

KeepAliveStats GetKeepAliveStats() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    return s.ka;
}

Stall LastStall() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    return s.stall;
}

std::uint64_t SinceLastFlipUs() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    if (s.tl_prev_us == 0) {
        return 0;
    }
    const std::uint64_t now = NowMicros();
    return now > s.tl_prev_us ? now - s.tl_prev_us : 0;
}

Timeline TakeTimeline() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    Timeline out = s.tl;
    out.flips_total = s.flips;
    s.tl = Timeline{};
    return out;
}

void SetClockForTest(std::uint64_t (*now_micros)()) {
    g_clock = now_micros;
}

std::string Describe() {
    State& s = S();
    std::lock_guard lock{s.mutex};
    char text[560];
    std::snprintf(text, sizeof(text),
                  "video hook: armed=%d handle=%d buffers=%d %ux%u pitch %u tiling %u (%s), format seen 0x%08x used 0x%08x, %llu flips, %s",
                  s.armed, s.handle, s.count, s.width, s.height, s.pitch, s.tiling, s.linear ? "linear: drawing on" : "NOT linear: drawing off",
                  s.format_seen, s.format_used, static_cast<unsigned long long>(s.flips),
                  s.settings.keepalive ? "keep-alive" : "classic");
    std::string out = text;
    if (s.settings.keepalive) {
        std::snprintf(text, sizeof(text),
                      " (copied: %llu at the flip, %llu repairs after the game painted over it, %llu for new content; %llu probe reads; 0.7.7 fix: %llu flips stamped, %llu overlays found missing right after a flip, %llu rewrites while shown)",
                      static_cast<unsigned long long>(s.ka.stamps_flip), static_cast<unsigned long long>(s.ka.stamps_repair),
                      static_cast<unsigned long long>(s.ka.stamps_content), static_cast<unsigned long long>(s.ka.probe_reads),
                      static_cast<unsigned long long>(s.ka.blind_flips), static_cast<unsigned long long>(s.ka.erased_after_flip),
                      static_cast<unsigned long long>(s.ka.restamps));
    } else {
        std::snprintf(text, sizeof(text), ", drawn %llu hidden / %llu late", static_cast<unsigned long long>(s.draws_hidden),
                      static_cast<unsigned long long>(s.draws_late));
    }
    return out + text;
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
            s.count = std::max(s.count, std::min(start + count, MaxBuffers));
            s.last_index = -1; // a new set of buffers: nothing is on screen from it yet
            InvalidateElements(s); // the pixel format may differ, and no buffer holds any bitmap yet
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
                    std::this_thread::sleep_for(std::chrono::microseconds(WorkerSleepMicros()));
                }
            });
            s.worker.detach();
            Ps4::Log("video hook: worker started (%s)", Pending().keepalive ? "keep-alive" : "classic");
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
        std::lock_guard lock{s.mutex};
        if (s.armed && handle == s.handle && index >= 0 && index < s.count && s.address[index] != nullptr) {
            s.last_index = index;
            ++s.flips;
            s.submit_us = NowMicros();
            if (s.tl_prev_us != 0 && s.submit_us >= s.tl_prev_us) {
                const std::uint64_t gap = s.submit_us - s.tl_prev_us;
                s.tl.gap_sum_us += gap;
                s.tl.gap_max_us = std::max(s.tl.gap_max_us, gap);
                s.tl.over_34ms += gap > 34000;
                s.tl.over_50ms += gap > 50000;
                s.tl.over_100ms += gap > 100000;
                s.tl.over_250ms += gap > 250000;
                s.gap_max_window_us = std::max(s.gap_max_window_us, gap);
                if (gap >= StallGapUs) {
                    ++s.stall.count;
                    s.stall.gap_us = gap;
                    s.stall.end_us = s.submit_us;
                    s.stall.flip = s.flips;
                }
            }
            s.tl_prev_us = s.submit_us;
            ++s.tl.flips;
            // Keep-alive: the frame is stamped before the real flip. If the game's GPU is already done
            // with it this is its final overlay; if not, the worker puts the overlay back within a
            // millisecond of the game painting over it. Only bitmaps that already exist are copied
            // here (nothing is built on this thread), and a buffer that still has its overlay costs
            // nine reads.
            if (s.settings.keepalive && s.settings.flip_stamp && Drawable(s)) {
                FlipStamp(s, index);
            }
        }
    }
    return __real_sceVideoOutSubmitFlip(handle, index, mode, arg);
}
