// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: a minimal PS4 frontend for the Eden emulator core.
//
// Personal port, not affiliated with the Eden project. The boot sequence follows Eden's own
// command-line frontend (src/yuzu_cmd/yuzu.cpp) and ProsperoEden's PS5 frontend.
//
// Files, all under /data/edenps4:
//   keys/prod.keys (+ title.keys)     the console's keys, dumped from the user's Switch
//   firmware/*.nca                    the firmware, dumped from the user's Switch (copied into
//                                     nand/system/Contents/registered the first time)
//   roms/*.nsp|*.xci                  listed in the menu that opens at start (menu/menu_ui.h: games
//                                     and settings in tabs), the last one played preselected,
//                                     unless game.txt names one
//   game.txt                          optional: the full path of the game to start
//   updates/                          update and DLC NSPs, applied when the game starts
//   nomenu.txt                        optional: skip the menu, start the last game played (or,
//                                     with no games, the bundled Homebrew Menu)
//   settings.txt                      "key=value" per line; written by the menu's settings tabs
//                                     and still fine to edit by hand (see ApplySettingsFile)
//   boot.log                         this frontend's log; log/eden_log.txt is Eden's
//
// Controls (Switch layout by position): Circle=A Cross=B Triangle=X Square=Y, L1/R1=L/R,
// L2/R2=ZL/ZR, Options=+, touch pad click=-, L3/R3 sticks. Options + touch pad for 2 s quits.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/orbis_lazy_memory.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"

#include <csignal>
#include <unistd.h>

#include "firmware_zip.h"
#include "menu/menu_ui.h"
#include "video_hook.h"
#include "menu/rom_menu.h"
#include "ps4_platform.h"
#include "shader_policy.h"

namespace fs = std::filesystem;

extern "C" void Ps4HeapCensus(); // heap_census.cpp

namespace Vulkan {
extern int ps4_bgra_mode;    // vk_texture_cache.cpp
extern int ps4_swizzle_mode; // vk_texture_cache.cpp
extern int ps4_rg_mode;      // vk_texture_cache.cpp
extern int ps4_a2b10_mode;   // vk_texture_cache.cpp
}

namespace {

constexpr u32 OutputWidth = 1920;
constexpr u32 OutputHeight = 1080;

class DummyContext final : public Core::Frontend::GraphicsContext {};

/// The TV: RADV's headless surface, whose presents become video-out flips.
class Ps4Window final : public Core::Frontend::EmuWindow {
public:
    Ps4Window() {
        window_info.type = Core::Frontend::WindowSystemType::Orbis;
        // Non-null so the renderer presents instead of running headless.
        window_info.render_surface = reinterpret_cast<void*>(1);
        window_info.render_surface_scale = 1.0f;
        UpdateCurrentFramebufferLayout(OutputWidth, OutputHeight);
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<DummyContext>();
    }
    bool IsShown() const override {
        return true;
    }
    void OnFrameDisplayed() override {
        Ps4::NoteFrame();
    }
};

bool HasExtension(const fs::path& p, std::initializer_list<const char*> exts) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return std::any_of(exts.begin(), exts.end(), [&](const char* x) { return e == x; });
}

/// What the menu's SYSTEM tab shows: why a game may not start (keys, firmware) and where things are.
Menu::SystemInfo CollectSystemInfo() {
    Menu::SystemInfo info;
    const fs::path data{Ps4::DataDir};
    std::error_code ec;
    info.build_id = EDEN_PS4_BUILD_ID;
    info.data_dir = Ps4::DataDir;
    info.keys_found = fs::exists(data / "keys/prod.keys", ec);
    for (fs::directory_iterator it{data / "firmware", ec}, end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && HasExtension(it->path(), {".nca"})) {
            ++info.firmware_files;
        } else if (it->is_regular_file(ec) && HasExtension(it->path(), {".zip"})) {
            ++info.firmware_zips;
        }
    }
    // Already installed (from an earlier run)?
    for (fs::directory_iterator it{data / "nand/system/Contents/registered", ec}, end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && HasExtension(it->path(), {".nca"})) {
            ++info.firmware_installed;
        }
    }
    info.free_memory_mib = Ps4::FreeDirectMemory() >> 20;
    // The per-game shader caches Eden keeps in cache/shader/<title id>/.
    for (fs::directory_iterator game{data / "cache/shader", ec}, end; !ec && game != end; game.increment(ec)) {
        if (!game->is_directory(ec)) {
            continue;
        }
        std::uint64_t bytes = 0;
        std::error_code walk_ec;
        for (fs::recursive_directory_iterator f{game->path(), walk_ec}, stop; !walk_ec && f != stop; f.increment(walk_ec)) {
            std::error_code size_ec;
            if (f->is_regular_file(size_ec)) {
                bytes += fs::file_size(f->path(), size_ec);
            }
        }
        if (bytes > 0) {
            ++info.shader_cache_games;
            info.shader_cache_mib += bytes >> 20;
        }
    }
    info.clear_shader_cache = [data] {
        std::error_code clear_ec;
        fs::remove_all(data / "cache/shader", clear_ec);
        fs::create_directories(data / "cache/shader", clear_ec);
        return !clear_ec;
    };
    // Can the app write here? (No boot.log and no settings.txt otherwise.)
    {
        const fs::path probe = data / ".write-test";
        {
            std::ofstream out{probe, std::ios::binary | std::ios::trunc};
            out << 'x';
            out.flush();
            info.data_writable = static_cast<bool>(out);
        }
        fs::remove(probe, ec);
    }
    const auto log_size = fs::file_size(data / "boot.log", ec);
    info.log_bytes = ec ? -1 : std::int64_t(log_size);
    const auto settings_size = fs::file_size(data / "settings.txt", ec);
    info.settings_bytes = ec ? -1 : std::int64_t(settings_size);
    // The LOG tab: the end of the previous session's log and of this one so far.
    {
        std::ifstream in{data / "boot.old.log"};
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) {
            lines.push_back(line);
            if (lines.size() > 40) {
                lines.erase(lines.begin());
            }
        }
        info.log_previous = std::move(lines);
        info.log_current = Ps4::RecentLog(40);
    }
    return info;
}

std::string FindGame() {
    const fs::path data{Ps4::DataDir};
    std::error_code ec;
    if (std::ifstream in{data / "game.txt"}; in) {
        std::string line;
        std::getline(in, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && fs::is_regular_file(line, ec)) {
            return line;
        }
        Ps4::Log("game.txt names '%s', which is not a file", line.c_str());
    }
    std::vector<fs::path> games;
    for (fs::directory_iterator it{data / "roms", ec}, end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && HasExtension(it->path(), {".nsp", ".xci", ".nro", ".nso", ".nca"})) {
            games.push_back(it->path());
        }
    }
    std::sort(games.begin(), games.end());

    // The last game played is preselected in the menu (menu/menu_ui.h).
    const fs::path last_file = data / "last_game.txt";
    size_t index = 0;
    if (!games.empty()) {
        if (std::ifstream in{last_file}; in) {
            std::string last;
            std::getline(in, last);
            for (size_t i = 0; i < games.size(); ++i) {
                if (games[i].filename().string() == last) {
                    index = i;
                }
            }
        }
    }
    const fs::path bundled{"/app0/assets/misc/hbmenu.nro"};

    // The menu opens whether or not there are games: its tabs hold the settings and the facts
    // (keys, firmware) that explain why a game does not start. nomenu.txt skips it (in case Eden
    // cannot open the display after it).
    if (!fs::exists(data / "nomenu.txt", ec)) {
        Ps4::SetPadLight(255, 255, 255);
        std::vector<Menu::GameEntry> entries;
        for (const auto& game : games) {
            std::error_code size_ec;
            entries.push_back({game.filename().string(), std::uint64_t(fs::file_size(game, size_ec))});
        }
        const MenuOutcome outcome = RunMainMenu(entries, int(index), CollectSystemInfo());
        if (outcome.choice == MenuChoice::Homebrew) {
            // The Homebrew Menu (switchbrew/nx-hbmenu, ISC) that ships in the package: a test of
            // CPU, GPU and presentation that needs no game.
            Ps4::Log("the bundled Homebrew Menu was picked in the menu");
            return fs::is_regular_file(bundled, ec) ? bundled.string() : std::string{};
        }
        if (outcome.choice == MenuChoice::Game && outcome.game >= 0 && size_t(outcome.game) < games.size()) {
            index = size_t(outcome.game);
        }
        // MenuChoice::Failed: the display could not be opened; go on with the last game.
    }
    if (!games.empty()) {
        if (std::ofstream out{last_file}; out) {
            out << games[index].filename().string() << '\n';
        }
        Ps4::Log("game %zu of %zu in roms/", index + 1, games.size());
        return games[index].string();
    }
    // Nothing of the user's and no menu: the bundled Homebrew Menu.
    if (fs::is_regular_file(bundled, ec)) {
        Ps4::Log("no game of yours found; starting the bundled Homebrew Menu");
        return bundled.string();
    }
    return {};
}

/// The firmware is read from nand/system/Contents/registered. A dump in firmware/ is copied there
/// the first time (and again if a file is missing or has another size).
void InstallFirmware() {
    const fs::path source = fs::path{Ps4::DataDir} / "firmware";
    const fs::path target = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "system/Contents/registered";
    std::error_code ec;
    if (!fs::is_directory(source, ec)) {
        Ps4::Log("no firmware/ folder; games that need the firmware will not start");
        return;
    }
    fs::create_directories(target, ec);
    size_t copied = 0, present = 0, failed = 0;
    for (fs::directory_iterator it{source, ec}, end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        if (HasExtension(it->path(), {".zip"})) {
            // The firmware as it is found: "Firmware 20.5.0.zip". Its .nca files are taken out straight
            // into the registered folder (a file already there with the same size is left alone).
            const Ps4Zip::Result zip = Ps4Zip::ExtractNcas(it->path(), target);
            Ps4::Log("firmware zip %s: %zu extracted, %zu already there, %zu failed%s%s",
                     it->path().filename().string().c_str(), zip.extracted, zip.skipped, zip.failed,
                     zip.first_error.empty() ? "" : ": ", zip.first_error.c_str());
            copied += zip.extracted;
            present += zip.skipped;
            failed += zip.failed;
            continue;
        }
        if (!HasExtension(it->path(), {".nca"})) {
            continue;
        }
        const fs::path to = target / it->path().filename();
        std::error_code e2;
        if (fs::exists(to, e2) && fs::file_size(to, e2) == it->file_size(e2)) {
            ++present;
            continue;
        }
        if (fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, e2)) {
            ++copied;
        } else {
            ++failed;
        }
    }
    Ps4::Log("firmware: %zu NCAs installed now, %zu already there, %zu failed", copied, present, failed);
}

void ApplyPs4Settings() {
    auto& v = Settings::values;
    v.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    v.use_multi_core.SetValue(true);
    // Unsafe (inaccurate NaNs, reduced-precision floating point): the speed setting of v0.1.0.
    // Auto still splits fused multiply-adds, which costs time on a CPU-bound console. settings.txt:
    // cpu_accuracy=auto|accurate brings the accuracy back for a game that misbehaves.
    v.cpu_accuracy.SetValue(Settings::CpuAccuracy::Unsafe);
    // Handheld renders at 720p: less GPU work for a GPU that is much smaller than the PS5's.
    v.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    v.resolution_setup.SetValue(Settings::ResolutionSetup::Res1X);
    v.use_disk_shader_cache.SetValue(true);
    v.use_asynchronous_shaders.SetValue(true);
    v.use_speed_limit.SetValue(true);
    v.sink_id.SetValue(Settings::AudioEngine::Auto);
    v.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_4Gb);
    v.vram_usage_mode.SetValue(Settings::VramUsageMode::Conservative);
    v.renderer_debug = false;
    // The GPU macro JIT takes its code buffer from an anonymous mmap (flexible memory, not known
    // to take PROT_EXEC on the PS4); the macro interpreter needs nothing special.
    v.disable_macro_jit.SetValue(true);
    // MK8D lost the GPU twice while loading a race, both times with ASTC decoded by Eden's compute
    // pass (a large compute shader; compute is the least exercised path of the PS4 driver). On the
    // CPU, synchronously: the asynchronous decode uploaded into images already deleted (crashes in
    // tests 7 and 10, radv_image_queue_family_mask from Image::UploadMemory).
    v.accelerate_astc.SetValue(Settings::AstcDecodeMode::Cpu);
    // Speed (test 22): races ran at 10-27 fps with the GPU never waited on; the processor is the
    // limit. What Eden itself defaults to on Android phones: low GPU accuracy (fewer syncs and
    // readbacks) and no reactive flushing (no GPU->CPU flushes on guest reads).
    v.gpu_accuracy.SetValue(Settings::GpuAccuracy::Low);
    v.use_reactive_flushing.SetValue(false);
    v.external_content_dirs = {(fs::path{Ps4::DataDir} / "updates").string()};

    // The PS4 driver's own log (why it declared the device lost, among others), read when the
    // Vulkan instance is created. settings.txt can override any of these with env=NAME=VALUE.
    const fs::path mesa_log = fs::path{Ps4::DataDir} / "mesa.log";
    std::error_code ec;
    fs::rename(mesa_log, fs::path{Ps4::DataDir} / "mesa.old.log", ec);
    setenv("MESA_LOG_FILE", mesa_log.string().c_str(), 1);
    setenv("MESA_LOG_LEVEL", "info", 1);
    setenv("RADV_DEBUG", "info", 1);
    // The driver's GPU arena: 1024 MiB by default ran out loading an MK8D race (test 20); there
    // was direct memory to spare. The texture and buffer caches size their budgets from this.
    setenv("ORBIS_ARENA_MIB", "1280", 1);
}

/// settings.txt: lines "key=value" ('#' starts a comment). Each recognized line is logged; any
/// other line is logged as unknown and ignored.
///   cpu_accuracy=unsafe|auto|accurate  dynarmic accuracy (default unsafe: faster floating point)
///   astc=gpu|cpu|async          ASTC texture decoding (default cpu)
///   bgra=auto|on|off            B8G8R8A8 red/blue workaround (default auto: startup self-test)
///   a2b10=auto|off              A2B10G10R10 workaround from the startup self-test (default auto)
///   rg=auto|on|off              R8G8 red/green workaround (default auto: startup self-test)
///   swizzle=test|off            run the component-mapping self-test at startup (default off)
///   dyna_state=0..3             Vulkan extended dynamic state level (default 2)
///   vertex_input_dynamic=on|off (default on)
///   gpu_accuracy=low|high       (default low)
///   reactive_flushing=on|off    (default off)
///   profile=on|off              sampling profiler in boot.log (default on)
///   async_shaders=on|off        (default on)
///   mode=handheld|docked        console mode: 720p or 1080p rendering (default handheld)
///   resolution=0.5|0.75|1|1.5|2 internal resolution scale (default 1)
///   speed_limit=on|off          (default on)
///   multicore=on|off            (default on)
///   vram=conservative|aggressive  (default conservative)
///   shader_cache=on|off         shaders kept on disk (default on)
///   shader_preload=0|128|256|1024|all  how much of the disk cache is compiled at start (default 256): the
///                               cache may weigh up to N x 32 KiB, a bigger one is set aside (see shader_policy.h)
///   shader_notice=on|off        the shader counter and the low-memory warning over the game (default on)
///   overlay_delay=0|1|2|4|8     ms the GPU gets to finish a frame before the HUD is drawn into it (default 2)
///   driver_cache=on|off         the driver's own pipeline cache file (default on; off saves memory and disk)
///   overlay=on|off              the video hook: HUD, pause menu, colour swap (default on)
///   overlay_draw=on|off         draw the HUD and the pause menu over the game (default on)
///   hud=on|off                  performance HUD over the game (default off)
///   hud_size=normal|small, hud_position=left|right
///   colour_swap=on|off          swap red and blue on the screen (default on)
///   language=pt|en              the menu's language (read by the menu only)
///   env=NAME=VALUE              environment for the PS4 driver (e.g. env=RADV_DEBUG=info,nohiz)
void ApplySettingsFile() {
    std::ifstream in{fs::path{Ps4::DataDir} / "settings.txt"};
    if (!in) {
        return;
    }
    auto& v = Settings::values;
    for (std::string line; std::getline(in, line);) {
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        line.erase(std::remove(line.begin(), line.end(), ' '), line.end());
        const auto eq = line.find('=');
        if (line.empty() || eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        bool known = true;
        if (key == "cpu_accuracy" && (value == "unsafe" || value == "auto" || value == "accurate")) {
            v.cpu_accuracy.SetValue(value == "unsafe"   ? Settings::CpuAccuracy::Unsafe
                                    : value == "auto" ? Settings::CpuAccuracy::Auto
                                                      : Settings::CpuAccuracy::Accurate);
        } else if (key == "astc" && (value == "gpu" || value == "cpu" || value == "async")) {
            v.accelerate_astc.SetValue(value == "gpu"   ? Settings::AstcDecodeMode::Gpu
                                       : value == "cpu" ? Settings::AstcDecodeMode::Cpu
                                                        : Settings::AstcDecodeMode::CpuAsynchronous);
        } else if (key == "bgra" && (value == "auto" || value == "on" || value == "off")) {
            Vulkan::ps4_bgra_mode = value == "auto" ? 0 : value == "on" ? 1 : 2;
        } else if (key == "profile" && (value == "on" || value == "off")) {
            Ps4::SetProfiling(value == "on");
        } else if (key == "reactive_flushing" && (value == "on" || value == "off")) {
            v.use_reactive_flushing.SetValue(value == "on");
        } else if (key == "a2b10" && (value == "auto" || value == "off")) {
            Vulkan::ps4_a2b10_mode = value == "auto" ? 0 : 2;
        } else if (key == "rg" && (value == "auto" || value == "on" || value == "off")) {
            Vulkan::ps4_rg_mode = value == "auto" ? 0 : value == "on" ? 1 : 2;
        } else if (key == "swizzle" && (value == "test" || value == "off")) {
            Vulkan::ps4_swizzle_mode = value == "test" ? 0 : 1;
        } else if (key == "dyna_state" && value.size() == 1 && value[0] >= '0' && value[0] <= '3') {
            v.dyna_state.SetValue(static_cast<Settings::ExtendedDynamicState>(value[0] - '0'));
        } else if (key == "vertex_input_dynamic" && (value == "on" || value == "off")) {
            v.vertex_input_dynamic_state.SetValue(value == "on");
        } else if (key == "gpu_accuracy" && (value == "low" || value == "high")) {
            v.gpu_accuracy.SetValue(value == "low" ? Settings::GpuAccuracy::Low
                                                   : Settings::GpuAccuracy::High);
        } else if (key == "async_shaders" && (value == "on" || value == "off")) {
            v.use_asynchronous_shaders.SetValue(value == "on");
        } else if (key == "mode" && (value == "handheld" || value == "docked")) {
            v.use_docked_mode.SetValue(value == "docked" ? Settings::ConsoleMode::Docked
                                                         : Settings::ConsoleMode::Handheld);
        } else if (key == "resolution") {
            // By name, not by number: this Eden counts 1/4x first, so the old "Res1X is 2" check
            // never held and every log said "resolution=0.75 (unknown, ignored)".
            using R = Settings::ResolutionSetup;
            if (value == "0.5") {
                v.resolution_setup.SetValue(R::Res1_2X);
            } else if (value == "0.75") {
                v.resolution_setup.SetValue(R::Res3_4X);
            } else if (value == "1") {
                v.resolution_setup.SetValue(R::Res1X);
            } else if (value == "1.5") {
                v.resolution_setup.SetValue(R::Res3_2X);
            } else if (value == "2") {
                v.resolution_setup.SetValue(R::Res2X);
            } else {
                known = false;
            }
        } else if (key == "shader_preload" &&
                   (value == "0" || value == "128" || value == "256" || value == "1024" || value == "all")) {
            Ps4Hook::Pending().shader_preload = value == "all" ? -1 : std::atoi(value.c_str());
        } else if (key == "shader_notice" && (value == "on" || value == "off")) {
            Ps4Hook::Pending().shader_notice = value == "on";
        } else if (key == "overlay_delay" && (value == "0" || value == "1" || value == "2" || value == "4" || value == "8")) {
            Ps4Hook::Pending().draw_delay_ms = std::atoi(value.c_str());
        } else if (key == "driver_cache" && (value == "on" || value == "off")) {
            v.use_vulkan_driver_pipeline_cache.SetValue(value == "on");
        } else if (key == "overlay_draw" && (value == "on" || value == "off")) {
            Ps4Hook::Pending().draw = value == "on";
        } else if (key == "overlay" && (value == "on" || value == "off")) {
            Ps4Hook::Pending().overlay = value == "on";
        } else if (key == "hud" && (value == "on" || value == "off")) {
            Ps4Hook::Pending().hud = value == "on";
        } else if (key == "hud_size" && (value == "normal" || value == "small")) {
            Ps4Hook::Pending().hud_small = value == "small";
        } else if (key == "hud_position" && (value == "left" || value == "right")) {
            Ps4Hook::Pending().hud_right = value == "right";
        } else if (key == "colour_swap" && (value == "on" || value == "off")) {
            Ps4Hook::Pending().swap_colours = value == "on";
        } else if (key == "shader_cache" && (value == "on" || value == "off")) {
            v.use_disk_shader_cache.SetValue(value == "on");
        } else if (key == "speed_limit" && (value == "on" || value == "off")) {
            v.use_speed_limit.SetValue(value == "on");
        } else if (key == "multicore" && (value == "on" || value == "off")) {
            v.use_multi_core.SetValue(value == "on");
        } else if (key == "vram" && (value == "conservative" || value == "aggressive") &&
                   static_cast<int>(Settings::VramUsageMode::Conservative) == 0) {
            v.vram_usage_mode.SetValue(static_cast<Settings::VramUsageMode>(value == "aggressive" ? 1 : 0));
        } else if (key == "language" && (value == "pt" || value == "en")) {
            // Only the menu reads it.
        } else if (key == "env" && value.find('=') != std::string::npos && value.find('=') > 0) {
            // A driver switch, e.g. env=ORBIS_GS_RING_SCALE=4 (read when Vulkan starts).
            const auto split = value.find('=');
            setenv(value.substr(0, split).c_str(), value.c_str() + split + 1, 1);
        } else {
            known = false;
        }
        Ps4::Log("settings.txt: %s=%s%s", key.c_str(), value.c_str(), known ? "" : " (unknown, ignored)");
    }
}

/// DualShock 4 -> the virtual gamepad Eden binds to player 1, by position like a Switch pad.
void PollPad(InputCommon::VirtualGamepad& pad, const Ps4::PadState& s) {
    using VB = InputCommon::VirtualGamepad::VirtualButton;
    struct Map {
        u32 bit;
        VB button;
    };
    static constexpr Map map[] = {
        {Ps4::Button::Circle, VB::ButtonA},     {Ps4::Button::Cross, VB::ButtonB},
        {Ps4::Button::Triangle, VB::ButtonX},   {Ps4::Button::Square, VB::ButtonY},
        {Ps4::Button::L1, VB::TriggerL},        {Ps4::Button::R1, VB::TriggerR},
        {Ps4::Button::L2, VB::TriggerZL},       {Ps4::Button::R2, VB::TriggerZR},
        {Ps4::Button::Options, VB::ButtonPlus}, {Ps4::Button::TouchPad, VB::ButtonMinus},
        {Ps4::Button::L3, VB::StickL},          {Ps4::Button::R3, VB::StickR},
        {Ps4::Button::Up, VB::ButtonUp},        {Ps4::Button::Down, VB::ButtonDown},
        {Ps4::Button::Left, VB::ButtonLeft},    {Ps4::Button::Right, VB::ButtonRight},
    };
    for (const auto& m : map) {
        pad.SetButtonState(0, m.button, (s.buttons & m.bit) != 0);
    }
    const auto axis = [](u8 value) { return std::clamp((static_cast<int>(value) - 128) / 127.0f, -1.0f, 1.0f); };
    // Switch sticks: up is positive.
    pad.SetStickPosition(0, InputCommon::VirtualGamepad::VirtualStick::Left, axis(s.lx), -axis(s.ly));
    pad.SetStickPosition(0, InputCommon::VirtualGamepad::VirtualStick::Right, axis(s.rx), -axis(s.ry));
}

/// Thread-local storage check: an initialised thread_local must read its initial value on the
/// main thread and on a new thread, and keep per-thread writes apart (test 1 crashed on exactly
/// this: Eden's KernelCore thread_local read garbage).
struct TlsProbe {
    u64 a = 0x1122334455667788ULL;
    u8 pad[3000]{};
    u64 b = 0xA5A5A5A5A5A5A5A5ULL;
};
thread_local TlsProbe tls_probe;

bool CheckTls() {
    const bool main_ok = tls_probe.a == 0x1122334455667788ULL && tls_probe.b == 0xA5A5A5A5A5A5A5A5ULL;
    tls_probe.a = 1;
    bool worker_ok = false;
    std::thread{[&] {
        worker_ok = tls_probe.a == 0x1122334455667788ULL && tls_probe.b == 0xA5A5A5A5A5A5A5A5ULL;
        tls_probe.a = 2;
    }}.join();
    const bool kept = tls_probe.a == 1;
    Ps4::Log("TLS check: main %s, new thread %s, per-thread values %s", main_ok ? "ok" : "BAD",
             worker_ok ? "ok" : "BAD", kept ? "ok" : "BAD");
    return main_ok && worker_ok && kept;
}

/// Mutual exclusion under contention, for std::mutex and std::recursive_mutex as this libc++ and
/// the console's pthreads build them. The texture cache (guarded by a recursive_mutex) goes
/// inconsistent when the nvservices thread unmaps GPU memory while the GPU thread draws (MK8D).
extern "C" int EdenPs4FlatMapCheck(char* report, int report_size); // tests/flat_map_check.cpp

void CheckMutexes() {
    constexpr int Iterations = 400000;
    const auto run = [](auto& mutex, bool nested) {
        volatile u64 counter = 0;
        const auto worker = [&] {
            for (int i = 0; i < Iterations; ++i) {
                std::lock_guard lock{mutex};
                if constexpr (requires { mutex.try_lock(); }) {
                    if (nested) {
                        // recursive_mutex only: the owner can take it again.
                        std::lock_guard inner{mutex};
                        counter = counter + 1;
                        continue;
                    }
                }
                counter = counter + 1;
            }
        };
        std::thread a{worker};
        std::thread b{worker};
        std::thread c{worker};
        a.join();
        b.join();
        c.join();
        return u64(counter);
    };
    std::mutex plain;
    std::recursive_mutex recursive;
    const u64 plain_count = run(plain, false);
    const u64 recursive_count = run(recursive, true);
    const u64 expected = u64(Iterations) * 3;
    Ps4::Log("mutex check: std::mutex %llu/%llu %s, std::recursive_mutex (nested) %llu/%llu %s",
             static_cast<unsigned long long>(plain_count), static_cast<unsigned long long>(expected),
             plain_count == expected ? "ok" : "BROKEN",
             static_cast<unsigned long long>(recursive_count),
             static_cast<unsigned long long>(expected),
             recursive_count == expected ? "ok" : "BROKEN");
}

/// Eden's log file is written through a buffer and the last lines die with the app (the eden_log.txt
/// of a 253 s session ended at 158 s). With this setting every line is flushed. Detected at compile
/// time: an Eden without the setting just keeps the old behaviour and says so in boot.log.
template <class V>
void KeepEdenLogComplete(V& values) {
    if constexpr (requires { values.log_flush_line.SetValue(true); }) {
        values.log_flush_line.SetValue(true);
        Ps4::Log("eden_log.txt: every line is flushed (log_flush_line)");
    } else {
        Ps4::Log("eden_log.txt: this Eden has no log_flush_line, its last lines are lost if the app dies");
    }
}

bool MenuInPortuguese() {
    std::ifstream in{fs::path{Ps4::DataDir} / "settings.txt", std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    return Menu::SettingsFile(text.str()).Language() == Menu::Lang::Pt;
}

} // Anonymous namespace

/// The system asks the app to close (the PS button's "Close Application"): say so in the log and leave
/// at once, without running destructors (Eden's teardown crashes here, see the end of main()).
extern "C" void OnCloseRequest(int signal_number) {
    Ps4::Log("!! the system asked the app to close (signal %d); leaving now", signal_number);
    _exit(0);
}

int main() {
    for (const int sig : {SIGTERM, SIGHUP, SIGINT}) {
        std::signal(sig, OnCloseRequest);
    }

    Ps4::OpenBootLog();
    Ps4::Log("eden-ps4 starting (Eden %s %s)", Common::g_scm_branch, Common::g_scm_desc);
    Ps4::Log("PS4 build: %s (btver2 build; defaults: cpu_accuracy unsafe, gpu_accuracy low, no reactive flushing, CPU ASTC, profiler on)",
             EDEN_PS4_BUILD_ID);
    Ps4::InstallCrashReporting();
    Ps4::StartWatchdog();
    Ps4::RegisterThread("main");
    Ps4::LoadSystemModules();
    Ps4::HideSplashScreen();
    CheckTls();
    CheckMutexes();
    {
        // std::find over 4-byte elements goes through wmemchr (libc_wide_fixes.cpp).
        const std::vector<u32> values{1, 3, 7, 0x10000, 42};
        const auto at = [&](u32 v) {
            return static_cast<long>(std::find(values.begin(), values.end(), v) - values.begin());
        };
        const bool find_ok = at(1) == 0 && at(3) == 1 && at(7) == 2 && at(0x10000) == 3 &&
                             at(42) == 4 && at(5) == 5;
        Ps4::Log("find check: 1->%ld 3->%ld 7->%ld 0x10000->%ld 42->%ld 5->%ld %s", at(1), at(3),
                 at(7), at(0x10000), at(42), at(5), find_ok ? "ok" : "BROKEN");
    }
    {
        char report[256];
        const int missing = EdenPs4FlatMapCheck(report, sizeof(report));
        Ps4::Log("flat_map check: %s %s", report, missing == 0 ? "ok" : "BROKEN");
    }

    std::error_code ec;
    // The folders the user fills (keys, firmware, roms, updates) and the ones Eden writes in: all
    // made now, so they are there to see over FTP from the first run. (The process has no umask,
    // see Ps4::PrepareDataDir: they open to everyone.)
    for (const char* sub : {"keys", "firmware", "roms", "updates", "log", "nand", "sdmc", "play_time",
                            "shader", "cache", "screenshots", "dump", "load"}) {
        fs::create_directories(fs::path{Ps4::DataDir} / sub, ec);
        if (ec) {
            Ps4::Log("could not create %s/%s: %s", Ps4::DataDir, sub, ec.message().c_str());
        }
    }
    if (const fs::path readme = fs::path{Ps4::DataDir} / "LEIA-ME.txt"; !fs::exists(readme, ec)) {
        if (std::ofstream out{readme}; out) {
            out << "NX on Orbis - pasta de dados / data folder\n\n"
                   "keys/       coloque aqui prod.keys (e title.keys), tirados do SEU Switch\n"
                   "            put prod.keys (and title.keys) here, dumped from YOUR Switch\n"
                   "firmware/   os arquivos .nca do firmware do SEU Switch\n"
                   "            the .nca files of YOUR Switch's firmware\n"
                   "roms/       seus jogos .nsp ou .xci / your .nsp or .xci games\n"
                   "updates/    atualizacoes e DLC (.nsp) / updates and DLC (.nsp)\n"
                   "nand/ sdmc/ saves e dados do emulador / saves and the emulator's data\n"
                   "settings.txt  as opcoes do menu / the menu's options\n"
                   "boot.log, mesa.log, log/eden_log.txt  registros / logs\n";
        }
    }
    {
        std::error_code size_ec;
        const auto bytes = fs::file_size(fs::path{Ps4::DataDir} / "settings.txt", size_ec);
        Ps4::Log("settings.txt: %s", size_ec ? "none yet" : (std::to_string(bytes) + " bytes").c_str());
    }

    Ps4::SetPhase("logging");
    KeepEdenLogComplete(Settings::values);
    Common::Log::Initialize();
    Common::Log::Start();
    LOG_INFO(Frontend, "eden-ps4 frontend");

    const bool pad_ok = Ps4::OpenPad();
    Ps4::Log("controller %s", pad_ok ? "open" : "NOT available");

    const std::string game = FindGame();
    if (game.empty()) {
        Ps4::Log("no game: put an .nsp or .xci in %s/roms (or its path in game.txt)", Ps4::DataDir);
        Ps4::SetPadLight(255, 0, 0);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }
    Ps4::Log("game: %s", game.c_str());
    if (!fs::exists(fs::path{Ps4::DataDir} / "keys/prod.keys", ec)) {
        Ps4::Log("missing %s/keys/prod.keys - dump it from your Switch", Ps4::DataDir);
    }
    Ps4::SetPhase("firmware");
    InstallFirmware();

    Ps4::SetPhase("core init");
    ApplyPs4Settings();
    ApplySettingsFile();
    Core::System system;
    system.Initialize();
    InputCommon::InputSubsystem input;
    input.Initialize();
    system.ApplySettings();
    Ps4Window window;

    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();

    // The game's own contents, registered as Eden's desktop frontends do, so an update's data
    // applies on top of the base game.
    FileSys::ManualContentProvider game_contents;
    if (!game_contents.AddEntriesFromContainer(system.GetFilesystem()->OpenFile(game, FileSys::OpenMode::Read))) {
        Ps4::Log("game contents not registered (an update's data will not apply)");
    }
    system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &game_contents);

    std::mutex exit_mutex;
    std::condition_variable exit_cv;
    bool exited = false;
    system.RegisterExitCallback([&] {
        std::lock_guard lock{exit_mutex};
        exited = true;
        exit_cv.notify_all();
    });

    Ps4::SetPhase("loading game");
    Service::AM::FrontendAppletParameters params{.applet_id = Service::AM::AppletId::Application};
    // From here the video buffers the game registers are known to the hook (HUD, pause menu, colours).
    if (Ps4Hook::Pending().overlay) {
        Ps4Hook::Arm(Ps4Hook::Pending());
    } else {
        Ps4::Log("overlay off: no HUD, no pause menu, no colour swap");
    }
    const Core::SystemResultStatus loaded = system.Load(window, game, params);
    if (loaded != Core::SystemResultStatus::Success) {
        const unsigned code = static_cast<unsigned>(loaded);
        Ps4::Log("load failed: status %u%s", code,
                 loaded == Core::SystemResultStatus::ErrorVideoCore ? " (video core: see the RADV lines above)" : "");
        if (code > static_cast<unsigned>(Core::SystemResultStatus::ErrorLoader)) {
            const auto loader = static_cast<Loader::ResultStatus>(code - static_cast<unsigned>(Core::SystemResultStatus::ErrorLoader));
            Ps4::Log("loader: %s", Loader::GetResultStatusString(loader).c_str());
        }
        Ps4::SetPadLight(255, 0, 0);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 2;
    }

    Ps4::SetPhase("starting GPU");
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        Ps4::SetPhase("shader cache");
        // Eden compiles EVERY pipeline of the game's disk cache before the game starts, and each one
        // stays in the driver's memory. A cache grown over many sessions (2185 pipelines in one log)
        // used the whole arena and the app died with the game not yet started. Until 0.7.2 this was
        // limited by stopping Eden's load through its stop token, but Eden answers a stop by ending
        // every thread of its pipeline worker pool (LoadDiskResources -> WaitForRequests), so after
        // a capped preload nothing was left to build the shaders the game asked for. Now the size of
        // the cache is limited BEFORE Eden reads it (shader_preload, see shader_policy.h) and Eden
        // always reads what is left to the end, with its pool intact.
        // A marker file records that the load is under way: if the app dies in it, the next start
        // finds the marker, puts that game's cache aside and starts with an empty one.
        const std::uint64_t title_id = system.GetApplicationProcessProgramID();
        char id[17];
        std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(title_id));
        const fs::path shader_root = fs::path{Ps4::DataDir} / "cache/shader";
        const fs::path marker = shader_root / (std::string(id) + ".loading");
        std::error_code cache_ec;
        fs::create_directories(shader_root, cache_ec);
        if (fs::exists(marker, cache_ec)) {
            const fs::path aside = shader_root / (std::string(id) + ".bad");
            fs::remove_all(aside, cache_ec);
            fs::rename(shader_root / id, aside, cache_ec);
            fs::remove(marker, cache_ec);
            Ps4::Log("the last start died while loading the shader cache of %s: it was moved to %s.bad; this game starts with an empty cache",
                     id, id);
        }
        const int cap = Ps4Hook::Pending().shader_preload;
        {
            const Ps4Shader::CacheCheck check = Ps4Shader::LimitCache(shader_root / id, cap);
            const double size_mib = double(check.size) / (1024.0 * 1024.0);
            const double limit_mib = double(check.limit) / (1024.0 * 1024.0);
            switch (check.action) {
            case Ps4Shader::CacheAction::NoFile:
                Ps4::Log("shader cache: no vulkan.bin yet (a new one starts)");
                break;
            case Ps4Shader::CacheAction::Kept:
                if (check.unlimited) {
                    Ps4::Log("shader cache: vulkan.bin is %.1f MiB, no limit (shader_preload=all)", size_mib);
                } else {
                    Ps4::Log("shader cache: vulkan.bin is %.1f MiB, within the %.1f MiB limit (shader_preload=%d)", size_mib,
                             limit_mib, cap);
                }
                break;
            case Ps4Shader::CacheAction::SetAside:
                Ps4::Log("shader cache: vulkan.bin is %.1f MiB, over the %.1f MiB limit (shader_preload=%d): set aside as vulkan.bin.bak, a new cache starts",
                         size_mib, limit_mib, cap);
                break;
            case Ps4Shader::CacheAction::Failed:
                Ps4::Log("shader cache: vulkan.bin is %.1f MiB, over the %.1f MiB limit but it could not be moved; loading it anyway",
                         size_mib, limit_mib);
                break;
            }
        }
        {
            std::ofstream out{marker};
            out << "loading\n";
        }
        // Never stopped: see above.
        const std::stop_source never_stopped;
        std::atomic<size_t> loaded{0}, total{0}, logged{0};
        Ps4::Log("shader cache: loading");
        system.Renderer().ReadRasterizer()->LoadDiskResources(
            title_id, never_stopped.get_token(), [&](VideoCore::LoadCallbackStage, size_t value, size_t count) {
                if (count != 0) {
                    total = count;
                }
                if (value != 0) {
                    loaded = value;
                    // Called from Eden's pipeline threads: one line per 64 pipelines, and the last one.
                    if (value >= logged + 64 || value == total) {
                        logged = value;
                        Ps4::Log("shader cache: %zu of %zu pipelines built (free direct memory %lu MiB)", value, total.load(),
                                 static_cast<unsigned long>(Ps4::FreeDirectMemory() >> 20));
                    }
                }
            });
        fs::remove(marker, cache_ec);
        Ps4::Log("shader cache: %zu pipelines loaded", loaded.load());
    }

    Ps4::SetPhase("running");
    Ps4::SetPadLight(0, 80, 255);
    Ps4::ArmHangWatch(15);
    system.Run();

    // Controller in, status out, until the game exits or the player leaves from the pause menu.
    auto* gamepad = input.GetVirtualGamepad();
    std::uint64_t quit_since = 0;
    const bool pt = MenuInPortuguese();
    // The pause menu (L3 + R3), drawn over the frozen game by the video hook.
    bool in_menu = false, combo_before = false, overlay_warned = false;
    int menu_sel = 0;
    std::uint32_t menu_buttons_before = 0;
    bool notice_on = Ps4Hook::Pending().shader_notice;
    constexpr int MenuRows = 5; // Continue, HUD, speed limit, shader notice, back to the game list
    const auto show_menu = [&] {
        const auto state = [&](bool on) { return std::string(on ? (pt ? "ligado" : "on") : (pt ? "desligado" : "off")); };
        const std::vector<std::string> lines = {
            pt ? "Continuar" : "Continue",
            std::string(pt ? "HUD de desempenho: " : "Performance HUD: ") + state(Ps4Hook::HudOn()),
            std::string(pt ? "Limite de velocidade: " : "Speed limit: ") + state(Settings::values.use_speed_limit.GetValue()),
            std::string(pt ? "Aviso de shaders e memoria: " : "Shader and memory notice: ") + state(notice_on),
            pt ? "Sair do jogo (volta ao menu de jogos)" : "Exit game (back to the game list)"};
        Ps4Hook::ShowPanel(pt ? "Menu do emulador" : "Emulator menu", lines, menu_sel,
                           pt ? "Cima/Baixo: escolher   X: confirmar   O: voltar" : "Up/Down: choose   X: confirm   O: back");
    };
    std::uint64_t last_status = Ps4::NowUs(), last_notice = 0;
    unsigned status_count = 0, notice_ticks = 0;
    int shaders_last = 0, shaders_total = 0;
    std::uint64_t free_mib = Ps4::FreeDirectMemory() >> 20;
    const char* leave_reason = "the game exited";
    for (;;) {
        {
            std::unique_lock lock{exit_mutex};
            if (exit_cv.wait_for(lock, std::chrono::milliseconds(8), [&] { return exited; })) {
                Ps4::Log("the game exited");
                break;
            }
        }
        const Ps4::PadState pad = Ps4::ReadPad();
        const bool menu_combo = (pad.buttons & Ps4::Button::L3) && (pad.buttons & Ps4::Button::R3);
        if (in_menu) {
            const std::uint32_t pressed = pad.buttons & ~menu_buttons_before;
            menu_buttons_before = pad.buttons;
            bool redraw = false, close = (pressed & Ps4::Button::Circle) != 0 || (menu_combo && !combo_before), leave = false;
            combo_before = menu_combo;
            if (pressed & Ps4::Button::Down) {
                menu_sel = (menu_sel + 1) % MenuRows;
                redraw = true;
            }
            if (pressed & Ps4::Button::Up) {
                menu_sel = (menu_sel + MenuRows - 1) % MenuRows;
                redraw = true;
            }
            if (pressed & Ps4::Button::Cross) {
                if (menu_sel == 0) {
                    close = true;
                } else if (menu_sel == 1) {
                    Ps4Hook::SetHud(!Ps4Hook::HudOn());
                    redraw = true;
                } else if (menu_sel == 2) {
                    Settings::values.use_speed_limit.SetValue(!Settings::values.use_speed_limit.GetValue());
                    redraw = true;
                } else if (menu_sel == 3) {
                    notice_on = !notice_on;
                    if (!notice_on) {
                        Ps4Hook::SetNotice({});
                    }
                    redraw = true;
                } else {
                    leave = true;
                }
            }
            if (leave) {
                Ps4::Log("exit chosen in the pause menu");
                leave_reason = "exit chosen in the pause menu";
                break;
            }
            if (close) {
                in_menu = false;
                Ps4Hook::HidePanel();
                system.Run();
                Ps4::Log("pause menu closed, the game runs again");
            } else if (redraw) {
                show_menu();
            }
            continue;
        }
        if (menu_combo && !combo_before) {
            if (Ps4Hook::Ready()) {
                in_menu = true;
                menu_sel = 0;
                menu_buttons_before = pad.buttons;
                system.Pause();
                Ps4::Log("pause menu opened (L3 + R3)");
                show_menu();
            } else if (!overlay_warned) {
                overlay_warned = true;
                Ps4::Log("pause menu not available: %s", Ps4Hook::Describe().c_str());
            }
        }
        combo_before = menu_combo;
        if (pad.connected && gamepad != nullptr) {
            PollPad(*gamepad, pad);
        }
        const bool quit_combo = (pad.buttons & Ps4::Button::Options) && (pad.buttons & Ps4::Button::TouchPad);
        if (!quit_combo) {
            quit_since = 0;
        } else if (quit_since == 0) {
            quit_since = Ps4::NowUs();
        } else if (Ps4::NowUs() - quit_since > 2000000) {
            Ps4::Log("quit combo held");
            leave_reason = "quit combo held";
            break;
        }
        // The shader counter (Cemu's "compiling shaders"): what Eden's own notifier says is being
        // built, kept as a running total, and a warning when direct memory runs low. Every 250 ms;
        // the memory is asked for once a second.
        if (notice_on && Ps4::NowUs() - last_notice > 250000) {
            last_notice = Ps4::NowUs();
            Ps4Shader::CountBuilding(system.GPU().ShaderNotify().ShadersBuilding(), &shaders_last, &shaders_total);
            if (notice_ticks++ % 4 == 0) {
                free_mib = Ps4::FreeDirectMemory() >> 20;
            }
            Ps4Hook::SetNotice(Ps4Shader::NoticeText(shaders_last, shaders_total, free_mib, pt));
        }
        if (Ps4::NowUs() - last_status > 10000000) {
            last_status = Ps4::NowUs();
            const auto perf = system.GetAndResetPerfStats();
            const auto mem = Common::Orbis::GetStats();
            Ps4::Log("status: game %.1f fps, speed %.0f%%, frame %.1f ms, guest+tables %lu MiB committed (%lu MiB given back), largest free direct block %lu MiB, shaders built %d",
                     perf.average_game_fps, perf.emulation_speed * 100.0, perf.frametime * 1000.0,
                     static_cast<unsigned long>(mem.committed_bytes >> 20),
                     static_cast<unsigned long>(mem.decommitted_bytes >> 20),
                     static_cast<unsigned long>(Ps4::FreeDirectMemory() >> 20), shaders_total);
            if (++status_count % 3 == 1) {
                Ps4HeapCensus();
                Ps4::Log("%s", Ps4Hook::Describe().c_str());
            }
        }
    }

    // Eden cannot be torn down and started again inside one run (test 2 showed its shutdown crashing
    // in a worker thread, and the game's memory is not given back), so leaving the game means
    // starting the app again: a new process opens on the game list. The saves are written by the
    // game itself; the logs are flushed line by line.
    Ps4::SetPhase("leaving");
    Ps4::ArmHangWatch(0);
    system.Pause();
    {
        const auto mem = Common::Orbis::GetStats();
        Ps4::Log("leaving the game (%s): lazy memory peak %lu MiB, %d shaders built", leave_reason,
                 static_cast<unsigned long>(mem.committed_bytes >> 20), shaders_total);
    }
    Ps4Hook::ShowPanel(pt ? "Menu do emulador" : "Emulator menu",
                       {pt ? "Voltando ao menu de jogos..." : "Going back to the game list..."}, -1, "");
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); // a frame with the message
    if (Ps4::RestartApp()) {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1)); // the system is replacing the process
        }
    }
    Ps4::SetPhase("finished");
    Ps4Hook::ShowPanel(pt ? "Nao foi possivel voltar ao menu" : "Could not go back to the game list",
                       {pt ? "Feche o app com o botao PS e abra de novo." : "Close the app with the PS button and open it again."},
                       -1, "boot.log: restart");
    Ps4::Log("finished: close the app with the PS button");
    Ps4::SetPadLight(0, 255, 0);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
