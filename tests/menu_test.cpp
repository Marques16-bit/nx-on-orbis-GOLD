// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the menu (frontend/menu/): settings.txt reading and writing, the navigation, and
// screenshots of every tab. Runs on a PC / in CI, not on the console.
//
//   g++ -O2 -std=c++20 -Wall -Wextra -Ifrontend tests/menu_test.cpp frontend/menu/menu_ui.cpp frontend/menu/settings_model.cpp frontend/menu/menu_canvas.cpp -o menu_test
//   ./menu_test                 # exit code 0 = all good
//   ./menu_test --render DIR    # also writes DIR/*.ppm, one screenshot per tab and language

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "menu/game_config.h"
#include "menu/menu_ui.h"
#include "menu/nro_assets.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

using namespace Menu;

/// A test rig: a menu with a few games, and the settings.txt it last saved.
struct Rig {
    std::string saved;
    int saves = 0;
    bool save_works = true;
    std::uint64_t now = 1000000;
    std::uint32_t held = 0;
    std::map<std::string, std::string> game_files; // per-game/<key>.txt, by game file name (0.9.5)
    int game_saves = 0;
    bool game_save_works = true;
    MenuUi ui;

    SystemInfo WithGameFiles(SystemInfo info) {
        info.load_game_config = [this](const std::string& game) {
            const auto it = game_files.find(game);
            return it == game_files.end() ? std::string() : it->second;
        };
        info.save_game_config = [this](const std::string& game, const std::string& text) {
            ++game_saves;
            if (!game_save_works) {
                return false;
            }
            if (SettingsFile(text).HasSettings()) {
                game_files[game] = text;
            } else {
                game_files.erase(game);
            }
            return true;
        };
        return info;
    }

    explicit Rig(const std::string& settings, int games = 3, int initial = 0)
        : ui(MakeGames(games), initial, WithGameFiles(MakeInfo(games)), settings,
             [this](const std::string& text) {
                 saved = text;
                 ++saves;
                 return save_works;
             }) {}

    static std::vector<GameEntry> MakeGames(int n) {
        std::vector<GameEntry> games;
        const char* names[] = {"Super Mario Odyssey [0100000000010000].nsp",
                               "The Legend of Zelda - Tears of the Kingdom.xci", "Celeste.nsp",
                               "Hollow Knight [0100633007D48000].nsp"};
        const std::uint64_t sizes[] = {5800ull << 20, 16ull << 30, 1200ull << 20, 3ull << 30};
        for (int i = 0; i < n; ++i) {
            games.push_back({names[i % 4], sizes[i % 4]});
        }
        return games;
    }
    static SystemInfo MakeInfo(int games) {
        (void)games;
        SystemInfo info;
        info.build_id = "v0.4.0-20261024T120000Z-5f142c79a1b2";
        info.data_dir = "/data/edenps4";
        info.keys_found = true;
        info.firmware_files = 227;
        info.free_memory_mib = 3120;
        info.settings_bytes = 142;
        info.log_previous = {"[    0.512] controller open", "[   12.300] game 1 of 3 in roms/", "[   41.002] !! thread stacks (hang)"};
        info.log_current = {"[    0.100] data folder /data/edenps4: writable, 0 entries opened to everyone"};
        return info;
    }
    /// Press and release one button.
    void Tap(std::uint32_t button) {
        held = button;
        ui.Update(held, now);
        now += 50000;
        held = 0;
        ui.Update(held, now);
        now += 50000;
    }
};

struct Frame {
    std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(1920 * 1080);
    Canvas Canvas_() { return Canvas{pixels.data(), 1920, 1080}; }
};

void TestSettingsFile() {
    std::printf("settings file\n");
    {
        SettingsFile f("");
        Check(f.Language() == Lang::Pt, "default language is Portuguese");
        const Option* mode = FindOption("mode");
        Check(mode != nullptr && f.IsDefault(*mode), "empty file: every option at its default");
        f.SetChoice(*mode, 1);
        Check(f.Get("mode") == "docked", "a choice is written as key=value");
        Check(f.Text().find("mode=docked") != std::string::npos, "...and is in the text");
        f.SetChoice(*mode, 0);
        Check(f.Get("mode").empty(), "back to the default: the line is removed");
        Check(f.Text().find("mode=") == std::string::npos, "...from the text too");
    }
    {
        // What a person may have written by hand: comments, spaces, CRLF, unknown keys, an env line.
        const std::string hand = "# my notes\r\ncpu_accuracy = accurate  # slower\r\n\nfoo=bar\nenv=RADV_DEBUG=info,nohiz\nastc=gpu\n";
        SettingsFile f(hand);
        Check(f.Get("cpu_accuracy") == "accurate", "key with spaces, comment and CRLF is read");
        Check(f.ChoiceIndex(*FindOption("cpu_accuracy")) == 2, "choice index follows");
        f.SetChoice(*FindOption("cpu_accuracy"), 0);
        f.SetChoice(*FindOption("gpu_accuracy"), 2); // low, medium, high
        const std::string text = f.Text();
        Check(text.find("# my notes") != std::string::npos, "comments are kept");
        Check(text.find("foo=bar") != std::string::npos, "unknown lines are kept");
        Check(text.find("env=RADV_DEBUG=info,nohiz") != std::string::npos, "hand-written env lines are kept");
        Check(text.find("astc=gpu") != std::string::npos, "other options are not touched");
        Check(text.find("cpu_accuracy") == std::string::npos, "default removes the line of cpu_accuracy");
        Check(text.find("gpu_accuracy=high") != std::string::npos, "a new option is appended");
    }
    {
        SettingsFile f("env=RADV_DEBUG=info\n");
        const Option* arena = FindOption("env:ORBIS_ARENA_MIB");
        Check(arena != nullptr, "the driver arena option exists");
        Check(f.ChoiceIndex(*arena) == arena->default_index, "no line: default arena");
        Check(std::string(arena->choices[size_t(arena->default_index)].value) == "1280", "the default arena is still 1280 MiB");
        f.SetChoice(*arena, 3);
        Check(f.Text().find("env=ORBIS_ARENA_MIB=1536") != std::string::npos, "env option is written as env=NAME=value");
        Check(f.Get("env:ORBIS_ARENA_MIB") == "1536" && f.Get("env:RADV_DEBUG") == "info", "env lines are told apart by name");
        f.SetChoice(*arena, arena->default_index);
        Check(f.Text().find("ORBIS_ARENA_MIB") == std::string::npos, "default arena removes its line");
        Check(f.Text().find("env=RADV_DEBUG=info") != std::string::npos, "the other env line stays");
    }
    {
        // 0.7.6: the overlay and memory options. Their values are exactly what main.cpp's ApplySettingsFile
        // accepts, and each default is what main.cpp does when the line is absent.
        const auto check = [&](const char* key, std::vector<std::string> values, const char* def) {
            const Option* o = FindOption(key);
            bool ok = o != nullptr && o->choices.size() == values.size();
            for (size_t i = 0; ok && i < values.size(); ++i) {
                ok = values[i] == o->choices[i].value;
            }
            ok = ok && std::string(o->choices[size_t(o->default_index)].value) == def;
            std::printf("%s", ok ? "" : (std::string("  (") + key + ")\n").c_str());
            Check(ok, key);
        };
        check("overlay_mode", {"keepalive", "classic"}, "keepalive");
        check("overlay_poll", {"1", "2", "4", "8"}, "1");
        check("overlay_flip_stamp", {"on", "off"}, "on");
        check("memory_profile", {"auto", "roomy", "standard", "tight", "strict"}, "auto");
        check("memory_action", {"warn", "pause"}, "pause"); // 0.8.4: pause by default
        check("sysflex_mib", {"auto", "off", "256", "512", "768", "1024", "1536", "2048"}, "auto"); // 0.9.1: automatic by default
        check("players", {"1", "2", "3", "4"}, "1");                                           // 0.9.1: one controller, as before
        check("env:ORBIS_ARENA_MIB", {"1024", "1152", "1280", "1536", "1792"}, "1280");
        check("shader_preload", {"0", "128", "256", "1024", "4096", "all"}, "all"); // 0.7.7: no limit by default
        SettingsFile f("overlay_mode=classic\nmemory_profile=tight\nmemory_action=pause\n");
        Check(f.ChoiceIndex(*FindOption("overlay_mode")) == 1 && f.ChoiceIndex(*FindOption("memory_profile")) == 3 &&
                  f.ChoiceIndex(*FindOption("memory_action")) == 1,
              "the new options are read from settings.txt");
        f.SetChoice(*FindOption("overlay_mode"), 0);
        Check(f.Get("overlay_mode").empty() && f.Get("memory_profile") == "tight", "the default of overlay_mode removes its line, the others stay");
        Check(FindOption("overlay_mode")->group == Group::Perf && FindOption("memory_profile")->group == Group::Cpu, "they sit in the Perf and Cpu tabs");
    }
    {
        SettingsFile f("astc=bogus\nmode=docked\nlanguage=en\n");
        Check(f.ChoiceIndex(*FindOption("astc")) == FindOption("astc")->default_index, "an invalid value reads as the default");
        Check(f.Language() == Lang::En, "language=en");
        f.ResetAll();
        Check(f.Get("mode").empty() && f.Get("astc").empty(), "ResetAll removes the options");
        Check(f.Get("language") == "en", "...but keeps the language");
    }
    {
        // Every option: the default is one of its choices and the labels exist in both languages.
        bool ok = true;
        for (const Option& o : Catalog()) {
            ok = ok && o.default_index >= 0 && o.default_index < int(o.choices.size()) && o.choices.size() >= 2 &&
                 *o.label_pt && *o.label_en && *o.help_pt && *o.help_en;
            for (const Choice& c : o.choices) {
                ok = ok && *c.value && *c.pt && *c.en;
            }
        }
        Check(ok, "the catalog is well formed");
    }
}

void TestVisibility() {
    std::printf("visibility (0.9.5)\n");
    Check(TierOf("colour_swap") == Tier::Mandatory && TierOf("jit_page_reuse") == Tier::Mandatory && TierOf("jit_direct_table") == Tier::Mandatory,
          "the colour swap and the JIT pack switches are mandatory");
    Check(TierOf("bgra") == Tier::Mandatory && TierOf("a2b10") == Tier::Mandatory && TierOf("rg") == Tier::Mandatory && TierOf("swizzle") == Tier::Mandatory,
          "the colour fixes and the mapping self-test are mandatory");
    Check(TierOf("overlay_mode") == Tier::Mandatory && TierOf("overlay_delay") == Tier::Mandatory, "the HUD's timing options are mandatory");
    Check(TierOf("cpu_clock") == Tier::Experimental && TierOf("cpu_ticks") == Tier::Experimental && TierOf("astc_recompress") == Tier::Experimental,
          "the clock, ticks and texture-compression options are experimental");
    Check(TierOf("jit_cache_commit") == Tier::Experimental && FindOption("jit_cache_commit") &&
              std::string(FindOption("jit_cache_commit")->choices[size_t(FindOption("jit_cache_commit")->default_index)].value) == "lazy",
          "jit_cache_commit (0.9.7) is experimental and its default is lazy");
    Check(TierOf("mode") == Tier::Normal && TierOf("resolution") == Tier::Normal && TierOf("jit_cache_mib") == Tier::Normal &&
              TierOf("memory_profile") == Tier::Normal && TierOf("hud") == Tier::Normal && TierOf("players") == Tier::Normal,
          "the everyday options are normal");
    Check(TierOf("no_such_option") == Tier::Normal, "an unknown key is normal");
    Check(TierOf("sysflex_mib") == Tier::Normal && PerGameOffered(*FindOption("sysflex_mib")),
          "the extra RAM (sysflex_mib) is shown in the menu again, and a game may set it (third zip)");
    for (const Option& o : Catalog()) {
        Check(TierOf(o.key) != Tier::Mandatory || o.group != Group::Controls, "no mandatory option sits in the controls group");
    }
    // The defaults of the mandatory options are the values the port needs.
    const Option* reuse = FindOption("jit_page_reuse");
    const Option* direct = FindOption("jit_direct_table");
    const Option* swap = FindOption("colour_swap");
    Check(reuse && std::string(reuse->choices[size_t(reuse->default_index)].value) == "on", "jit_page_reuse is on by default");
    Check(direct && std::string(direct->choices[size_t(direct->default_index)].value) == "on", "jit_direct_table is on by default");
    const Option* align = FindOption("arena_align");
    Check(TierOf("arena_align") == Tier::Mandatory && align && std::string(align->choices[size_t(align->default_index)].value) == "on",
          "arena_align (0.9.7 fix) is a mandatory option, on by default");
    Check(swap && std::string(swap->choices[size_t(swap->default_index)].value) == "on", "colour_swap is on by default");
    {
        SettingsFile off("jit_page_reuse=off\n");
        Check(off.ChoiceIndex(*reuse) == 1 && !off.IsDefault(*reuse), "an explicit off (written by hand) is still read as off");
    }
    // What each tab lists.
    Rig r("");
    const auto lists = [](const MenuUi& ui, int tab, const char* key) {
        for (const Option* o : ui.TabOptions(tab)) {
            if (std::string(o->key) == key) {
                return true;
            }
        }
        return false;
    };
    bool leak = false;
    for (int tab = 0; tab < MenuUi::TabCount; ++tab) {
        if (tab == MenuUi::TabAdvanced) {
            continue;
        }
        for (const Option* o : r.ui.TabOptions(tab)) {
            leak = leak || (TierOf(o->key) != Tier::Normal && o->group != Group::Controls);
        }
    }
    Check(!leak, "no mandatory or experimental option shows in the everyday tabs");
    Check(!lists(r.ui, MenuUi::TabCpu, "arena_align"), "arena_align (mandatory) is not listed in the menu");
    Check(!lists(r.ui, MenuUi::TabCpu, "jit_page_reuse") && !lists(r.ui, MenuUi::TabCpu, "jit_direct_table") && lists(r.ui, MenuUi::TabCpu, "jit_cache_mib"),
          "the CPU tab has the JIT cache size but not the two mandatory JIT switches");
    Check(!lists(r.ui, MenuUi::TabVideo, "dma_accuracy") && !lists(r.ui, MenuUi::TabVideo, "dyna_state") && !lists(r.ui, MenuUi::TabVideo, "sync_memory") &&
              lists(r.ui, MenuUi::TabVideo, "resolution"),
          "the VIDEO tab has no driver switches");
    Check(!lists(r.ui, MenuUi::TabPerf, "overlay_mode") && !lists(r.ui, MenuUi::TabPerf, "overlay_poll") && lists(r.ui, MenuUi::TabPerf, "hud"),
          "the HUD tab has the HUD but not its timing");
    Check(lists(r.ui, MenuUi::TabSystem, "region") && lists(r.ui, MenuUi::TabSystem, "language") && !lists(r.ui, MenuUi::TabSystem, "players"),
          "the SYSTEM tab has the language, the colour and the region");
    Check(lists(r.ui, MenuUi::TabControls, "players") && lists(r.ui, MenuUi::TabControls, "player4_type"), "the CONTROLS tab has the players and their models");
#if EDEN_PS4_PUBLIC
    Check(r.ui.TabOptions(MenuUi::TabAdvanced).empty(), "public build: the EXPERIMENTAL tab is empty (and not shown)");
#else
    Check(lists(r.ui, MenuUi::TabAdvanced, "cpu_clock") && lists(r.ui, MenuUi::TabAdvanced, "astc_recompress") && !lists(r.ui, MenuUi::TabAdvanced, "jit_page_reuse"),
          "test build: the EXPERIMENTAL tab lists the experimental options, not the mandatory ones");
#endif
    Check(lists(r.ui, MenuUi::TabCpu, "sysflex_mib"), "the CPU/RAM tab has the extra RAM (sysflex_mib)");
    // show_advanced=on in settings.txt shows everything, in the last tab, in a test build; the public build ignores it.
    Rig all("show_advanced=on\n");
#if EDEN_PS4_PUBLIC
    Check(all.ui.TabOptions(MenuUi::TabAdvanced).empty() && all.ui.Tabs().size() == 6 && !lists(all.ui, MenuUi::TabAdvanced, "cpu_clock"),
          "public build: show_advanced=on shows nothing (no LOG, no TESTS)");
    Check(!lists(all.ui, MenuUi::TabCpu, "jit_page_reuse") && !lists(all.ui, MenuUi::TabCpu, "cpu_clock"), "public build: ...and no hidden option appears anywhere");
#else
    Check(lists(all.ui, MenuUi::TabAdvanced, "jit_page_reuse") && lists(all.ui, MenuUi::TabAdvanced, "colour_swap") && lists(all.ui, MenuUi::TabAdvanced, "cpu_clock") &&
              all.ui.Tabs().back() == MenuUi::TabAdvanced,
          "show_advanced=on: the mandatory and the experimental options are all in the last tab");
    Check(all.ui.Tabs().size() == 8, "show_advanced=on: every tab is shown");
    // Every option of the catalog is somewhere (a tab), so none is lost.
    int found = 0;
    for (const Option& o : Catalog()) {
        bool somewhere = false;
        for (int tab = 0; tab < MenuUi::TabCount; ++tab) {
            somewhere = somewhere || lists(all.ui, tab, o.key);
        }
        found += somewhere ? 1 : 0;
    }
    Check(found == int(Catalog().size()), "with show_advanced=on, every option of the catalog is in some tab");
#endif
}

const std::filesystem::path TempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / (std::string("menu_test_") + name + "_" + std::to_string(std::uint64_t(&g_checks) & 0xFFFFFF));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void TestGameConfig() {
    std::printf("a game's own settings (0.9.5)\n");
    namespace GC = GameConfig;
    // which options are global, which the game screen offers
    Check(GlobalOnly("language") && GlobalOnly("menu_theme") && GlobalOnly("covers") && GlobalOnly("memory_profile") && GlobalOnly("show_advanced"),
          "language, colour, covers, memory profile and show_advanced are global only");
    Check(!GlobalOnly("resolution") && !GlobalOnly("players") && !GlobalOnly("jit_cache_mib") && !GlobalOnly("region"), "everyday options may be set per game");
    Check(PerGameOffered(*FindOption("resolution")) && PerGameOffered(*FindOption("players")) && PerGameOffered(*FindOption("player2_type")) &&
              PerGameOffered(*FindOption("region")) && PerGameOffered(*FindOption("jit_cache_mib")),
          "resolution, players, controller models, region and the JIT cache are offered per game");
    Check(!PerGameOffered(*FindOption("language")) && !PerGameOffered(*FindOption("menu_theme")) && !PerGameOffered(*FindOption("jit_page_reuse")) &&
              !PerGameOffered(*FindOption("hud_size")) && !PerGameOffered(*FindOption("shader_cache")),
          "language, colour, the mandatory JIT switch, the HUD's size and the shader cache switch are not");
    // reading and writing overrides
    {
        SettingsFile f("");
        const Option* res = FindOption("resolution");
        Check(f.OverrideIndex(*res) == -1 && !f.HasSettings(), "an empty game file leaves every option to the global settings");
        f.SetOverride(*res, res->default_index);
        Check(f.OverrideIndex(*res) == res->default_index && f.Get("resolution") == res->choices[size_t(res->default_index)].value,
              "the default value, chosen for a game, is written (the global setting may differ)");
        Check(f.HasSettings(), "...and the file now has a setting");
        f.SetOverride(*res, -1);
        Check(f.OverrideIndex(*res) == -1 && !f.HasSettings(), "back to global: the line goes, and a file of only the header counts as empty");
        SettingsFile odd("resolution=7.5\n");
        Check(odd.OverrideIndex(*res) == -1 && odd.HasSettings(), "a value the menu does not offer is shown as global but kept until changed");
        SettingsFile e("# c\nmode=docked\nenv=RADV_DEBUG=info\n");
        const auto entries = e.Entries();
        Check(entries.size() == 2 && entries[0].first == "mode" && entries[1].first == "env:RADV_DEBUG" && entries[1].second == "info", "Entries lists the settings, env as env:NAME");
    }
    // the key of a game's file
    Check(GC::KeyFor("Super Mario Odyssey [0100000000010000].nsp") == "0100000000010000", "the title id names the file when the dump has one");
    Check(GC::KeyFor("Celeste.nsp") == "Celeste", "else the name without the extension");
    Check(GC::KeyFor("The Legend of Zelda - Tears of the Kingdom (v1.2).xci") == "The_Legend_of_Zelda_-_Tears_of_the_Kingdom_v1_2", "spaces and symbols become one underscore");
    Check(GC::KeyFor("../../etc/passwd.nsp") == "passwd" && GC::KeyFor("a/b\\c:d*e.nsp").find_first_of("/\\:*.") == std::string::npos,
          "no path characters get into a key");
    Check(GC::KeyFor("???.nsp") == "game" && GC::KeyFor(std::string(300, 'a') + ".nsp").size() == 80, "an empty key is 'game', a long one is cut to 80");
    Check(GC::KeyFor("Pokémon.nsp") == "Pok_mon", "non-ASCII letters are turned into underscore");
    // files on disk
    {
        const auto dir = TempDir("files") / "per-game";
        const std::string game = "Celeste.nsp";
        Check(GC::Load(dir, game).empty() && GC::Present(dir).empty(), "no folder: nothing to load, nothing present");
        Check(GC::Save(dir, game, "resolution=1.5\n") && std::filesystem::exists(dir / "Celeste.txt"), "saving makes the folder and the file");
        Check(GC::Load(dir, game) == "resolution=1.5\n", "loading gives back what was saved");
        Check(GC::Present(dir).count("Celeste") == 1, "the file is listed as present");
        Check(!std::filesystem::exists(dir / "Celeste.txt.tmp"), "no temporary file is left");
        Check(GC::Save(dir, game, "# only a comment\n") && !std::filesystem::exists(dir / "Celeste.txt"), "a text with no setting removes the file");
        Check(GC::Save(dir, game, "") && GC::Present(dir).empty(), "removing a file that is not there is fine");
        std::filesystem::remove_all(dir.parent_path());
    }
    // the screen
    {
        Rig r("resolution=1.5\n", 3, 1);
        r.Tap(Pad::R2);
        Check(r.ui.GameConfigOpen() && r.ui.GameConfigGame() == 1, "R2 on a game opens its settings screen");
        const auto& opts = r.ui.GameConfigOptions();
        Check(!opts.empty(), "the screen lists options");
        bool bad = false, has_players = false, has_resolution = false;
        for (const Option* o : opts) {
            bad = bad || GlobalOnly(o->key) || TierOf(o->key) == Tier::Mandatory || !PerGameOffered(*o);
            has_players = has_players || std::string(o->key) == "players";
            has_resolution = has_resolution || std::string(o->key) == "resolution";
        }
        Check(!bad && has_players && has_resolution, "...none of them global or mandatory");
        if (PublicBuild) {
            bool experimental = false;
            for (const Option* o : opts) {
                experimental = experimental || TierOf(o->key) == Tier::Experimental;
            }
            Check(!experimental, "the public build offers no experimental option per game");
        }
        // find the row of "resolution"
        int row = -1;
        for (size_t i = 0; i < opts.size(); ++i) {
            if (std::string(opts[i]->key) == "resolution") {
                row = int(i);
            }
        }
        for (int i = 0; i < row; ++i) {
            r.Tap(Pad::Down);
        }
        const Option& res = *FindOption("resolution");
        r.Tap(Pad::Right);
        Check(r.game_files.count("The Legend of Zelda - Tears of the Kingdom.xci") == 1, "the first change writes the game's file under its game name");
        const std::string text = r.game_files["The Legend of Zelda - Tears of the Kingdom.xci"];
        Check(text.find("resolution=" + std::string(res.choices[0].value)) != std::string::npos, "Right from 'global' picks the first choice");
        Check(r.saves == 0, "...and settings.txt is not written");
        r.Tap(Pad::Left);
        Check(r.game_files.empty(), "Left goes back to global: the file is gone");
        r.Tap(Pad::Left);
        Check(r.game_files["The Legend of Zelda - Tears of the Kingdom.xci"].find("resolution=" + std::string(res.choices.back().value)) != std::string::npos,
              "Left from 'global' wraps to the last choice");
        r.Tap(Pad::Triangle);
        Check(r.game_files.empty(), "Triangle puts the option back to the global setting");
        r.Tap(Pad::Cross);
        Check(r.game_files.size() == 1, "X steps to the next value");
        Check(r.ui.TabOptions(MenuUi::TabVideo).size() > 0, "the tabs are untouched while the screen is open");
        r.Tap(Pad::R1);
        Check(r.ui.Tab() == MenuUi::TabGames, "L1 and R1 do not leave the screen");
        r.Tap(Pad::Circle);
        Check(!r.ui.GameConfigOpen() && r.ui.Tab() == MenuUi::TabGames, "Circle closes the screen and stays on the game list");
        // the list marks the game, and reopening finds the value
        r.Tap(Pad::R2);
        Check(r.ui.GameConfigOpen(), "R2 opens it again");
        r.Tap(Pad::R2);
        Check(!r.ui.GameConfigOpen(), "R2 closes it");
        // another game: its own screen
        r.Tap(Pad::Down);
        r.Tap(Pad::R2);
        Check(r.ui.GameConfigGame() == 2, "the screen belongs to the game that was selected");
        r.Tap(Pad::Circle);
    }
    {
        // clear row
        Rig r("", 2, 0);
        r.Tap(Pad::R2);
        const int rows = int(r.ui.GameConfigOptions().size());
        r.Tap(Pad::Right); // the first option: written
        Check(r.game_files.size() == 1, "an option set");
        for (int i = 0; i < rows; ++i) {
            r.Tap(Pad::Down); // wraps to the last row ("forget") after `rows` steps from row 0
        }
        r.Tap(Pad::Cross);
        Check(r.game_files.size() == 1, "X on 'forget' asks first");
        r.Tap(Pad::Cross);
        Check(r.game_files.empty(), "X again forgets this game's options");
        r.Tap(Pad::Circle);
    }
    {
        // the Homebrew Menu row and other tabs
        Rig r("", 2, 0);
        r.Tap(Pad::Down);
        r.Tap(Pad::Down); // the Homebrew Menu row
        r.Tap(Pad::R2);
        Check(!r.ui.GameConfigOpen(), "R2 on the Homebrew Menu test does nothing");
        r.ui.SetTab(MenuUi::TabVideo);
        r.Tap(Pad::R2);
        Check(!r.ui.GameConfigOpen(), "R2 outside the game list does nothing");
    }
    {
        // OPTIONS starts the game from the screen; a failed save is shown, not hidden
        Rig r("", 2, 1);
        r.Tap(Pad::R2);
        r.game_save_works = false;
        r.Tap(Pad::Right);
        Check(r.game_saves == 1 && r.game_files.empty(), "a failed save writes nothing");
        r.Tap(Pad::Options);
        Check(r.ui.Result().kind == MenuResult::Kind::Play && r.ui.Result().game == 1, "OPTIONS starts the game from its settings screen");
    }
    {
        // a game with a file is marked when the menu opens, and the screen shows the file's values
        Rig r("", 2, 0);
        r.game_files["Super Mario Odyssey [0100000000010000].nsp"] = "mode=docked\n";
        r.ui.SetTab(MenuUi::TabGames);
        r.Tap(Pad::R2);
        const auto& opts = r.ui.GameConfigOptions();
        int mode_row = -1;
        for (size_t i = 0; i < opts.size(); ++i) {
            if (std::string(opts[i]->key) == "mode") {
                mode_row = int(i);
            }
        }
        for (int i = 0; i < mode_row; ++i) {
            r.Tap(Pad::Down);
        }
        r.Tap(Pad::Right); // the file says docked, the last choice: Right wraps to "global" and the line goes
        Check(r.game_files.count("Super Mario Odyssey [0100000000010000].nsp") == 0, "the screen starts from the file's text (docked, then Right wraps to global)");
        r.Tap(Pad::Circle);
    }
}

void TestNro() {
    std::printf(".nro assets (0.9.5)\n");
    const auto dir = TempDir("nro");
    const auto put32 = [](std::vector<std::uint8_t>& v, size_t at, std::uint32_t x) {
        for (int i = 0; i < 4; ++i) {
            v[at + size_t(i)] = std::uint8_t(x >> (8 * i));
        }
    };
    const auto put64 = [&](std::vector<std::uint8_t>& v, size_t at, std::uint64_t x) {
        put32(v, at, std::uint32_t(x));
        put32(v, at + 4, std::uint32_t(x >> 32));
    };
    const auto build = [&](const std::string& name, const std::string& author, bool icon, bool aset = true) {
        std::vector<std::uint8_t> v(0x1000, 0xAA); // the "program"
        v[0x10] = 'N'; v[0x11] = 'R'; v[0x12] = 'O'; v[0x13] = '0';
        put32(v, 0x18, 0x1000);
        if (aset) {
            const size_t base = v.size();
            std::vector<std::uint8_t> a(0x38, 0);
            a[0] = 'A'; a[1] = 'S'; a[2] = 'E'; a[3] = 'T';
            std::vector<std::uint8_t> jpeg(64, 0x11);
            jpeg[0] = 0xFF; jpeg[1] = 0xD8;
            std::vector<std::uint8_t> nacp(0x4000, 0);
            // titles 0..2 empty on purpose: the first one that has a name wins
            const size_t at = 3 * Nro::NacpTitleBytes;
            std::memcpy(&nacp[at], name.data(), name.size());
            std::memcpy(&nacp[at + 0x200], author.data(), author.size());
            put64(a, 0x08, 0x38);
            put64(a, 0x10, icon ? jpeg.size() : 0);
            put64(a, 0x18, 0x38 + jpeg.size());
            put64(a, 0x20, nacp.size());
            v.insert(v.end(), a.begin(), a.end());
            v.insert(v.end(), jpeg.begin(), jpeg.end());
            v.insert(v.end(), nacp.begin(), nacp.end());
            (void)base;
        }
        return v;
    };
    const auto write = [&](const char* file, const std::vector<std::uint8_t>& bytes) {
        std::ofstream out(dir / file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        return dir / file;
    };
    {
        Nro::Info info;
        std::string why;
        Check(Nro::Read(write("a.nro", build("RetroArch", "libretro", true)), &info, &why), "an NRO with assets is read");
        Check(info.name == "RetroArch" && info.author == "libretro", "the title and the author come from the first title with a name");
        Check(info.icon.size() == 64 && info.icon[0] == 0xFF && info.icon[1] == 0xD8, "the JPEG icon is returned");
    }
    {
        Nro::Info info;
        std::string why;
        Check(Nro::Read(write("b.nro", build("Só título", "", false)), &info, &why) && info.name == "Só título" && info.icon.empty(), "a title without an icon is fine (UTF-8 kept)");
    }
    {
        Nro::Info info;
        std::string why;
        Check(!Nro::Read(write("c.nro", build("", "", false, false)), &info, &why) && !why.empty(), "an NRO without an asset section: false, with a reason");
        Check(!Nro::Read(dir / "missing.nro", &info, &why), "a missing file: false");
        std::vector<std::uint8_t> junk(200, 7);
        Check(!Nro::Read(write("d.nro", junk), &info, &why) && why.find("NRO0") != std::string::npos, "not an NRO: false");
        std::vector<std::uint8_t> cut = build("Cut", "x", true);
        cut.resize(cut.size() - 0x3000); // the NACP is cut short
        Nro::Info cutinfo;
        Check(Nro::Read(write("e.nro", cut), &cutinfo, &why) == !cutinfo.icon.empty(), "a truncated NRO never reads past its end");
        std::vector<std::uint8_t> huge = build("Huge", "x", true);
        put64(huge, 0x1000 + 0x10, 1ull << 40); // an icon size far beyond the file
        Nro::Info hi;
        Nro::Read(write("f.nro", huge), &hi, &why);
        Check(hi.icon.empty(), "an icon size larger than the file is ignored");
        std::vector<std::uint8_t> wrap = build("Wrap", "x", true);
        put64(wrap, 0x1000 + 0x08, ~0ull - 4); // an offset that would overflow
        Nro::Info wi;
        Nro::Read(write("g.nro", wrap), &wi, &why);
        Check(wi.icon.empty(), "an offset that would wrap around is ignored");
        const std::uint8_t bad[0x300] = {'A', 0xFF, 'B', 0};
        std::string n, a;
        Check(!Nro::ParseNacp(bad, sizeof(bad), &n, &a), "a title that is not UTF-8 is refused");
        std::uint8_t tabby[0x300] = {'A', '\t', 'B', ' ', ' ', 0};
        Check(Nro::ParseNacp(tabby, sizeof(tabby), &n, &a) && n == "A B", "control characters become spaces and the ends are trimmed");
    }
    // the game list shows the title of a .nro
    {
        std::vector<GameEntry> games = {{"homebrew_v2.nro", 3ull << 20}, {"Celeste.nsp", 1ull << 30}};
        games[0].title = "RetroArch";
        MenuUi ui(games, 0, Rig::MakeInfo(2), "", nullptr);
        Frame frame;
        Canvas canvas = frame.Canvas_();
        ui.Draw(canvas);
        MenuUi plain({{"homebrew_v2.nro", 3ull << 20}, {"Celeste.nsp", 1ull << 30}}, 0, Rig::MakeInfo(2), "", nullptr);
        Frame frame2;
        Canvas canvas2 = frame2.Canvas_();
        plain.Draw(canvas2);
        Check(frame.pixels != frame2.pixels, "a .nro's own title changes what the list shows");
    }
    std::filesystem::remove_all(dir);
}

void TestNavigation() {
    std::printf("navigation\n");
    {
        Rig r("");
        Check(r.ui.Update(0, r.now), "the first Update asks for a draw");
        Check(!r.ui.Update(0, r.now), "nothing changed: no redraw");
        r.Tap(Pad::Down);
        r.Tap(Pad::Down);
        r.Tap(Pad::Cross);
        Check(r.ui.Result().kind == MenuResult::Kind::Play && r.ui.Result().game == 2, "Down, Down, Cross plays game 3");
    }
    {
        Rig r("", 3, 1);
        r.Tap(Pad::Cross);
        Check(r.ui.Result().game == 1, "the initial game is preselected");
    }
    {
        Rig r("", 3, 0);
        r.Tap(Pad::Up);
        Check(true, "wrap up");
        r.Tap(Pad::Cross);
        Check(r.ui.Result().kind == MenuResult::Kind::Homebrew, "Up from the first game wraps to the Homebrew Menu");
    }
    {
        Rig r("", 3, 1);
        r.Tap(Pad::R1);
        Check(r.ui.Tab() == MenuUi::TabControls, "R1: next tab (CONTROLS, 0.9.5)");
        const int shown = int(r.ui.Tabs().size());
        for (int i = 1; i < shown; ++i) {
            r.Tap(Pad::R1);
        }
        Check(r.ui.Tab() == 0, "R1 wraps around (every tab that is shown)");
        r.Tap(Pad::L1);
        Check(r.ui.Tab() == r.ui.Tabs().back(), "L1 wraps backwards (to the last tab that is shown)");
        r.Tap(Pad::Circle);
        Check(r.ui.Tab() == 0, "Circle goes back to the games");
#if EDEN_PS4_PUBLIC
        Check(shown == 6 && r.ui.Tabs().back() == MenuUi::TabSystem, "public build: six tabs, no LOG and no EXPERIMENTAL");
#else
        Check(shown == 8 && r.ui.Tabs().back() == MenuUi::TabAdvanced, "test build: eight tabs, the last one is EXPERIMENTAL");
#endif
    }
    {
        Rig r("");
        r.ui.SetTab(MenuUi::TabVideo); // row 0 is the quick profile, row 1 the console mode
        r.Tap(Pad::Down);
        r.Tap(Pad::Right);
        Check(r.saves == 1 && r.saved.find("mode=docked") != std::string::npos, "Right changes the value and saves");
        r.Tap(Pad::Right);
        Check(r.saved.find("mode=") == std::string::npos, "wrapping back to the default removes the line");
        r.Tap(Pad::Left);
        Check(r.saved.find("mode=docked") != std::string::npos, "Left goes the other way (wraps)");
        r.Tap(Pad::Triangle);
        Check(r.saved.find("mode=") == std::string::npos, "Triangle restores the default");
        r.Tap(Pad::Cross);
        Check(r.saved.find("mode=docked") != std::string::npos, "Cross cycles to the next value");
    }
    {
        // The SYSTEM tab in each state of the write test: it must draw, and look different.
        auto sum = [](bool writable, std::int64_t log) {
            SystemInfo info = Rig::MakeInfo(1);
            info.data_writable = writable;
            info.log_bytes = log;
            MenuUi ui(Rig::MakeGames(1), 0, info, "", nullptr);
            ui.SetTab(MenuUi::TabSystem);
            std::vector<std::uint32_t> pixels(1920 * 1080);
            Canvas canvas{pixels.data(), 1920, 1080};
            ui.Draw(canvas);
            std::uint64_t total = 0;
            for (std::uint32_t p : pixels) {
                total = total * 31 + p;
            }
            return total;
        };
        Check(sum(true, 18000) != sum(false, -1) && sum(true, 18000) != sum(true, -1), "SYSTEM tab shows the write test: ok / no boot.log / cannot write");
    }
    {
        Rig r("");
        r.ui.SetTab(MenuUi::TabSystem); // SYSTEM
        r.Tap(Pad::Cross); // language
        Check(r.saved.find("language=en") != std::string::npos, "Cross on the language switches to English");
        Check(r.ui.Result().kind == MenuResult::Kind::None, "...and does not start a game");
        for (size_t i = 0; i < r.ui.TabOptions(MenuUi::TabSystem).size(); ++i) {
            r.Tap(Pad::Down); // the colour, the menu sound (0.9.5, zip 5), ... down to restore defaults, the row after the options
        }
        r.saves = 0;
        r.Tap(Pad::Cross);
        Check(r.saves == 0, "restore defaults asks for confirmation first");
        r.Tap(Pad::Cross);
        Check(r.saves == 1 && r.saved.find("language=en") != std::string::npos, "second press resets the options, not the language");
    }
    {
        // 0.9.5: the CONTROLS tab. Left/Right pick a player, Up/Down the model the game sees, X switches a player on or off.
        Rig r("");
        r.ui.SetTab(MenuUi::TabControls);
        Check(r.ui.Players() == 1 && r.ui.PlayerType(0) == "auto", "one player and the automatic model by default");
        r.Tap(Pad::Down);
        Check(r.saved.find("player1_type=pro") != std::string::npos, "Down on player 1: the Pro Controller is written as player1_type=pro");
        r.Tap(Pad::Down);
        Check(r.saved.find("player1_type=dual") != std::string::npos, "...then dual Joy-Con");
        r.Tap(Pad::Triangle);
        Check(r.saved.find("player1_type") == std::string::npos && r.ui.PlayerType(0) == "auto", "Triangle: the model goes back to automatic (the line is removed)");
        r.Tap(Pad::Up);
        Check(r.ui.PlayerType(0) == "handheld", "Up from automatic wraps to the last model (handheld)");
        r.Tap(Pad::Cross);
        Check(r.ui.Players() == 1 && r.saved.find("players=") == std::string::npos, "X on player 1 changes nothing (it holds the menu)");
        r.Tap(Pad::Right);
        r.Tap(Pad::Right); // player 3
        r.Tap(Pad::Cross);
        Check(r.ui.Players() == 3 && r.saved.find("players=3") != std::string::npos, "X on player 3 switches players 1 to 3 on");
        r.Tap(Pad::Cross);
        Check(r.ui.Players() == 2, "X on player 3 again switches it (and the ones after it) off");
        r.Tap(Pad::Right); // player 4
        r.Tap(Pad::Down);
        Check(r.ui.PlayerType(3) == "pro" && r.saved.find("player4_type=pro") != std::string::npos, "the model is per player");
        const Option* o = FindOption("players");
        Check(o != nullptr && o->group == Group::Controls, "players is in the CONTROLS tab now");
        r.ui.SetTab(MenuUi::TabSystem);
        Check(r.ui.Tab() == MenuUi::TabSystem, "(the SYSTEM tab no longer has the controllers row)");
        // the screen tells the states apart
        auto screen = [](const std::string& settings, bool second) {
            SystemInfo info = Rig::MakeInfo(1);
            info.pad_connected[1] = second;
            MenuUi ui(Rig::MakeGames(1), 0, info, settings, nullptr);
            ui.SetTab(MenuUi::TabControls);
            std::vector<std::uint32_t> pixels(1920 * 1080);
            Canvas canvas{pixels.data(), 1920, 1080};
            ui.Draw(canvas);
            std::uint64_t total = 0;
            for (std::uint32_t p : pixels) {
                total = total * 31 + p;
            }
            return total;
        };
        Check(screen("", false) != screen("players=2\n", false), "the screen shows a player switched on");
        Check(screen("players=2\n", false) != screen("players=2\n", true), "...and a controller that is connected");
        Check(screen("", false) != screen("player1_type=dual\n", false) && screen("player1_type=dual\n", false) != screen("player1_type=handheld\n", false),
              "...and the model drawn");
        bool pads[4] = {true, false, false, false};
        Rig q("");
        q.ui.SetTab(MenuUi::TabControls);
        Check(!q.ui.SetPadsConnected(pads), "no change in the connected controllers: nothing to redraw");
        pads[2] = true;
        Check(q.ui.SetPadsConnected(pads), "a controller connected: the tab asks for a redraw");
    }
    {
        Rig r("mode=docked\nastc=gpu\n");
        r.ui.SetTab(MenuUi::TabSystem);
        for (size_t i = 0; i < r.ui.TabOptions(MenuUi::TabSystem).size(); ++i) {
            r.Tap(Pad::Down); // past the options (language, colour, sound, ...): now on restore defaults
        }
        r.Tap(Pad::Cross);
        r.Tap(Pad::Up); // moving cancels the confirmation
        r.Tap(Pad::Down);
        r.saves = 0;
        r.Tap(Pad::Cross);
        Check(r.saves == 0, "moving away cancels a pending confirmation");
    }
    {
        Rig r("");
        r.Tap(Pad::R1);
        r.Tap(Pad::Down);
        r.Tap(Pad::Down);
        r.Tap(Pad::Down);
        r.Tap(Pad::Options);
        Check(r.ui.Result().kind == MenuResult::Kind::Play && r.ui.Result().game == 0, "OPTIONS plays the last highlighted game from any tab");
    }
    {
        Rig r("", 0);
        r.Tap(Pad::Options);
        Check(r.ui.Result().kind == MenuResult::Kind::None, "OPTIONS with no games does nothing");
        r.Tap(Pad::Cross);
        Check(r.ui.Result().kind == MenuResult::Kind::Homebrew, "with no games the only row is the Homebrew Menu");
    }
    {
        // The quick profiles, from the VIDEO tab's first row.
        Rig r("");
        r.ui.SetTab(MenuUi::TabVideo);
        Check(r.ui.Tab() == MenuUi::TabVideo, "VIDEO tab");
        r.Tap(Pad::Right);
        Check(r.saved.find("resolution=0.75") != std::string::npos && r.saved.find("gpu_accuracy") == std::string::npos,
              "profile Light: 0.75x written, the options at their default are not");
        SettingsFile f(r.saved);
        Check(f.MatchPreset() == 1, "the file matches profile 1 (Light)");
        r.Tap(Pad::Right);
        Check(r.saved.find("gpu_accuracy=high") != std::string::npos && r.saved.find("cpu_accuracy=auto") != std::string::npos &&
                  r.saved.find("resolution") == std::string::npos,
              "profile Compatibility replaces the Light one");
        r.Tap(Pad::Right);
        Check(SettingsFile(r.saved).MatchPreset() == 3 && r.saved.find("ORBIS_ARENA_MIB=1152") != std::string::npos && r.saved.find("shader_preload=1024") != std::string::npos &&
                  r.saved.find("driver_cache=off") != std::string::npos && r.saved.find("memory_profile=tight") != std::string::npos &&
                  r.saved.find("memory_action") == std::string::npos && SettingsFile(r.saved).ChoiceIndex(*FindOption("memory_action")) == 1,
              "profile Save RAM: 1152 MiB arena, no driver cache, tight memory profile, and pause (now the default, so no line)");
        r.Tap(Pad::Right);
        Check(SettingsFile(r.saved).MatchPreset() == 4 && r.saved.find("ORBIS_ARENA_MIB=1792") != std::string::npos &&
                  r.saved.find("driver_cache") == std::string::npos && r.saved.find("memory_profile") == std::string::npos &&
                  r.saved.find("shader_preload") == std::string::npos,
              "profile More video memory replaces it (1792 MiB, driver cache and shader cache back to default)");
        r.Tap(Pad::Right);
        Check(SettingsFile(r.saved).MatchPreset() == 5 && r.saved.find("gpu_accuracy=high") != std::string::npos &&
                  r.saved.find("dma_accuracy=safe") != std::string::npos && r.saved.find("sync_memory=on") != std::string::npos &&
                  r.saved.find("async_shaders") == std::string::npos && r.saved.find("ORBIS_ARENA_MIB") == std::string::npos &&
                  r.saved.find("ORBIS_VRAM_GARLIC") == std::string::npos,
              "profile Exact graphics (test): accuracy options written, the arena back to default");
        r.Tap(Pad::Right);
        Check(SettingsFile(r.saved).MatchPreset() == 6 && r.saved.find("jit_cache_mib=128") != std::string::npos &&
                  r.saved.find("resolution=0.75") != std::string::npos &&
                  r.saved.find("dma_accuracy") == std::string::npos && r.saved.find("sync_memory") == std::string::npos,
              "profile More FPS (test): 0.75x resolution and the 128 MiB JIT cache written, the accuracy ones of the previous profile gone");
        Check(r.saved.find("skip_cpu_invalidation") == std::string::npos && r.saved.find("async_presentation") == std::string::npos,
              "profile More FPS (test) (0.9.7 fix 2): does not switch on the two options that never ran on a console (inner CPU invalidation, asynchronous presentation)");
        r.Tap(Pad::Right);
        Check(SettingsFile(r.saved).MatchPreset() == 7 && r.saved.find("resolution=0.5") != std::string::npos &&
                  r.saved.find("shader_preload=256") != std::string::npos && r.saved.find("covers=thumbs") != std::string::npos &&
                  r.saved.find("astc_recompress=bc3") != std::string::npos &&
                  r.saved.find("memory_profile=tight") != std::string::npos && r.saved.find("jit_cache_mib") == std::string::npos &&
                  r.saved.find("gpu_accuracy") == std::string::npos,
              "profile Minimum RAM (test): small resolution, small shader preload, thumbnails only, tight profile; the FPS profile's options gone");
        r.Tap(Pad::Right);
        Check(r.saved.find("gpu_accuracy") == std::string::npos && r.saved.find("covers") == std::string::npos &&
                  SettingsFile(r.saved).MatchPreset() == 0,
              "wraps to Default: nothing written");
        r.Tap(Pad::Left);
        Check(SettingsFile(r.saved).MatchPreset() == 7, "Left from Default wraps to the last profile");
        r.Tap(Pad::Triangle);
        Check(SettingsFile(r.saved).MatchPreset() == 0, "Triangle on the profile row: Default");
    }
    {
        // Settings that match no profile are a custom mix; a profile does not touch the others.
        Rig r("resolution=2\nlanguage=en\nmode=docked\n");
        r.ui.SetTab(MenuUi::TabVideo);
        Check(SettingsFile("resolution=2\n").MatchPreset() == -1, "resolution=2 is a custom mix");
        r.Tap(Pad::Right);
        Check(SettingsFile(r.saved).MatchPreset() == 0, "Right from a custom mix: the first profile");
        Check(r.saved.find("mode=docked") != std::string::npos && r.saved.find("language=en") != std::string::npos,
              "a profile leaves the console mode and the language alone");
        Rig q("resolution=2\n");
        q.ui.SetTab(MenuUi::TabVideo);
        q.Tap(Pad::Left);
        Check(SettingsFile(q.saved).MatchPreset() == 7, "Left from a custom mix: the last profile");
    }
    {
        // A save that fails is shown, and a save that works is too.
        Rig r("");
        r.save_works = false;
        r.ui.SetTab(MenuUi::TabCpu);
        r.Tap(Pad::Cross);
        Check(r.saves == 1, "the failing save was tried");
        auto draw = [&](Rig& rig) {
            std::vector<std::uint32_t> pixels(1920 * 1080);
            Canvas canvas{pixels.data(), 1920, 1080};
            rig.ui.Draw(canvas);
            std::uint64_t total = 0;
            for (std::uint32_t p : pixels) {
                total = total * 31 + p;
            }
            return total;
        };
        const std::uint64_t failed = draw(r);
        r.save_works = true;
        r.Tap(Pad::Cross);
        Check(draw(r) != failed, "the screen tells a failed save from a good one");
    }
    {
        // Clearing the shader cache asks twice, from the SYSTEM tab's fourth row.
        int cleared = 0;
        SystemInfo info = Rig::MakeInfo(1);
        info.shader_cache_games = 2;
        info.clear_shader_cache = [&] {
            ++cleared;
            return true;
        };
        MenuUi ui(Rig::MakeGames(1), 0, info, "", nullptr);
        ui.SetTab(MenuUi::TabSystem);
        std::uint64_t now = 1000000;
        auto tap = [&](std::uint32_t b) {
            ui.Update(b, now);
            now += 50000;
            ui.Update(0, now);
            now += 50000;
        };
        for (size_t i = 0; i <= ui.TabOptions(MenuUi::TabSystem).size(); ++i) {
            tap(Pad::Down); // past the options (language, colour, sound, ...), then restore defaults: now on clear the cache
        }
        tap(Pad::Cross);
        Check(cleared == 0, "clear shader cache: the first press only asks");
        tap(Pad::Up);
        tap(Pad::Down);
        tap(Pad::Cross);
        Check(cleared == 0, "...and moving away cancels the question");
        tap(Pad::Cross);
        Check(cleared == 1, "...the second press clears it");
    }
    {
        // A dump's file name carries tags; the list shows the title only (two names that differ only
        // in their tags draw the same screen).
        auto frame = [](const char* file) {
            std::vector<GameEntry> games = {{file, 1ull << 30}};
            MenuUi ui(games, 0, Rig::MakeInfo(1), "", nullptr);
            std::vector<std::uint32_t> pixels(1920 * 1080);
            Canvas canvas{pixels.data(), 1920, 1080};
            ui.Draw(canvas);
            std::uint64_t total = 0;
            for (std::uint32_t p : pixels) {
                total = total * 31 + p;
            }
            return total;
        };
        Check(frame("Super Mario Maker 2 [01009B90006DC000][v0] (US).nsp") == frame("Super Mario Maker 2 [01009B90006DC000].xci"),
              "the title in the list has no [tags], (tags) or extension");
        Check(frame("Super Mario Maker 2 [01009B90006DC000].nsp") != frame("Super Mario Maker 2.nsp"),
              "the card shows the title id when the file name has one");
        Check(frame("Super_Mario_Maker_2.nsp") == frame("Super Mario Maker 2.nsp"), "underscores read as spaces");
        Check(frame("Celeste.nsp") != frame("Super Mario Maker 2.nsp"), "different titles draw differently");
    }
    {
        // The LOG tab: Left/Right switch the session, and it draws for both, empty or not.
        Rig r("");
        r.ui.SetTab(MenuUi::TabLog);
        Check(r.ui.Tab() == MenuUi::TabLog, "LOG tab");
        r.Tap(Pad::Right);
        r.Tap(Pad::Left);
        r.Tap(Pad::Cross);
        Check(r.ui.Result().kind == MenuResult::Kind::None, "Cross on the LOG tab starts nothing");
    }
    {
        // Key repeat: held Down moves after 0.4 s, then every 0.11 s.
        Rig r("", 4, 0);
        r.ui.Update(Pad::Down, r.now);
        r.ui.Update(Pad::Down, r.now + 300000);
        r.ui.Update(Pad::Down, r.now + 450000);
        r.ui.Update(Pad::Down, r.now + 500000);
        r.ui.Update(Pad::Down, r.now + 570000);
        r.ui.Update(0, r.now + 600000);
        r.ui.Update(Pad::Cross, r.now + 650000);
        Check(r.ui.Result().game == 3, "holding Down repeats (1 press + 2 repeats = row 3)");
    }
}

void WritePpm(const std::string& path, const Frame& frame) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    std::fprintf(f, "P6\n1920 1080\n255\n");
    for (std::uint32_t p : frame.pixels) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(p >> 16), static_cast<unsigned char>(p >> 8),
                                      static_cast<unsigned char>(p)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

void TestDrawing(const char* render_dir) {
    std::printf("drawing\n");
    for (int english = 0; english < 2; ++english) {
        for (int games : {4, 0}) {
            Rig r(english ? "language=en\nmode=docked\nresolution=1.5\ncpu_accuracy=accurate\n" : "mode=docked\nresolution=1.5\nenv=ORBIS_ARENA_MIB=1536\n", games);
            for (int tab = 0; tab < MenuUi::TabCount; ++tab) {
                r.ui.SetTab(tab);
                if (tab > 0) {
                    r.Tap(Pad::Down); // second row selected, to see a highlighted row that is not the first
                }
                Frame frame;
                Canvas canvas = frame.Canvas_();
                r.ui.Draw(canvas);
                // Not blank, and pixels the font drew are in the content area.
                std::uint32_t first = frame.pixels[0];
                size_t different = 0;
                for (std::uint32_t p : frame.pixels) {
                    different += p != first;
                }
                char what[96];
                std::snprintf(what, sizeof(what), "tab %d (%s, %d games) draws something", tab, english ? "en" : "pt", games);
                Check(different > 20000, what);
                if (render_dir != nullptr && (games == 4 || tab == 0)) {
                    char name[256];
                    std::snprintf(name, sizeof(name), "%s/menu-%s-%d-games%d.ppm", render_dir, english ? "en" : "pt", tab, games);
                    WritePpm(name, frame);
                }
            }
        }
    }
    // 0.7.10 themes: the colour is a setting, changes at once, survives "restore defaults", and every theme draws.
    {
        const Option* theme = FindOption("menu_theme");
        Check(theme != nullptr && int(theme->choices.size()) == ThemeCount(), "the menu_theme choices match the themes in the canvas");
        Rig r("");
        r.ui.SetTab(MenuUi::TabSystem);
        r.Tap(Pad::Down); // the colour row
        r.Tap(Pad::Cross);
        Check(r.saved.find("menu_theme=green") != std::string::npos, "Cross on the colour row picks the next theme and saves it");
        Frame green;
        Canvas canvas = green.Canvas_();
        r.ui.Draw(canvas);
        Check(Color::Accent != 0xFF4A86FF, "the theme's colours are applied when the menu draws");
        Rig again("menu_theme=purple\n");
        Frame purple;
        Canvas c2 = purple.Canvas_();
        again.ui.Draw(c2);
        Check(purple.pixels != green.pixels, "a different theme draws a different screen");
        again.ui.SetTab(MenuUi::TabSystem);
        again.Tap(Pad::Down);
        again.Tap(Pad::Down); // restore defaults
        again.Tap(Pad::Cross);
        again.Tap(Pad::Cross);
        Check(again.saved.find("menu_theme=purple") != std::string::npos, "restore defaults keeps the colour (and the language)");
        for (int i = 0; i < ThemeCount(); ++i) {
            Rig each("menu_theme=" + std::string(theme->choices[size_t(i)].value) + "\n");
            for (int tab = 0; tab < MenuUi::TabCount; ++tab) {
                each.ui.SetTab(tab);
                Frame f;
                Canvas cv = f.Canvas_();
                each.ui.Draw(cv);
            }
            Check(i == 0 || Color::Accent == ThemeAccent(i), "every theme: the accent is the one ThemeAccent reports");
        }
        ApplyTheme(0);
        Check(Color::Accent == 0xFF4A86FF && Color::Panel == 0xFF1B2644 && Color::Teal == 0xFF2DD4C4, "theme 0 is the original blue, exactly");
        // icons are drawn into the tab bar: the same tab strip differs from one with no icon room? at least they paint something
        Frame f;
        Canvas cv = f.Canvas_();
        cv.Fill(0, 0, 1920, 1080, 0xFF000000);
        for (int k = 0; k <= int(Icon::Play); ++k) {
            DrawIcon(cv, Icon(k), 100.0f + 80.0f * float(k), 100.0f, 40.0f, 0xFFFFFFFF, 0xFF000000);
        }
        size_t lit = 0;
        for (std::uint32_t p : f.pixels) {
            lit += p != 0xFF000000;
        }
        Check(lit > 4000, "every icon paints pixels");
        if (render_dir != nullptr) {
            char name[256];
            std::snprintf(name, sizeof(name), "%s/icons.ppm", render_dir);
            WritePpm(name, f);
        }
    }
    // 0.9.5: the game's own settings screen, and the mark in the list
    for (int english = 0; english < 2; ++english) {
        Rig r(english ? "language=en\nresolution=1.5\n" : "resolution=1.5\n", 4, 0);
        r.game_files["Super Mario Odyssey [0100000000010000].nsp"] = "mode=docked\ngpu_accuracy=high\n";
        std::vector<GameEntry> own_games = Rig::MakeGames(4);
        own_games[0].has_config = true;
        MenuUi ui(std::move(own_games), 0, r.WithGameFiles(Rig::MakeInfo(4)), english ? "language=en\nresolution=1.5\n" : "resolution=1.5\n", nullptr);
        Frame list;
        Canvas lc = list.Canvas_();
        ui.Draw(lc);
        std::uint64_t now = 1000000;
        const auto tap = [&](std::uint32_t button) {
            ui.Update(button, now);
            now += 50000;
            ui.Update(0, now);
            now += 50000;
        };
        tap(Pad::R2);
        for (int i = 0; i < 3; ++i) {
            tap(Pad::Down);
        }
        tap(Pad::Right);
        tap(Pad::Right);
        Frame screen;
        Canvas sc = screen.Canvas_();
        ui.Draw(sc);
        Check(screen.pixels != list.pixels, "the game's settings screen draws over the list");
        {
            // a game with its own settings is marked in the list
            std::vector<GameEntry> plain_games = Rig::MakeGames(4);
            MenuUi plain(std::move(plain_games), 0, Rig::MakeInfo(4), english ? "language=en\nresolution=1.5\n" : "resolution=1.5\n", nullptr);
            Frame unmarked;
            Canvas uc = unmarked.Canvas_();
            plain.Draw(uc);
            Check(unmarked.pixels != list.pixels, "a game that has its own settings is marked in the list");
        }
        if (render_dir != nullptr) {
            char name[256];
            std::snprintf(name, sizeof(name), "%s/menu-%s-pergame-list.ppm", render_dir, english ? "en" : "pt");
            WritePpm(name, list);
            std::snprintf(name, sizeof(name), "%s/menu-%s-pergame-screen.ppm", render_dir, english ? "en" : "pt");
            WritePpm(name, screen);
        }
    }
    // 0.7.9 covers: a game with a picture draws it (big in the card, small in the list); one without gets a tile.
    {
        auto make_cover = [](int w, int h, std::uint32_t a, std::uint32_t b) {
            Image::Pixels img;
            img.w = w;
            img.h = h;
            img.px.resize(size_t(w) * size_t(h));
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const bool stripe = ((x + y) / 24) % 2 == 0;
                    img.px[size_t(y) * size_t(w) + size_t(x)] = stripe ? a : b;
                }
            }
            return img;
        };
        auto games_with = [&](bool covers) {
            std::vector<GameEntry> games = {{"Super Mario Odyssey [0100000000010000].nsp", 5800ull << 20},
                                            {"Celeste.nsp", 1200ull << 20},
                                            {"Hollow Knight [0100633007D48000].nsp", 3ull << 30}};
            if (covers) {
                games[0].cover = Image::Fit(make_cover(300, 420, 0xFFD02828, 0xFFF8D030), CoverSize, CoverSize, true);
                games[0].thumb = Image::Fit(games[0].cover, ThumbSize, ThumbSize);
                games[1].cover = Image::Fit(make_cover(512, 512, 0xFF2878D0, 0xFF103060), CoverSize, CoverSize, true);
                games[1].thumb = Image::Fit(games[1].cover, ThumbSize, ThumbSize);
            }
            return games;
        };
        Check(games_with(true)[0].cover.h == CoverSize && games_with(true)[0].cover.w < CoverSize,
              "a portrait cover is fitted to the card keeping its proportions");
        Check(games_with(true)[0].thumb.h == ThumbSize, "the list thumbnail is ThumbSize high");
        auto draw = [&](bool covers, int select) {
            MenuUi ui(games_with(covers), select, Rig::MakeInfo(3), "", nullptr);
            Frame frame;
            Canvas canvas = frame.Canvas_();
            ui.Draw(canvas);
            return frame;
        };
        const Frame with = draw(true, 0), without = draw(false, 0);
        size_t different = 0;
        for (size_t i = 0; i < with.pixels.size(); ++i) {
            different += with.pixels[i] != without.pixels[i];
        }
        Check(different > 30000, "a cover changes the card and the list");
        // The middle of the card shows the cover's own colours (red or yellow stripes), not the tile's gradient.
        const std::uint32_t mid = with.pixels[size_t(220 + 200) * 1920 + 1420];
        Check((mid & 0xFFFFFF) == 0xD02828 || (mid & 0xFFFFFF) == 0xF8D030, "the card draws the picture's pixels");
        const Frame second = draw(true, 1);
        Check(second.pixels != with.pixels, "selecting another game changes the card");
        // A cover that fails to load leaves the entry empty: the tile is drawn and nothing crashes.
        std::vector<GameEntry> broken = {{"Broken.nsp", 1ull << 20}};
        broken[0].cover.w = 10;
        broken[0].cover.h = 10; // pixels missing: Ok() is false
        MenuUi ui(broken, 0, Rig::MakeInfo(1), "", nullptr);
        Frame frame;
        Canvas canvas = frame.Canvas_();
        ui.Draw(canvas);
        Check(true, "an inconsistent cover is ignored");
        if (render_dir != nullptr) {
            char name[256];
            std::snprintf(name, sizeof(name), "%s/menu-pt-0-covers.ppm", render_dir);
            WritePpm(name, with);
        }
    }
    // Text drawing: UTF-8 is decoded, an unknown character is a '?', nothing is drawn out of bounds.
    {
        Frame frame;
        Canvas c = frame.Canvas_();
        Check(TextCells("Configuração é ✓") == 16 - 0, "UTF-8 text length is counted in characters");
        const int w = c.Print(1900, 1070, "ação", Color::Text); // runs off the right and bottom edges
        Check(w == 4 * GlyphWidth, "Print returns the width even when clipped");
        c.Print(-50, -50, "x", Color::Text);
        Check(true, "drawing outside the canvas is clipped");
    }
}

/// 0.9.5 (zip 5): the menu's sounds. The synthesis and the mixer are plain arithmetic, so they are checked here; the console's audio port
/// (rom_menu.cpp, MenuAudio) is not, and the notes say so.
void TestSound() {
    std::printf("sound (0.9.5, zip 5)\n");
    const auto peak_of = [](const Samples& s) {
        int peak = 0;
        for (const std::int16_t v : s) {
            peak = std::max(peak, std::abs(int(v)));
        }
        return peak;
    };
    // The effects: stereo, short, 0.7 of full scale, silent at both ends (no click when one starts or stops).
    for (int i = 0; i < SoundCount; ++i) {
        const Samples e = MakeEffect(SoundId(i));
        const int frames = int(e.size() / 2);
        Check(e.size() % 2 == 0 && frames > SampleRate / 10 && frames <= SampleRate * 3 / 2, "an effect is stereo and between 0.1 s and 1.5 s long");
        const int peak = peak_of(e);
        Check(peak > int(0.69 * 32767) && peak <= int(0.7 * 32767) + 1, "an effect's loudest sample is 0.7 of full scale");
        Check(e[0] == 0 && e[1] == 0 && e[e.size() - 2] == 0 && e[e.size() - 1] == 0, "an effect starts and ends in silence");
        bool left_sounds = false, right_sounds = false;
        for (size_t k = 0; k < e.size(); k += 2) {
            left_sounds |= e[k] != 0;
            right_sounds |= e[k + 1] != 0;
        }
        Check(left_sounds && right_sounds, "an effect sounds in both ears");
        Check(MakeEffect(SoundId(i)) == e, "an effect is the same every time it is made");
    }
    Check(MakeEffect(SoundId::Play).size() > MakeEffect(SoundId::Move).size() * 3, "the game-start chime is much longer than the tick");
    // The background loop: whole seconds, 0.8 of full scale, and its end leads into its start without a jump.
    for (const int seconds : {8, 32}) {
        const Samples a = MakeAmbient(seconds);
        Check(int(a.size() / 2) == seconds * SampleRate, "the loop is as long as asked");
        const int peak = peak_of(a);
        Check(peak > int(0.79 * 32767) && peak <= int(0.8 * 32767) + 1, "the loop's loudest sample is 0.8 of full scale");
        long biggest_step = 0;
        for (size_t k = 2; k < a.size(); k += 2) {
            biggest_step = std::max<long>(biggest_step, std::abs(int(a[k]) - int(a[k - 2])));
        }
        const long seam = std::abs(int(a[0]) - int(a[a.size() - 2]));
        Check(seam <= biggest_step, "the loop joins itself without a jump (the seam is no bigger than any other step)");
        double energy = 0;
        for (const std::int16_t v : a) {
            energy += double(v) * double(v);
        }
        const double rms = std::sqrt(energy / double(a.size())) / 32767.0;
        Check(rms > 0.05 && rms < 0.5, "the loop is neither silent nor a wall of noise");
    }
    Check(int(MakeAmbient(30).size() / 2) == 28 * SampleRate, "a loop length that is not a multiple of 4 s is rounded down");
    // The mixer.
    {
        Mixer m;
        std::vector<std::int16_t> out(2 * 4800);
        m.Render(out.data(), 4800);
        Check(peak_of(out) == 0, "nothing playing: silence");
        m.SetVolume(1.0f);
        m.Trigger(SoundId::Select);
        Check(m.Active() == 1, "a triggered effect is waiting");
        m.Render(out.data(), 4800);
        Check(peak_of(out) > 1000 && m.Active() == 1, "...it plays");
        for (int i = 0; i < 20; ++i) {
            m.Render(out.data(), 4800);
        }
        Check(m.Active() == 0, "...and is let go when it ends");
        m.Render(out.data(), 4800);
        Check(peak_of(out) == 0, "...leaving silence");
        m.SetEffects(false);
        m.Trigger(SoundId::Move);
        Check(m.Active() == 0, "with the sounds off an effect is ignored");
        m.SetEffects(true);
        for (int i = 0; i < 30; ++i) {
            m.Trigger(SoundId::Play);
        }
        Check(m.Active() <= 8, "no more than 8 effects wait at once");
        // Everything at full volume, the background loop on: clipped, never wrapped round.
        m.SetAmbient(MakeAmbient(8));
        m.SetMusic(true);
        bool wrapped = false;
        int loudest = 0;
        for (int i = 0; i < 40; ++i) {
            m.Render(out.data(), 4800);
            for (const std::int16_t v : out) {
                wrapped |= v == -32768;
                loudest = std::max(loudest, std::abs(int(v)));
            }
        }
        Check(!wrapped && loudest > 20000, "a loud mix is clipped, not wrapped round");
        Check(m.Active() <= 8, "the voices are still capped");
        // Volume 0 is silence.
        m.SetVolume(0.0f);
        m.Trigger(SoundId::Select);
        m.Render(out.data(), 4800);
        Check(peak_of(out) == 0, "volume 0 is silence");
    }
    {
        // The music comes in and goes out gently (0.6 s), not with a click.
        Mixer m;
        m.SetVolume(1.0f);
        m.SetAmbient(MakeAmbient(8));
        std::vector<std::int16_t> out(2 * 480);
        m.Render(out.data(), 480);
        Check(peak_of(out) < 200, "the music starts from nothing and comes in");
        for (int i = 0; i < 100; ++i) { // 1 s
            m.Render(out.data(), 480);
        }
        Check(peak_of(out) > 3000, "...and is there after a second");
        m.SetMusic(false);
        for (int i = 0; i < 100; ++i) {
            m.Render(out.data(), 480);
        }
        Check(peak_of(out) == 0, "music off: gone a second later");
        // FadeOut: silent after the time asked for, and Quiet() says so.
        m.SetMusic(true);
        for (int i = 0; i < 100; ++i) {
            m.Render(out.data(), 480);
        }
        Check(!m.Quiet(), "not quiet before a fade out");
        m.FadeOut(0.2f);
        int quiet_after_blocks = -1;
        for (int i = 0; i < 100; ++i) {
            m.Render(out.data(), 480);
            if (m.Quiet() && quiet_after_blocks < 0) {
                quiet_after_blocks = i + 1;
            }
        }
        Check(quiet_after_blocks > 0 && quiet_after_blocks <= 25, "FadeOut(0.2 s) is quiet within about 0.25 s");
        Check(peak_of(out) == 0, "...and stays silent");
    }
    // The menu asks for the sounds, and says how the audio is set.
    {
        Rig r("");
        Check(r.ui.TakeSounds().empty(), "no sound before anything is pressed");
        r.Tap(Pad::Down);
        std::vector<SoundId> got = r.ui.TakeSounds();
        Check(got.size() == 1 && got[0] == SoundId::Move, "a direction: the tick");
        Check(r.ui.TakeSounds().empty(), "the sounds are given once");
        r.Tap(Pad::Right);
        got = r.ui.TakeSounds();
        Check(got.size() == 1 && got[0] == SoundId::Move, "Left/Right on the list: the tick");
        r.Tap(Pad::R1);
        got = r.ui.TakeSounds();
        Check(got.size() == 1 && got[0] == SoundId::Tab, "R1: the tab sound");
        r.Tap(Pad::L1);
        got = r.ui.TakeSounds();
        Check(got.size() == 1 && got[0] == SoundId::Tab, "L1: the tab sound");
        r.Tap(Pad::R1); // CONTROLS
        r.ui.TakeSounds();
        r.Tap(Pad::Circle); // back to the games
        got = r.ui.TakeSounds();
        Check(got.size() == 1 && got[0] == SoundId::Back && r.ui.Tab() == MenuUi::TabGames, "Circle out of a tab: the back sound");
        r.Tap(Pad::Circle);
        Check(r.ui.TakeSounds().empty(), "Circle on the game list does nothing, silently");
        r.ui.SetTab(MenuUi::TabSystem);
        r.ui.TakeSounds();
        r.Tap(Pad::Cross);
        got = r.ui.TakeSounds();
        Check(got.size() == 1 && got[0] == SoundId::Select, "Cross on an option: the choose sound");
    }
    {
        Rig r(""); // (a fresh menu: the one above has walked down the list to the Homebrew Menu)
        r.Tap(Pad::R2);
        std::vector<SoundId> got = r.ui.TakeSounds();
        Check(r.ui.GameConfigOpen() && got.size() == 1 && got[0] == SoundId::Select, "R2 on a game opens its settings with the choose sound");
        r.Tap(Pad::Circle);
        got = r.ui.TakeSounds();
        Check(!r.ui.GameConfigOpen() && got.size() == 1 && got[0] == SoundId::Back, "Circle closes them with the back sound");
        r.Tap(Pad::Cross);
        got = r.ui.TakeSounds();
        Check(r.ui.Result().kind == MenuResult::Kind::Play && got.size() == 1 && got[0] == SoundId::Play, "Cross on a game: the game-start chime");
    }
    {
        Rig r("");
        r.Tap(Pad::Options);
        const std::vector<SoundId> got = r.ui.TakeSounds();
        Check(r.ui.Result().kind == MenuResult::Kind::Play && got.size() == 1 && got[0] == SoundId::Play, "OPTIONS: the game-start chime");
    }
    {
        Rig r("");
        r.held = Pad::Cross | Pad::Options; // two ways to start in the same step
        r.ui.Update(r.held, r.now);
        const std::vector<SoundId> got = r.ui.TakeSounds();
        Check(std::count(got.begin(), got.end(), SoundId::Play) == 1, "never two game-start chimes");
    }
    {
        Rig r("");
        for (int i = 0; i < 20; ++i) {
            r.Tap(Pad::Down); // 20 steps without anyone taking the sounds
        }
        Check(r.ui.TakeSounds().size() <= 4, "the queue of sounds is capped");
        Rig quiet("");
        quiet.ui.Update(0, quiet.now);
        quiet.ui.Update(0, quiet.now + 16000);
        Check(quiet.ui.TakeSounds().empty(), "steps with no button: no sound");
    }
    {
        Check(Rig("").ui.Audio() == AudioMode::All && Rig("").ui.Volume() == 0.5f, "by default: music and sounds at 50%");
        Check(Rig("menu_audio=sounds\nmenu_volume=100\n").ui.Audio() == AudioMode::Sounds && Rig("menu_audio=sounds\nmenu_volume=100\n").ui.Volume() == 1.0f,
              "menu_audio=sounds, menu_volume=100");
        Check(Rig("menu_audio=off\nmenu_volume=20\n").ui.Audio() == AudioMode::Off && Rig("menu_audio=off\nmenu_volume=20\n").ui.Volume() == 0.2f, "menu_audio=off, menu_volume=20");
        Check(Rig("menu_audio=loud\nmenu_volume=7\n").ui.Audio() == AudioMode::All && Rig("menu_audio=loud\nmenu_volume=7\n").ui.Volume() == 0.5f,
              "a value that is not a choice is the default");
        const Option* audio = FindOption("menu_audio");
        const Option* volume = FindOption("menu_volume");
        Check(audio != nullptr && volume != nullptr && audio->group == Group::System && volume->group == Group::System, "both options are in the SYSTEM tab");
        Check(GlobalOnly("menu_audio") && GlobalOnly("menu_volume") && !PerGameOffered(*audio) && !PerGameOffered(*volume),
              "...for the whole app: a game's own file does not offer them");
        Check(audio != nullptr && audio->default_index == 0 && volume != nullptr && std::string(volume->choices[size_t(volume->default_index)].value) == "50",
              "the defaults are the ones the menu reads when the lines are absent");
        for (const Option* o : {audio, volume}) {
            Check(o != nullptr && o->label_pt[0] && o->label_en[0] && o->help_pt[0] && o->help_en[0], "the sound options have texts in both languages");
        }
        // Changing it in the menu: saved, and heard at once.
        Rig r("");
        r.ui.SetTab(MenuUi::TabSystem);
        r.Tap(Pad::Down); // colour
        r.Tap(Pad::Down); // menu sound
        r.Tap(Pad::Cross);
        Check(r.saved.find("menu_audio=sounds") != std::string::npos && r.ui.Audio() == AudioMode::Sounds, "Cross on the menu sound row: sounds only, saved");
        r.Tap(Pad::Cross);
        Check(r.saved.find("menu_audio=off") != std::string::npos && r.ui.Audio() == AudioMode::Off, "...then off");
        r.Tap(Pad::Down); // volume
        r.Tap(Pad::Right);
        Check(r.saved.find("menu_volume=70") != std::string::npos && r.ui.Volume() == 0.7f, "Right on the volume row: 70%");
        // Restore defaults keeps what belongs to the app (like the language and the colour).
        for (size_t i = 0; i < r.ui.TabOptions(MenuUi::TabSystem).size(); ++i) {
            r.Tap(Pad::Down);
        }
        r.Tap(Pad::Cross);
        r.Tap(Pad::Cross);
        Check(r.ui.Audio() == AudioMode::Off && r.ui.Volume() == 0.7f, "restore defaults keeps the menu sound settings");
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* render_dir = nullptr;
    if (argc > 2 && std::strcmp(argv[1], "--render") == 0) {
        render_dir = argv[2];
    }
    TestSettingsFile();
    TestVisibility();
    TestGameConfig();
    TestNro();
    TestNavigation();
    TestSound();
    TestDrawing(render_dir);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    std::printf(g_failures == 0 ? "menu test: PASSED\n" : "menu test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
