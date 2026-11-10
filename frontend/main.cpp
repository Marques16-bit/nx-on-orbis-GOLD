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
//   safe_start.txt                   exists while a started game has not drawn yet; a start that finds it
//                                    runs without the experimental lines of settings.txt (safe_mode.h)
//
// Controls (Switch layout by position): Circle=A Cross=B Triangle=X Square=Y, L1/R1=L/R,
// L2/R2=ZL/ZR, Options=+, touch pad click=-, L3/R3 sticks. Options + touch pad for 2 s quits.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/orbis_lazy_memory.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
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

#include "eden_settings.h"
#include "firmware_zip.h"
#include "memory_guard.h"
#include "safe_mode.h"
#include "menu/cover_loader.h"
#include "menu/menu_ui.h"
#include "screenshot.h"
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

/// settings.txt as the menu reads it (empty when there is none).
Menu::SettingsFile LoadSettingsFile() {
    std::ifstream in{fs::path{Ps4::DataDir} / "settings.txt", std::ios::binary};
    std::ostringstream text;
    text << in.rdbuf();
    return Menu::SettingsFile(text.str());
}

/// What the menu's SYSTEM tab shows: why a game may not start (keys, firmware) and where things are.
Menu::SystemInfo CollectSystemInfo() {
    Menu::SystemInfo info;
    {
        // 0.7.6: the memory this console gives the process and what the memory profile does with it.
        const Menu::SettingsFile file = LoadSettingsFile();
        const bool pt = file.Language() == Menu::Lang::Pt;
        Ps4Memory::Profile profile = Ps4Memory::Profile::Auto;
        const std::string profile_text = file.Get("memory_profile");
        if (profile_text.empty() || !Ps4Memory::ParseProfile(profile_text, &profile)) {
            profile = Ps4Memory::Profile::Auto;
        }
        const std::uint64_t budget_mib = Ps4::DirectMemoryTotal() >> 20;
        const Ps4Memory::Tier detected = Ps4Memory::TierForBudget(budget_mib);
        const Ps4Memory::Plan plan = Ps4Memory::PlanFor(profile, detected);
        const char* const tier_names_pt[] = {"folgado", "padrao", "apertado"};
        const char* const tier_names_en[] = {"roomy", "standard", "tight"};
        const int tier = static_cast<int>(detected);
        info.memory_budget_mib = budget_mib;
        info.memory_tier = (pt ? tier_names_pt : tier_names_en)[tier];
        char plan_text[120];
        std::snprintf(plan_text, sizeof(plan_text), pt ? "%s: arena %d MiB, shaders %d, cache do driver %s" : "%s: arena %d MiB, shaders %d, driver cache %s",
                      profile_text.empty() ? "auto" : profile_text.c_str(), plan.arena_mib, plan.shader_preload,
                      plan.driver_cache ? (pt ? "sim" : "on") : (pt ? "nao" : "off"));
        info.memory_plan = plan_text;
    }
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

/// 0.7.10: the picture inside a game dump, for the menu's cover. The control NCA of an .nsp / .xci holds a RomFS
/// with `icon_<language>.dat` (a 256 x 256 JPEG); it is found and decrypted with the user's keys through Eden's
/// own NSP / XCI / NCA classes, with no emulator core running. Empty on any failure (`why` says what).
std::vector<std::uint8_t> ReadGameIcon(const fs::path& path, std::string* why) {
    std::vector<std::uint8_t> bytes;
    const auto fail = [&](const char* text) {
        if (why != nullptr) {
            *why = text;
        }
        return std::vector<std::uint8_t>{};
    };
    try {
        const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
        const FileSys::VirtualFile file = vfs->OpenFile(path.string(), FileSys::OpenMode::Read);
        if (file == nullptr) {
            return fail("cannot open the file");
        }
        std::string ext = path.extension().string();
        for (char& ch : ext) {
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        }
        std::shared_ptr<FileSys::NCA> control;
        if (ext == ".xci") {
            const FileSys::XCI xci{file};
            if (xci.GetStatus() != Loader::ResultStatus::Success) {
                return fail("not a readable .xci");
            }
            control = xci.GetNCAByType(FileSys::NCAContentType::Control);
        } else {
            const FileSys::NSP nsp{file};
            if (nsp.GetStatus() != Loader::ResultStatus::Success) {
                return fail("not a readable .nsp");
            }
            control = nsp.GetNCA(FileSys::GetBaseTitleID(nsp.GetProgramTitleID()), FileSys::ContentRecordType::Control);
        }
        if (control == nullptr || control->GetStatus() != Loader::ResultStatus::Success) {
            return fail("no control data (the keys cannot open it?)");
        }
        const FileSys::VirtualFile romfs = control->GetRomFS();
        if (romfs == nullptr) {
            return fail("the control data has no RomFS");
        }
        const FileSys::VirtualDir contents = FileSys::ExtractRomFS(romfs);
        if (contents == nullptr) {
            return fail("the control RomFS cannot be read");
        }
        FileSys::VirtualFile best;
        for (const FileSys::VirtualFile& entry : contents->GetFiles()) {
            const std::string name = entry == nullptr ? std::string{} : entry->GetName();
            if (name.size() < 9 || name.compare(0, 5, "icon_") != 0 || name.compare(name.size() - 4, 4, ".dat") != 0) {
                continue;
            }
            if (best == nullptr || name == "icon_AmericanEnglish.dat") {
                best = entry;
            }
        }
        if (best == nullptr) {
            return fail("the control data has no icon");
        }
        bytes = best->ReadAllBytes();
        if (bytes.size() < 16 || bytes.size() > (4u << 20)) {
            return fail("the icon has an odd size");
        }
    } catch (...) {
        return fail("an exception while reading the dump");
    }
    return bytes;
}

/// The icon of a dump, read once and kept in cache/icons/ (named by the dump's file name and size), so the next
/// start of the menu does not open every game again.
std::vector<std::uint8_t> CachedGameIcon(const fs::path& game, std::string* why, bool* from_cache) {
    std::error_code ec;
    std::uint64_t hash = 1469598103934665603ull;
    for (const char ch : game.filename().string()) {
        hash = (hash ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    }
    hash = (hash ^ static_cast<std::uint64_t>(fs::file_size(game, ec))) * 1099511628211ull;
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.jpg", static_cast<unsigned long long>(hash));
    const fs::path cached = fs::path{Ps4::DataDir} / "cache" / "icons" / name;
    if (std::ifstream in{cached, std::ios::binary}; in) {
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() >= 16) {
            *from_cache = true;
            return bytes;
        }
    }
    std::vector<std::uint8_t> bytes = ReadGameIcon(game, why);
    if (!bytes.empty()) {
        fs::create_directories(cached.parent_path(), ec);
        if (std::ofstream out{cached, std::ios::binary}; out) {
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
    }
    return bytes;
}

std::string FindGame() {
    const fs::path data{Ps4::DataDir};
    std::error_code ec;
    // 0.7.7: "Restart game" in the L3 + R3 menu leaves the game's path here and starts the app again; this
    // new process opens that game without the list. The file is removed before it is used, so a start
    // that dies cannot loop.
    if (const fs::path again = data / "restart_game.txt"; fs::exists(again, ec)) {
        std::string line;
        if (std::ifstream in{again}; in) {
            std::getline(in, line);
        }
        fs::remove(again, ec);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty() && fs::is_regular_file(line, ec)) {
            Ps4::Log("restart_game.txt: starting %s again, without the game list", line.c_str());
            return line;
        }
        Ps4::Log("restart_game.txt names '%s', which is not a file: the game list opens", line.c_str());
    }
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
        // 0.8.2: covers=full|thumbs|off. The decoded pictures sit in the C heap while the menu is open; thumbs keeps only
        // the small tiles (no 400x400 pictures, up to 64 MiB less), off decodes nothing. Read when the app starts.
        const std::string covers_mode = LoadSettingsFile().Get("covers");
        const std::uint64_t cover_cap = covers_mode == "thumbs" ? 0 : (64ull << 20);
        if (covers_mode == "off") {
            Ps4::Log("covers: off (settings.txt covers=off): no picture is decoded for the menu");
        }
        if (covers_mode != "off") {
            // 0.7.9: pictures the user put in covers/ (<title id>.png, <file name>.jpg, ...) come first.
            const fs::path covers_dir = data / "covers";
            Menu::Covers::Options cover_options;
            cover_options.max_cover_bytes = cover_cap;
            const Menu::Covers::Report covers = Menu::Covers::Load(covers_dir, entries, cover_options);
            if (!covers.dir_found) {
                std::error_code mk_ec;
                fs::create_directories(covers_dir, mk_ec);
                Ps4::Log("covers: no covers/ folder yet (%s); made one. Put PNG, JPG or BMP pictures there, named by title id or game file name", covers_dir.string().c_str());
            } else {
                Ps4::Log("covers: %d picture(s) in covers/, %d of %zu game(s) got one, %d failed%s%s, %d without the big picture (memory cap), %d not tried (time); %lu MiB, %lld ms",
                         covers.dir_pictures, covers.found, entries.size(), covers.failed, covers.first_error.empty() ? "" : " (first: ",
                         covers.first_error.empty() ? "" : (covers.first_error + ")").c_str(), covers.thumbs_only, covers.skipped_time,
                         static_cast<unsigned long>(covers.cover_bytes >> 20), covers.ms);
            }
        }
        if (covers_mode != "off") {
            // 0.7.10: the games the covers/ folder did not cover get the picture from inside the dump.
            using Clock = std::chrono::steady_clock;
            const auto began = Clock::now();
            std::uint64_t kept = 0;
            for (const Menu::GameEntry& entry : entries) {
                kept += std::uint64_t(entry.cover.px.size()) * 4;
            }
            int got = 0, cached = 0, failed = 0, skipped = 0;
            for (size_t i = 0; i < entries.size() && i < games.size(); ++i) {
                if (entries[i].cover.Ok() || entries[i].thumb.Ok()) {
                    continue;
                }
                if (Clock::now() - began > std::chrono::seconds(12)) {
                    ++skipped;
                    continue;
                }
                std::string why = "no picture";
                bool from_cache = false;
                const std::vector<std::uint8_t> bytes = CachedGameIcon(games[i], &why, &from_cache);
                Menu::Image::Pixels full;
                if (bytes.empty() || !Menu::Image::Decode(bytes.data(), bytes.size(), &full, &why)) {
                    if (++failed <= 5) {
                        Ps4::Log("cover of %s: %s", games[i].filename().string().c_str(), why.c_str());
                    }
                    continue;
                }
                Menu::Image::Pixels big = Menu::Image::Fit(full, Menu::CoverSize, Menu::CoverSize, true);
                entries[i].thumb = Menu::Image::Fit(big.Ok() ? big : full, Menu::ThumbSize, Menu::ThumbSize);
                const std::uint64_t cost = std::uint64_t(big.px.size()) * 4;
                if (big.Ok() && kept + cost <= cover_cap) {
                    entries[i].cover = std::move(big);
                    kept += cost;
                }
                ++got;
                cached += from_cache ? 1 : 0;
            }
            Ps4::Log("covers from the games themselves: %d read (%d from cache/icons), %d failed, %d not tried (time), %lld ms",
                     got, cached, failed, skipped,
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count()));
        }
        const MenuOutcome outcome = RunMainMenu(std::move(entries), int(index), CollectSystemInfo());
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
    // 0.7.7 fix: off, as Eden itself defaults. With it on, a draw whose pipeline is not built yet is SKIPPED
    // (PipelineCache::BuiltPipeline returns null and RasterizerVulkan::PrepareDraw returns), which shows up as
    // missing layers in a game such as Super Mario Maker 2 while its shaders compile. A first run stalls
    // instead; the disk cache (preloaded at start) removes most of the stalls from the next runs.
    v.use_asynchronous_shaders.SetValue(false);
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
    // The driver's GPU arena (ORBIS_ARENA_MIB) is set by ApplyMemoryPlan() from what this console
    // gives the process: 1280 MiB on the PS4 Pro that was measured (1024 ran out loading an MK8D
    // race, test 20, with direct memory to spare), less on a console with less memory.
}

/// What to do when the memory watch reaches its critical level (settings.txt: memory_action).
// 0.8.4: pause is the default. A Super Mario Odyssey run fell from 1790 MiB to 58 MiB free and died 7 s after the
// critical level; pausing there turns that crash into a choice (continue, restart, back to the list).
Ps4Memory::Action g_memory_action = Ps4Memory::Action::Pause;
/// settings.txt: sysflex_mib - how much system flexible memory the guest RAM may use (0 = off). 0.9.1: the default is
/// "auto" (Ps4Memory::SystemFlexibleAutoCapMiB), no longer off; the pool itself still limits it (see ConfigureSystemFlexible).
std::uint64_t g_sysflex_cap_mib = Ps4Memory::SystemFlexibleAutoCapMiB;
/// 0.9.1, settings.txt: players=1|2|3|4 - how many controllers the game is given (default 1, exactly as before).
int g_players = 1;
// 0.8.0: when the picture freezes for a second or more, three screenshots are taken as it recovers (autoshot=off in
// settings.txt stops it). At most three freezes per run, so a game that stutters all the time is not paused by it.
bool g_autoshot = true;
// 0.8.2: the last start died before the game drew anything, so the experimental lines of settings.txt are ignored for this
// one start (safe_mode.h). Set in main() before the memory plan and settings.txt are read.
bool g_safe_mode = false;
std::string SafeStartFile() {
    return (fs::path{Ps4::DataDir} / "safe_start.txt").string();
}

/// 0.7.6: the memory-related defaults for THIS console, set before settings.txt is applied (so a
/// line in settings.txt still wins). Everything here equals the 0.7.3 / 0.7.4 values on a console
/// with as much direct memory as the PS4 Pro that was measured; only a console with less gets less.
void ApplyMemoryPlan() {
    const Menu::SettingsFile file = LoadSettingsFile();

    Ps4Memory::Profile profile = Ps4Memory::Profile::Auto;
    const std::string profile_text = file.Get("memory_profile");
    if (!profile_text.empty() && !Ps4Memory::ParseProfile(profile_text, &profile)) {
        Ps4::Log("settings.txt: memory_profile=%s (unknown, auto is used)", profile_text.c_str());
    }
    if (g_safe_mode && profile == Ps4Memory::Profile::Strict) {
        Ps4::Log("safe mode: memory_profile=strict is experimental, the tight profile is used for this start");
        profile = Ps4Memory::Profile::Tight;
    }
    const std::string action_text = file.Get("memory_action");
    if (!action_text.empty() && !Ps4Memory::ParseAction(action_text, &g_memory_action)) {
        Ps4::Log("settings.txt: memory_action=%s (unknown, pause is used)", action_text.c_str());
    }

    const std::uint64_t budget_mib = Ps4::DirectMemoryTotal() >> 20;
    const Ps4Memory::Tier detected = Ps4Memory::TierForBudget(budget_mib);
    const Ps4Memory::Plan plan = Ps4Memory::PlanFor(profile, detected);
    setenv("ORBIS_ARENA_MIB", std::to_string(plan.arena_mib).c_str(), 1);
    Ps4Hook::Pending().shader_preload = plan.shader_preload;
    if (!plan.driver_cache) {
        Settings::values.use_vulkan_driver_pipeline_cache.SetValue(false);
    }
    if (plan.vram_in_arena) {
        setenv("ORBIS_VRAM_GARLIC", "0", 1);
    }
    Ps4::Log("memory: %lu MiB of direct memory for this process (%s), profile %s -> %s: driver arena %d MiB, shader preload %d, driver cache %s%s",
             static_cast<unsigned long>(budget_mib), Ps4Memory::TierName(detected, false),
             profile_text.empty() ? "auto" : profile_text.c_str(), Ps4Memory::TierName(plan.tier, false), plan.arena_mib,
             plan.shader_preload, plan.driver_cache ? "on" : "off", plan.vram_in_arena ? ", every GPU image inside the arena" : "");
    Ps4::Log("memory: watch warns below %lu MiB and is critical below %lu MiB of free direct memory (largest free block); on critical: %s",
             static_cast<unsigned long>(Ps4Memory::Watch::DefaultLowMiB), static_cast<unsigned long>(Ps4Memory::Watch::DefaultCriticalMiB),
             g_memory_action == Ps4Memory::Action::Pause ? "pause the game and ask" : "warn on screen");
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
///   async_shaders=on|off        (default off since 0.7.7 fix: on skips draws whose shader is still compiling)
///   mode=handheld|docked        console mode: 720p or 1080p rendering (default handheld)
///   resolution=0.5|0.75|1|1.5|2 internal resolution scale (default 1)
///   speed_limit=on|off          (default on)
///   multicore=on|off            (default on)
///   vram=conservative|aggressive  (default conservative)
///   shader_cache=on|off         shaders kept on disk (default on)
///   shader_preload=0|128|256|1024|4096|all  how much of the disk cache is compiled at start (default all, no
///                               limit, since 0.7.7): with a number the cache may weigh up to N x 32 KiB and a bigger
///                               one is set aside under a new name, never deleted (see shader_policy.h)
///   shader_notice=on|off        the shader counter and the low-memory warning over the game (default on)
///   overlay_mode=keepalive|classic  0.7.6: keepalive keeps the HUD, notice and pause menu in every video buffer so
///                               they do not flicker (default); classic is the 0.7.3 way, drawn once around the flip
///   overlay_poll=1|2|4|8        keepalive: ms between the worker's checks of the buffers (default 1)
///   overlay_flip_stamp=on|off   keepalive: also draw inside the flip call (default on)
///   overlay_delay=0|1|2|4|8     classic: ms the GPU gets to finish a frame before the HUD is drawn into it (default 2)
///   memory_profile=auto|roomy|standard|tight|strict  0.7.6: arena, shader preload and driver cache for this
///                               console's memory (default auto: chosen from the direct memory it gives; see memory_guard.h)
///   sysflex_mib=auto|off|256|512|768|1024|1536|2048  guest RAM may take up to N MiB of the console's system flexible
///                               memory (outside the ~4.5 GiB of direct memory), keeping 128 MiB of that pool for the
///                               system. 0.9.0: off by default, experimental. 0.9.1: default auto (1536 MiB, or what the
///                               pool has); a start after a death before drawing (safe mode) limits it to 512. Idea from Eden4.
///   players=1|2|3|4             0.9.1: controllers given to the game (default 1). Controllers 2-4 are the other users
///                               logged in to the console (a second DualShock logs in as a guest on its PS button)
///   memory_action=warn|pause    when the memory watch is critical: pause the game and ask (default since 0.8.4) or only warn on screen
///   driver_cache=on|off         the driver's own pipeline cache file (default on; off saves memory and disk)
///   overlay=on|off              the video hook: HUD, pause menu, colour swap (default on)
///   overlay_draw=on|off         draw the HUD and the pause menu over the game (default on)
///   hud=on|off                  performance HUD over the game (default off)
///   hud_size=normal|small, hud_position=left|right
///   colour_swap=on|off          swap red and blue on the screen (default on)
///   Options from Eden's own screens (0.7.4), each applied only if this Eden has the setting (eden_settings.h):
///   vsync=fifo|mailbox|immediate|relaxed, scaling_filter=bilinear|nearest|bicubic|gaussian|scaleforce|fsr|area,
///   anti_aliasing=none|fxaa|smaa, dma_accuracy=default|unsafe|safe, anisotropy=default|auto|2|4|8|16,
///   gpu_accuracy=medium, resolution=0.25|1.25, sync_memory=on|off, skip_cpu_invalidation=on|off,
///   auto_stub=on|off, speed_percent=50|75|100|150|200, region=japan|usa|europe|australia|china|korea|taiwan
///   covers=full|thumbs|off      0.8.2: the menu's game pictures: full (default), only the small tiles, or none (less heap)
///   0.8.2 (eden_settings.h, each only if this Eden has the setting): cpu_clock=normal|boost|overclock,
///   cpu_ticks=off|1000|4000|16000|21000, async_presentation=on|off
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
        if (g_safe_mode && Ps4Safe::IsExperimental(key, value)) {
            Ps4::Log("settings.txt: %s=%s (ignored for this start: safe mode)", key.c_str(), value.c_str());
            continue;
        }
        bool known = true;
        bool missing = false; // a 0.7.4 option this Eden has no setting for
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
                const auto result = Ps4Eden::Apply(v, key, value); // 0.25 and 1.25, if this Eden has them
                known = result == Ps4Eden::Result::Applied;
                missing = result == Ps4Eden::Result::Missing;
            }
        } else if (key == "shader_preload" &&
                   (value == "0" || value == "128" || value == "256" || value == "1024" || value == "4096" || value == "all")) {
            Ps4Hook::Pending().shader_preload = value == "all" ? -1 : std::atoi(value.c_str());
        } else if (key == "shader_notice" && (value == "on" || value == "off")) {
            Ps4Hook::Pending().shader_notice = value == "on";
        } else if (key == "overlay_delay" && (value == "0" || value == "1" || value == "2" || value == "4" || value == "8")) {
            Ps4Hook::Pending().draw_delay_ms = std::atoi(value.c_str());
        } else if (key == "overlay_mode" && (value == "keepalive" || value == "classic")) {
            Ps4Hook::Pending().keepalive = value == "keepalive";
        } else if (key == "overlay_poll" && (value == "1" || value == "2" || value == "4" || value == "8")) {
            Ps4Hook::Pending().poll_ms = std::atoi(value.c_str());
        } else if (key == "overlay_flip_stamp" && (value == "on" || value == "off")) {
            Ps4Hook::Pending().flip_stamp = value == "on";
        } else if (key == "memory_profile" && (value == "auto" || value == "roomy" || value == "standard" || value == "tight" || value == "strict")) {
            // Already applied by ApplyMemoryPlan(), before this file's other lines.
        } else if (key == "autoshot" && (value == "on" || value == "off")) {
            g_autoshot = value == "on";
        } else if (key == "sysflex_mib" && Ps4Memory::ParseSystemFlexibleCap(value, &g_sysflex_cap_mib)) {
            // Applied by ConfigureSystemFlexible() after this file is read, before the core starts.
            if (value == "128" || value == "384") {
                // 0.9.0 values that the menu does not offer any more (the old "Minimum RAM" profile wrote 384): read as auto.
                Ps4::Log("settings.txt: sysflex_mib=%s is a 0.9.0 value; 0.9.1 uses auto instead (choose 256 to 2048 in the menu for a fixed amount)",
                         value.c_str());
                g_sysflex_cap_mib = Ps4Memory::SystemFlexibleAutoCapMiB;
            }
        } else if (key == "players" && (value == "1" || value == "2" || value == "3" || value == "4")) {
            g_players = std::atoi(value.c_str()); // applied before the core starts (ConnectPlayers) and in the main loop
        } else if (key == "memory_action" && (value == "warn" || value == "pause")) {
            g_memory_action = value == "pause" ? Ps4Memory::Action::Pause : Ps4Memory::Action::Warn;
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
        } else if (key == "covers" && (value == "full" || value == "thumbs" || value == "off")) {
            // Read by the menu code before the game list opens.
        } else if (key == "language" && (value == "pt" || value == "en")) {
            // Only the menu reads it.
        } else if (key == "menu_theme" && Menu::FindOption("menu_theme") != nullptr && value.size() < 16) {
            // The menu's colour (0.7.10). The menu applies it as it draws; this is for a start with no menu
            // (nomenu.txt), so the HUD and the pause menu still get it.
            const Menu::Option& theme = *Menu::FindOption("menu_theme");
            for (size_t i = 0; i < theme.choices.size(); ++i) {
                if (value == theme.choices[i].value) {
                    Menu::ApplyTheme(int(i));
                }
            }
        } else if (key == "env" && value.find('=') != std::string::npos && value.find('=') > 0) {
            // A driver switch, e.g. env=ORBIS_GS_RING_SCALE=4 (read when Vulkan starts).
            const auto split = value.find('=');
            setenv(value.substr(0, split).c_str(), value.c_str() + split + 1, 1);
        } else if (const auto result = Ps4Eden::Apply(v, key, value); result != Ps4Eden::Result::NotMine) {
            known = result == Ps4Eden::Result::Applied;
            missing = result == Ps4Eden::Result::Missing;
        } else {
            known = false;
        }
        Ps4::Log("settings.txt: %s=%s%s", key.c_str(), value.c_str(),
                 known ? "" : missing ? " (this Eden has no such setting, ignored)" : " (unknown, ignored)");
    }
}

/// DualShock 4 -> the virtual gamepad Eden binds to a player (0.9.1: `player` 0-3, the port of Eden's virtual_gamepad),
/// by position like a Switch pad.
void PollPad(InputCommon::VirtualGamepad& pad, const Ps4::PadState& s, int player = 0) {
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
        pad.SetButtonState(static_cast<std::size_t>(player), m.button, (s.buttons & m.bit) != 0);
    }
    const auto axis = [](u8 value) { return std::clamp((static_cast<int>(value) - 128) / 127.0f, -1.0f, 1.0f); };
    // Switch sticks: up is positive.
    pad.SetStickPosition(static_cast<std::size_t>(player), InputCommon::VirtualGamepad::VirtualStick::Left, axis(s.lx), -axis(s.ly));
    pad.SetStickPosition(static_cast<std::size_t>(player), InputCommon::VirtualGamepad::VirtualStick::Right, axis(s.rx), -axis(s.ry));
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
    return LoadSettingsFile().Language() == Menu::Lang::Pt;
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
                   "covers/     capas: PNG, JPG ou BMP com o ID do titulo (01009B90006DC000.png) ou o nome do jogo\n"
                   "            covers: PNG, JPG or BMP named by title id or by the game's file name\n"
                   "shots/      capturas de tela (L3 + TouchPad; e 3 depois de cada travada de 1 s ou mais) / screenshots (L3 + TouchPad; and 3 after each freeze of 1 s or more)\n"
                   "logs/       os ultimos 8 boot.log / the last 8 boot logs\n"
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

    Ps4::NoteRestartOutcome();
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
    // 0.8.2: safe mode. If the previous start never got as far as drawing, its experimental options stay off for this one.
    g_safe_mode = Ps4Safe::PreviousStartDied(SafeStartFile());
    if (g_safe_mode) {
        Ps4::Log("safe mode: the last start ended before the game drew anything; experimental lines of settings.txt are ignored for this start (settings.txt is not changed)");
    }
    Ps4Safe::MarkStart(SafeStartFile(), game);
    ApplyPs4Settings();
    ApplyMemoryPlan();
    ApplySettingsFile();
    if (g_safe_mode && g_sysflex_cap_mib > Ps4Memory::SystemFlexibleProvenMiB) {
        // The last start died before drawing: the pool is not switched off (that costs direct memory), but it goes back
        // to the amount a console has been seen to give (0.9.0 test) instead of the larger automatic one.
        Ps4::Log("safe mode: system flexible memory limited to %lu MiB for this start (settings.txt asked for %lu)",
                 static_cast<unsigned long>(Ps4Memory::SystemFlexibleProvenMiB), static_cast<unsigned long>(g_sysflex_cap_mib));
        g_sysflex_cap_mib = Ps4Memory::SystemFlexibleProvenMiB;
    }
    Ps4::ConfigureSystemFlexible(g_sysflex_cap_mib);
    if (g_players > 1) {
        const Ps4Eden::Result linked = Ps4Eden::ConnectPlayers(Settings::values, g_players);
        const int opened = Ps4::OpenExtraPads(g_players);
        Ps4::Log("controllers: players=%d, this Eden %s; %d controller(s) open now (the others are looked for every 2 s)", g_players,
                 linked == Ps4Eden::Result::Applied ? "has the player settings (players 2-4 marked connected)" : "has no player settings (only controller 1 works)",
                 Ps4::PadsOpen());
        (void)opened;
    }
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
        const Ps4Hook::Settings& hook = Ps4Hook::Pending();
        if (hook.keepalive) {
            Ps4::Log("overlay: keep-alive drawing (the HUD, notice and pause menu are kept in every video buffer), checks every %d ms, flip stamp %s",
                     hook.poll_ms, hook.flip_stamp ? "on" : "off");
        } else {
            Ps4::Log("overlay: classic drawing (0.7.3), %d ms wait", hook.draw_delay_ms);
        }
        Ps4Hook::Arm(Ps4Hook::Pending());
        Ps4::Log("screenshot: L3 + TouchPad saves the screen (HUD included) as %s/shots/shot_NNNN.bmp; the game pauses for a moment", Ps4::DataDir);
    } else {
        Ps4::Log("overlay off: no HUD, no pause menu, no colour swap");
    }
    // 0.7.7: the pool that builds the disk cache's pipelines is sized from hardware_concurrency() when Eden
    // creates its pipeline cache, which happens inside Load, so the answer is decided here. A marker file
    // says the last start died while the cache loaded; the deaths seen so far were in those builder
    // threads, so this start builds with ONE (see shader_policy.h for the whole ladder).
    const fs::path shader_root = fs::path{Ps4::DataDir} / "cache/shader";
    const fs::path marker_file = shader_root / "loading.marker";
    fs::create_directories(shader_root, ec);
    Ps4Shader::LoadMarker marker = Ps4Shader::ReadMarker(marker_file);
    if (!marker.present) {
        // 0.7.6 kept one "<title id>.loading" file per game: the same news, from an older version.
        for (fs::directory_iterator it{shader_root, ec}, end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == ".loading") {
                marker = Ps4Shader::ReadMarker(it->path());
                std::error_code rm_ec;
                fs::remove(it->path(), rm_ec);
                break;
            }
        }
    }
    const bool disk_cache = Settings::values.use_disk_shader_cache.GetValue();
    if (disk_cache && marker.present) {
        Ps4::SetWorkerCap(2); // Eden builds with max(threads, 2) - 1
        Ps4::Log("shader cache: the last start died while the cache loaded (attempt %d, title %s): this start builds with one thread",
                 marker.attempt, marker.title.empty() ? "unknown" : marker.title.c_str());
    }
    const Core::SystemResultStatus loaded = system.Load(window, game, params);
    {
        unsigned asked = 0, capped = 0;
        Ps4::WorkerCapStats(&asked, &capped);
        Ps4::Log("shader cache: hardware_concurrency was asked %u times, %u answers cut by the cap%s", asked, capped,
                 asked == 0 ? " (never asked: the --wrap did not take effect, the cap does nothing)" : "");
    }
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
    if (disk_cache) {
        Ps4::SetPhase("shader cache");
        // Eden compiles EVERY pipeline of the game's disk cache before the game starts, and each one
        // stays in the driver's memory. Until 0.7.2 this was limited by stopping Eden's load through its
        // stop token, but Eden answers a stop by ending every thread of its pipeline worker pool
        // (LoadDiskResources -> WaitForRequests), so after a capped preload nothing was left to build the
        // shaders the game asked for. Since then the size of the cache is limited BEFORE Eden reads it
        // (shader_preload, see shader_policy.h) and Eden always reads what is left to the end.
        // 0.7.7: no limit by default (like the Cemu cache the player expects), and a start that finds
        // the marker of a death goes down a ladder that never deletes anything: the same cache with one
        // builder thread, then the cache parked (vulkan.bin.bak) and an empty one.
        const std::uint64_t title_id = system.GetApplicationProcessProgramID();
        char id[17];
        std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(title_id));
        std::error_code cache_ec;
        const Ps4Shader::LoadMode mode = Ps4Shader::DecideLoad(marker, id);
        const int attempt = Ps4Shader::NextAttempt(marker, id);
        if (mode != Ps4Shader::LoadMode::Normal) {
            Ps4::Log("shader cache: %s - mode %s", id, Ps4Shader::LoadModeName(mode));
        }
        const int cap = Ps4Hook::Pending().shader_preload;
        if (mode == Ps4Shader::LoadMode::Empty) {
            fs::path parked;
            if (Ps4Shader::ParkCache(shader_root / id, &parked)) {
                Ps4::Log("shader cache: the last two starts died loading it, so vulkan.bin was kept as %s and this game starts with an empty cache",
                         parked.filename().string().c_str());
            } else {
                Ps4::Log("shader cache: no vulkan.bin to park (or it could not be moved)");
            }
        } else {
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
                Ps4::Log("shader cache: vulkan.bin is %.1f MiB, over the %.1f MiB limit (shader_preload=%d): kept as vulkan.bin.bak (nothing deleted), a new cache starts",
                         size_mib, limit_mib, cap);
                break;
            case Ps4Shader::CacheAction::Failed:
                Ps4::Log("shader cache: vulkan.bin is %.1f MiB, over the %.1f MiB limit but it could not be moved; loading it anyway",
                         size_mib, limit_mib);
                break;
            }
        }
        if (!Ps4Shader::WriteMarker(marker_file, attempt, id)) {
            Ps4::Log("shader cache: the marker file could not be written (a death in the load will not be noticed)");
        }
        // Never stopped: see above.
        const std::stop_source never_stopped;
        std::atomic<size_t> loaded{0}, total{0}, logged{0};
        const std::uint64_t load_began = Ps4::NowUs();
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
        fs::remove(marker_file, cache_ec);
        Ps4::SetWorkerCap(0); // the recovery start is over; whatever Eden asks for from now on gets the real answer
        Ps4::Log("shader cache: %zu pipelines loaded in %.1f s", loaded.load(), double(Ps4::NowUs() - load_began) / 1e6);
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
    bool extra_was_connected[Ps4::MaxPads] = {};
    int menu_sel = 0;
    std::uint32_t menu_buttons_before = 0;
    bool notice_on = Ps4Hook::Pending().shader_notice;
    constexpr int MenuRows = 6;   // Continue, HUD, speed limit, shader notice, restart the game, back to the game list
    constexpr int MemoryRows = 3; // the memory alert: continue anyway, restart the game, back to the game list
    bool restart_game = false;    // 0.7.7: leave by starting this same game again (no game list)
    bool memory_panel = false;    // the panel is the memory alert (memory_action=pause), not the L3 + R3 menu
    Ps4Memory::Watch mem_watch;
    Ps4Memory::Status mem_status;
    const auto show_menu = [&] {
        if (memory_panel) {
            char title[80];
            std::snprintf(title, sizeof(title), pt ? "Memoria critica: %lu MiB livres" : "Memory critical: %lu MiB free",
                          static_cast<unsigned long>(mem_status.free_mib));
            Ps4Hook::ShowPanel(title,
                               {pt ? "Continuar assim mesmo (pode travar)" : "Continue anyway (it may crash)",
                                pt ? "Reiniciar o jogo (libera a memoria)" : "Restart the game (frees the memory)",
                                pt ? "Sair do jogo (volta ao menu de jogos)" : "Exit game (back to the game list)"},
                               menu_sel,
                               pt ? "O jogo foi pausado antes de a memoria acabar.   X: confirmar" : "The game was paused before memory ran out.   X: confirm");
            return;
        }
        const auto state = [&](bool on) { return std::string(on ? (pt ? "ligado" : "on") : (pt ? "desligado" : "off")); };
        const std::vector<std::string> lines = {
            pt ? "Continuar" : "Continue",
            std::string(pt ? "HUD de desempenho: " : "Performance HUD: ") + state(Ps4Hook::HudOn()),
            std::string(pt ? "Limite de velocidade: " : "Speed limit: ") + state(Settings::values.use_speed_limit.GetValue()),
            std::string(pt ? "Aviso de shaders e memoria: " : "Shader and memory notice: ") + state(notice_on),
            pt ? "Reiniciar o jogo" : "Restart the game",
            pt ? "Sair do jogo (volta ao menu de jogos)" : "Exit game (back to the game list)"};
        Ps4Hook::ShowPanel(pt ? "Menu do emulador" : "Emulator menu", lines, menu_sel,
                           pt ? "Cima/Baixo: escolher   X: confirmar   O: voltar" : "Up/Down: choose   X: confirm   O: back");
    };
    std::uint64_t last_status = Ps4::NowUs(), last_notice = 0, last_mem_check = 0, last_timeline = Ps4::NowUs();
    // 0.8.2: safe mode says so on the screen for the first 15 s, and the file is cleared once the game has drawn.
    const std::uint64_t safe_notice_until = g_safe_mode ? Ps4::NowUs() + 15000000 : 0;
    bool safe_start_cleared = false;
    unsigned status_count = 0;
    int shaders_last = 0, shaders_total = 0;
    std::uint64_t free_mib = Ps4::FreeDirectMemory() >> 20;
    const auto stage_name = [](Ps4Memory::Stage stage) {
        return stage == Ps4Memory::Stage::Ok ? "ok" : stage == Ps4Memory::Stage::Low ? "low" : "critical";
    };
    // 0.7.8: L3 + TouchPad saves what is on the screen as shots/shot_NNNN.bmp (HUD included), so a graphics
    // fault can be looked at as it is, not through a phone camera. The game is paused for the copy.
    bool shot_before = false;
    std::uint64_t shot_until = 0;
    // 0.8.0: freezes of the picture. last_pause_us is the last time the game was paused on purpose (the pause menu
    // or a screenshot), so a gap made by that is not mistaken for a freeze of the game itself.
    std::uint64_t stall_seen = Ps4Hook::LastStall().count, last_pause_us = 0;
    int stalls_shot = 0;
    std::uint64_t auto_shot_due[3] = {0, 0, 0}; // 0 = nothing waiting
    // 0.8.1: the profile of a freeze. 0.6 s after the flips stop, a fresh profiler window opens; when the picture
    // comes back that window (the threads busy during the freeze) goes to the log. Three per run.
    std::uint64_t stall_window = 0; // when the window was opened, 0 = none open
    int stall_profiles = 0;
    const auto take_shot = [&](bool automatic = false) {
        if (!Ps4Hook::Ready()) {
            Ps4::Log("screenshot: not available yet (%s)", Ps4Hook::Describe().c_str());
            return;
        }
        system.Pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // frames already in flight reach the screen first
        const std::string path = Ps4Shot::NextPath(fs::path{Ps4::DataDir} / "shots");
        const std::uint64_t t0 = Ps4::NowUs();
        std::string why;
        const bool ok = Ps4Hook::SaveScreenshot(path, &why);
        const unsigned long ms = static_cast<unsigned long>((Ps4::NowUs() - t0) / 1000);
        system.Run();
        last_pause_us = Ps4::NowUs();
        const std::string name = fs::path{path}.filename().string();
        if (ok) {
            Ps4::Log("screenshot: saved %s in %lu ms (the picture on screen, HUD included%s)", path.c_str(), ms,
                     automatic ? ", taken by itself after a freeze" : "");
        } else {
            Ps4::Log("screenshot: NOT saved (%s)", why.c_str());
        }
        if (automatic) {
            return; // no notice: it would be drawn into the next pictures and hide what is being looked at
        }
        Ps4Hook::SetNotice(ok ? std::string(pt ? "Print salvo: shots/" : "Screenshot saved: shots/") + name
                              : std::string(pt ? "Print nao salvo: " : "Screenshot failed: ") + why);
        shot_until = Ps4::NowUs() + 4000000;
    };
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
            const int rows = memory_panel ? MemoryRows : MenuRows;
            if (pressed & Ps4::Button::Down) {
                menu_sel = (menu_sel + 1) % rows;
                redraw = true;
            }
            if (pressed & Ps4::Button::Up) {
                menu_sel = (menu_sel + rows - 1) % rows;
                redraw = true;
            }
            if (pressed & Ps4::Button::Cross) {
                if (memory_panel) {
                    if (menu_sel == 0) {
                        close = true;
                    } else {
                        restart_game = menu_sel == 1;
                        leave = true;
                    }
                } else if (menu_sel == 0) {
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
                    restart_game = menu_sel == 4;
                    leave = true;
                }
            }
            if (leave) {
                leave_reason = restart_game ? (memory_panel ? "restart chosen in the memory alert" : "restart chosen in the pause menu")
                                            : (memory_panel ? "exit chosen in the memory alert" : "exit chosen in the pause menu");
                Ps4::Log("%s", leave_reason);
                break;
            }
            if (close) {
                in_menu = false;
                Ps4Hook::HidePanel();
                system.Run();
                if (memory_panel) {
                    Ps4::Log("memory alert closed, the game runs again (largest free direct block %lu MiB)",
                             static_cast<unsigned long>(Ps4::FreeDirectMemory() >> 20));
                } else {
                    Ps4::Log("pause menu closed, the game runs again");
                }
                memory_panel = false;
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
        {
            const bool shot_combo = (pad.buttons & Ps4::Button::L3) && (pad.buttons & Ps4::Button::TouchPad) &&
                                    !(pad.buttons & Ps4::Button::R3) && !(pad.buttons & Ps4::Button::Options);
            if (shot_combo && !shot_before) {
                take_shot();
            }
            shot_before = shot_combo;
        }
        // 0.8.0: a freeze of the picture (one second or more) is logged, with its flip number, the moment it ends,
        // and up to three freezes per run are followed by three screenshots (0.4 s, 2.5 s and 6 s later). Super
        // Mario Maker 2 shows its flat grey terrain and missing text only for the first seconds after a stall like
        // this one, so the pictures show how the defect evolves without anyone pressing the buttons at the right moment.
        {
            if (in_menu) {
                last_pause_us = Ps4::NowUs();
            }
            const Ps4Hook::Stall st = Ps4Hook::LastStall();
            const std::uint64_t quiet_us = Ps4Hook::SinceLastFlipUs();
            if (!in_menu && stall_window == 0 && stall_profiles < 3 && quiet_us >= 600000) {
                stall_window = Ps4::NowUs();
                Ps4::ProfileWindow(false);
                Ps4::Log("stall: no new picture for %.1f s so far; sampling the busy threads until it ends", double(quiet_us) / 1e6);
            } else if (stall_window != 0 && st.count == stall_seen && quiet_us < 100000) {
                stall_window = 0; // the pictures came back after less than a second: not a freeze, drop the window
            }
            if (st.count != stall_seen) {
                stall_seen = st.count;
                const std::uint64_t now = Ps4::NowUs();
                const bool by_pause = last_pause_us != 0 && now - last_pause_us < st.gap_us + 300000;
                Ps4::Log("stall: the picture froze for %.2f s and came back on flip #%lu%s; the Mesa log counts these frames in its "
                         "\"scan-out copy ... over N frames\" lines, and its last \"GPU HAS NOT FINISHED\" line (if any) belongs here",
                         double(st.gap_us) / 1e6, static_cast<unsigned long>(st.flip), by_pause ? " (the game was paused: not a freeze)" : "");
                if (stall_window != 0) {
                    stall_window = 0;
                    if (!by_pause) {
                        ++stall_profiles;
                        Ps4::Log("stall: the profile of the freeze follows (about the last %.1f s of it; busy = not waiting in the kernel)",
                                 double(st.gap_us - 600000) / 1e6);
                        Ps4::ProfileWindow(true);
                    }
                }
                if (g_autoshot && !by_pause && stalls_shot < 3) {
                    ++stalls_shot;
                    auto_shot_due[0] = now + 400000;
                    auto_shot_due[1] = now + 2500000;
                    auto_shot_due[2] = now + 6000000;
                }
            }
            for (std::uint64_t& due : auto_shot_due) {
                if (due != 0 && Ps4::NowUs() >= due) {
                    due = 0;
                    if (!in_menu) {
                        take_shot(true);
                    }
                }
            }
        }
        if (pad.connected && gamepad != nullptr) {
            PollPad(*gamepad, pad);
        }
        // 0.9.1: controllers 2-4. Nothing is sent while the pause menu is open (it is player 1's), and a controller that
        // goes away gets one neutral state so no button stays pressed.
        if (g_players > 1 && gamepad != nullptr) {
            Ps4::OpenExtraPads(g_players);
            for (int player = 1; player < g_players; ++player) {
                const Ps4::PadState extra = in_menu ? Ps4::PadState{} : Ps4::ReadPad(player);
                if (extra.connected && !extra_was_connected[player]) {
                    Ps4::Log("controllers: player %d connected", player + 1);
                }
                if (extra.connected || extra_was_connected[player]) {
                    PollPad(*gamepad, extra, player);
                }
                extra_was_connected[player] = extra.connected;
            }
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
        // The memory watch (0.7.6), once a second: the largest free block of direct memory, its stage
        // and the speed it is falling at. What kills the app is that block running out (the C heap
        // takes 128 MiB at a time), so this warns early and, with memory_action=pause, stops the game
        // at the critical level with a choice instead of letting it crash.
        if (Ps4::NowUs() - last_mem_check > 1000000) {
            last_mem_check = Ps4::NowUs();
            free_mib = Ps4::FreeDirectMemory() >> 20;
            const Ps4Memory::Stage before = mem_status.stage;
            mem_status = mem_watch.Update(free_mib, last_mem_check / 1000);
            if (mem_status.stage != before) {
                Ps4::Log("memory watch: %s -> %s, largest free direct block %lu MiB, trend %.0f MiB/min", stage_name(before),
                         stage_name(mem_status.stage), static_cast<unsigned long>(free_mib), mem_status.trend_mib_per_min);
            }
            if (mem_status.entered_critical && g_memory_action == Ps4Memory::Action::Pause) {
                if (Ps4Hook::Ready()) {
                    in_menu = true;
                    memory_panel = true;
                    menu_sel = 0;
                    menu_buttons_before = pad.buttons;
                    system.Pause();
                    Ps4::Log("memory critical (%lu MiB free): the game is paused (memory_action=pause)", static_cast<unsigned long>(free_mib));
                    show_menu();
                    continue;
                }
                Ps4::Log("memory critical (%lu MiB free): the pause panel needs the overlay, which is not ready", static_cast<unsigned long>(free_mib));
            }
        }
        // The shader counter (Cemu's "compiling shaders"): what Eden's own notifier says is being
        // built, kept as a running total, with the memory watch's warning after it. Every 250 ms.
        if (shot_until != 0 && Ps4::NowUs() >= shot_until) {
            shot_until = 0; // the screenshot message has been up long enough
            last_notice = 0;
            if (!notice_on) {
                Ps4Hook::SetNotice({});
            }
        }
        if (notice_on && shot_until == 0 && Ps4::NowUs() - last_notice > 250000) {
            last_notice = Ps4::NowUs();
            Ps4Shader::CountBuilding(system.GPU().ShaderNotify().ShadersBuilding(), &shaders_last, &shaders_total);
            // The memory warning comes from the watch above (with the speed of the fall), not from the
            // shader text: its own low-memory line is switched off by passing a huge free size.
            std::string text = Ps4Shader::NoticeText(shaders_last, shaders_total, std::uint64_t{1} << 40, pt);
            if (safe_notice_until != 0 && Ps4::NowUs() < safe_notice_until) {
                text = std::string(pt ? "Modo seguro: opcoes experimentais desligadas nesta partida" : "Safe mode: experimental options off for this start") +
                       (text.empty() ? "" : "   ") + text;
            }
            const std::string banner = Ps4Memory::BannerText(mem_status, pt);
            if (!banner.empty()) {
                text += text.empty() ? "" : "   ";
                text += banner;
            }
            Ps4Hook::SetNotice(text);
        }
        if (Ps4::NowUs() - last_status > 10000000) {
            last_status = Ps4::NowUs();
            const auto perf = system.GetAndResetPerfStats();
            const auto mem = Common::Orbis::GetStats();
            Ps4::Log("status: game %.1f fps, speed %.0f%%, frame %.1f ms, guest+tables %lu MiB committed (%lu MiB in system flexible memory, %lu MiB given back), largest free direct block %lu MiB (%s, %+.0f MiB/min), shaders built %d, kernel frees %lu calls / %lu MiB (%lu under 64 KiB)",
                     perf.average_game_fps, perf.emulation_speed * 100.0, perf.frametime * 1000.0,
                     static_cast<unsigned long>(mem.committed_bytes >> 20),
                     static_cast<unsigned long>(mem.system_flexible_bytes >> 20),
                     static_cast<unsigned long>(mem.decommitted_bytes >> 20),
                     static_cast<unsigned long>(Ps4::FreeDirectMemory() >> 20), stage_name(mem_status.stage),
                     mem_status.trend_mib_per_min, shaders_total, static_cast<unsigned long>(mem.guest_free_calls),
                     static_cast<unsigned long>(mem.guest_free_bytes >> 20), static_cast<unsigned long>(mem.guest_free_small));
            if (++status_count % 3 == 1) {
                Ps4HeapCensus();
                Ps4::Log("%s", Ps4Hook::Describe().c_str());
            }
        }
        // 0.7.9: the frame pacing the game really delivered to the screen, every 2 s: flips, the average and
        // longest interval, and how many intervals were longer than 34 / 50 / 100 / 250 ms. A speed of 9 fps
        // with a steady 110 ms interval is a slow game; a speed of 9 fps made of 30 fps and long stalls is
        // something else (a stutter), and this line tells them apart. Lines are skipped while no flip happens.
        if (Ps4::NowUs() - last_timeline > 2000000) {
            last_timeline = Ps4::NowUs();
            const Ps4Hook::Timeline tl = Ps4Hook::TakeTimeline();
            if (!safe_start_cleared && tl.flips_total >= Ps4Safe::OpenedAfterFlips) {
                safe_start_cleared = true;
                Ps4Safe::MarkOpened(SafeStartFile()); // the game drew: this start is not a crash
            }
            if (tl.flips > 0) {
                Ps4::Log("tl: %lu flips/2s, interval avg %.1f ms max %.0f ms, longer than 34/50/100/250 ms: %lu/%lu/%lu/%lu",
                         static_cast<unsigned long>(tl.flips), tl.flips > 1 ? double(tl.gap_sum_us) / double(tl.flips - 1) / 1000.0 : 0.0,
                         double(tl.gap_max_us) / 1000.0, static_cast<unsigned long>(tl.over_34ms), static_cast<unsigned long>(tl.over_50ms),
                         static_cast<unsigned long>(tl.over_100ms), static_cast<unsigned long>(tl.over_250ms));
            }
        }
    }

    // Eden cannot be torn down and started again inside one run (test 2 showed its shutdown crashing
    // in a worker thread, and the game's memory is not given back), so leaving the game means
    // starting the app again: a new process opens on the game list. The saves are written by the
    // game itself; the logs are flushed line by line.
    Ps4::SetPhase("leaving");
    Ps4::ArmHangWatch(0);
    Ps4Safe::MarkOpened(SafeStartFile()); // leaving on purpose is not a crash
    system.Pause();
    {
        const auto mem = Common::Orbis::GetStats();
        Ps4::Log("leaving the game (%s): lazy memory peak %lu MiB, %d shaders built", leave_reason,
                 static_cast<unsigned long>(mem.committed_bytes >> 20), shaders_total);
    }
    if (restart_game) {
        // The new process reads this file first thing (FindGame) and opens the same game.
        std::ofstream out{fs::path{Ps4::DataDir} / "restart_game.txt"};
        out << game << '\n';
        if (!out) {
            Ps4::Log("restart_game.txt could not be written: the game list opens instead");
            restart_game = false;
        }
    }
    Ps4Hook::ShowPanel(pt ? "Menu do emulador" : "Emulator menu",
                       {restart_game ? (pt ? "Reiniciando o jogo..." : "Restarting the game...")
                                     : (pt ? "Voltando ao menu de jogos..." : "Going back to the game list...")},
                       -1, "");
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); // a frame with the message
    if (Ps4::RestartApp()) {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1)); // the system is replacing the process
        }
    }
    // 0.7.7: the system would not start the app again (the reason is in boot.log). Until 0.7.6 the app then
    // stayed open on a panel that sent the player to boot.log; now it says what happens and closes itself.
    Ps4::SetPhase("finished");
    Ps4::Log("the app could not start itself again: closing it%s", restart_game ? " (restart_game.txt stays: the game starts at the next open)" : "");
    Ps4Hook::ShowPanel(pt ? "O app vai fechar" : "The app is closing",
                       {restart_game ? (pt ? "O sistema nao deixou reiniciar sozinho. Abra o app de novo: o jogo inicia direto."
                                           : "The system would not restart it. Open the app again: the game starts right away.")
                                     : (pt ? "O sistema nao deixou voltar sozinho. Abra o app de novo para ver a lista de jogos."
                                           : "The system would not go back by itself. Open the app again to see the game list.")},
                       -1, "boot.log");
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    Ps4::QuitApp();
}

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
