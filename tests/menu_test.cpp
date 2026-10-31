// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the menu (frontend/menu/): settings.txt reading and writing, the navigation, and
// screenshots of every tab. Runs on a PC / in CI, not on the console.
//
//   g++ -O2 -std=c++20 -Wall -Wextra -Ifrontend tests/menu_test.cpp frontend/menu/menu_ui.cpp frontend/menu/settings_model.cpp frontend/menu/menu_canvas.cpp -o menu_test
//   ./menu_test                 # exit code 0 = all good
//   ./menu_test --render DIR    # also writes DIR/*.ppm, one screenshot per tab and language

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "menu/menu_ui.h"

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
    MenuUi ui;

    explicit Rig(const std::string& settings, int games = 3, int initial = 0)
        : ui(MakeGames(games), initial, MakeInfo(games), settings,
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
        check("memory_action", {"warn", "pause"}, "warn");
        check("env:ORBIS_ARENA_MIB", {"1024", "1152", "1280", "1536", "1792"}, "1280");
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
        Check(r.ui.Tab() == 1, "R1: next tab");
        r.Tap(Pad::R1); r.Tap(Pad::R1); r.Tap(Pad::R1); r.Tap(Pad::R1); r.Tap(Pad::R1); r.Tap(Pad::R1);
        Check(r.ui.Tab() == 0, "R1 wraps around (7 tabs)");
        r.Tap(Pad::L1);
        Check(r.ui.Tab() == 6, "L1 wraps backwards (to the PERFORMANCE tab)");
        r.Tap(Pad::Circle);
        Check(r.ui.Tab() == 0, "Circle goes back to the games");
    }
    {
        Rig r("");
        r.Tap(Pad::R1); // VIDEO: row 0 is the quick profile, row 1 the console mode
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
            ui.SetTab(4);
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
        r.ui.SetTab(4); // SYSTEM
        r.Tap(Pad::Cross); // language
        Check(r.saved.find("language=en") != std::string::npos, "Cross on the language switches to English");
        Check(r.ui.Result().kind == MenuResult::Kind::None, "...and does not start a game");
        r.Tap(Pad::Down);
        r.saves = 0;
        r.Tap(Pad::Cross);
        Check(r.saves == 0, "restore defaults asks for confirmation first");
        r.Tap(Pad::Cross);
        Check(r.saves == 1 && r.saved.find("language=en") != std::string::npos, "second press resets the options, not the language");
    }
    {
        Rig r("mode=docked\nastc=gpu\n");
        r.ui.SetTab(4);
        r.Tap(Pad::Down);
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
        r.Tap(Pad::R1);
        Check(r.ui.Tab() == 1, "VIDEO tab");
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
        Check(SettingsFile(r.saved).MatchPreset() == 3 && r.saved.find("ORBIS_ARENA_MIB=1024") != std::string::npos &&
                  r.saved.find("driver_cache=off") != std::string::npos && r.saved.find("memory_profile=tight") != std::string::npos &&
                  r.saved.find("memory_action=pause") != std::string::npos,
              "profile Save RAM: 1024 MiB arena, no driver cache, tight memory profile and pause written");
        r.Tap(Pad::Right);
        Check(SettingsFile(r.saved).MatchPreset() == 4 && r.saved.find("ORBIS_ARENA_MIB=1792") != std::string::npos &&
                  r.saved.find("driver_cache") == std::string::npos && r.saved.find("memory_profile") == std::string::npos, "profile More video memory replaces it (1792 MiB, driver cache back to default)");
        r.Tap(Pad::Right);
        Check(r.saved.find("gpu_accuracy") == std::string::npos && SettingsFile(r.saved).MatchPreset() == 0, "wraps to Default: nothing written");
        r.Tap(Pad::Left);
        Check(SettingsFile(r.saved).MatchPreset() == 4, "Left from Default wraps to the last profile");
        r.Tap(Pad::Triangle);
        Check(SettingsFile(r.saved).MatchPreset() == 0, "Triangle on the profile row: Default");
    }
    {
        // Settings that match no profile are a custom mix; a profile does not touch the others.
        Rig r("resolution=2\nlanguage=en\nmode=docked\n");
        r.Tap(Pad::R1);
        Check(SettingsFile("resolution=2\n").MatchPreset() == -1, "resolution=2 is a custom mix");
        r.Tap(Pad::Right);
        Check(SettingsFile(r.saved).MatchPreset() == 0, "Right from a custom mix: the first profile");
        Check(r.saved.find("mode=docked") != std::string::npos && r.saved.find("language=en") != std::string::npos,
              "a profile leaves the console mode and the language alone");
        Rig q("resolution=2\n");
        q.Tap(Pad::R1);
        q.Tap(Pad::Left);
        Check(SettingsFile(q.saved).MatchPreset() == 4, "Left from a custom mix: the last profile");
    }
    {
        // A save that fails is shown, and a save that works is too.
        Rig r("");
        r.save_works = false;
        r.ui.SetTab(2);
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
        // Clearing the shader cache asks twice, from the SYSTEM tab's third row.
        int cleared = 0;
        SystemInfo info = Rig::MakeInfo(1);
        info.shader_cache_games = 2;
        info.clear_shader_cache = [&] {
            ++cleared;
            return true;
        };
        MenuUi ui(Rig::MakeGames(1), 0, info, "", nullptr);
        ui.SetTab(4);
        std::uint64_t now = 1000000;
        auto tap = [&](std::uint32_t b) {
            ui.Update(b, now);
            now += 50000;
            ui.Update(0, now);
            now += 50000;
        };
        tap(Pad::Down);
        tap(Pad::Down);
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
        Check(frame("Super Mario Maker 2 [01009B90006DC000][v0] (US).nsp") == frame("Super Mario Maker 2.xci"),
              "the title in the list has no [tags], (tags) or extension");
        Check(frame("Super_Mario_Maker_2.nsp") == frame("Super Mario Maker 2.nsp"), "underscores read as spaces");
        Check(frame("Celeste.nsp") != frame("Super Mario Maker 2.nsp"), "different titles draw differently");
    }
    {
        // The LOG tab: Left/Right switch the session, and it draws for both, empty or not.
        Rig r("");
        r.ui.SetTab(5);
        Check(r.ui.Tab() == 5, "LOG tab");
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

struct Frame {
    std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(1920 * 1080);
    Canvas Canvas_() { return Canvas{pixels.data(), 1920, 1080}; }
};

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

} // namespace

int main(int argc, char** argv) {
    const char* render_dir = nullptr;
    if (argc > 2 && std::strcmp(argv[1], "--render") == 0) {
        render_dir = argv[2];
    }
    TestSettingsFile();
    TestNavigation();
    TestDrawing(render_dir);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    std::printf(g_failures == 0 ? "menu test: PASSED\n" : "menu test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
