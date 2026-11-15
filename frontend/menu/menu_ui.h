// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: the menu shown before the emulator starts: the game list and the settings, in tabs.
//
// This part knows nothing about the console: it takes the pad's buttons and draws on a Canvas, so
// it is tested on a PC (tests/menu_test.cpp), including screenshots of every tab. The console side
// (video-out, the pad) is rom_menu.cpp.

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "menu/image_decode.h"
#include "menu/menu_canvas.h"
#include "menu/settings_model.h"

namespace Menu {

/// DualShock 4 button bits (the same values as Ps4::Button; rom_menu.cpp checks they match).
namespace Pad {
inline constexpr std::uint32_t Options = 0x0008;
inline constexpr std::uint32_t Up = 0x0010;
inline constexpr std::uint32_t Right = 0x0020;
inline constexpr std::uint32_t Down = 0x0040;
inline constexpr std::uint32_t Left = 0x0080;
inline constexpr std::uint32_t L2 = 0x0100;
inline constexpr std::uint32_t R2 = 0x0200;
inline constexpr std::uint32_t L1 = 0x0400;
inline constexpr std::uint32_t R1 = 0x0800;
inline constexpr std::uint32_t Triangle = 0x1000;
inline constexpr std::uint32_t Circle = 0x2000;
inline constexpr std::uint32_t Cross = 0x4000;
} // namespace Pad

struct GameEntry {
    std::string name; // file name
    std::uint64_t bytes = 0;
    // 0.7.9: the cover, read from covers/ by the console side. Both empty when there is none (the menu then
    // draws a generated tile). `cover` fits in CoverSize x CoverSize, `thumb` in ThumbSize x ThumbSize.
    Image::Pixels cover;
    Image::Pixels thumb;
    // 0.9.5: the title the game calls itself (the NACP of a .nro); empty = the title is taken from the file name.
    std::string title;
    // 0.9.5: the game has its own settings file (per-game/<title id>.txt); the list marks it.
    bool has_config = false;

    GameEntry() = default;
    GameEntry(std::string file, std::uint64_t size) : name(std::move(file)), bytes(size) {}
};
inline constexpr int CoverSize = 400;
inline constexpr int ThumbSize = 44;

/// What the SYSTEM tab shows. Filled by the console side.
struct SystemInfo {
    std::string build_id;
    std::string data_dir;
    bool keys_found = false;
    int firmware_files = 0;     // .nca files in firmware/
    int firmware_zips = 0;      // .zip files in firmware/ (installed when a game starts)
    int firmware_installed = 0; // .nca files already in the registered folder
    int shader_cache_games = 0; // games with a shader cache (cache/shader/<title id>/)
    std::uint64_t shader_cache_mib = 0;
    std::function<bool()> clear_shader_cache; // empties cache/shader; true when it worked
    bool data_writable = true;       // a test file could be written in data_dir
    std::int64_t log_bytes = -1;     // size of boot.log, or -1 when there is none
    std::int64_t settings_bytes = -1; // size of settings.txt, or -1 when there is none
    /// The last lines of the previous session's log (boot.old.log) and of this session's so far,
    /// for the LOG tab: they can be read on the TV when the files cannot be reached over FTP.
    std::vector<std::string> log_previous;
    std::vector<std::string> log_current;
    std::uint64_t free_memory_mib = 0;
    /// 0.7.6: all the direct memory the console gives the process (0 when unknown), its class in words
    /// ("folgado", "roomy"...) and what the memory profile sets, e.g. "auto: arena 1280 MiB, ...".
    std::uint64_t memory_budget_mib = 0;
    std::string memory_tier;
    std::string memory_plan;
    /// 0.9.5: which controllers are connected right now (player 1 always is: it is the one holding the menu). The console
    /// side refreshes this while the menu is up (MenuUi::SetPadsConnected); the CONTROLS tab shows it.
    bool pad_connected[4] = {true, false, false, false};
    /// 0.9.5: a game's own settings (R2 on the game list). `load` gives the text of the game's file ("" when it has none); `save` writes it
    /// (an empty text, or one with no setting in it, removes the file) and returns true when it is there. Both take the game's file name.
    std::function<std::string(const std::string&)> load_game_config;
    std::function<bool(const std::string&, const std::string&)> save_game_config;
};

struct MenuResult {
    enum class Kind { None, Play, Homebrew };
    Kind kind = Kind::None;
    int game = -1; // index into the game list, for Play
};

class MenuUi {
public:
    /// Called with the whole new text of settings.txt each time an option changes; true when it
    /// was saved (and read back).
    using SaveFn = std::function<bool(const std::string&)>;

    MenuUi(std::vector<GameEntry> games, int initial_game, SystemInfo info,
           const std::string& settings_text, SaveFn save);

    /// One step: `held` is the buttons held now, `now_us` a monotonic clock. Returns true when
    /// the screen changed and has to be drawn again.
    bool Update(std::uint32_t held, std::uint64_t now_us);
    void Draw(Canvas& canvas) const;

    const MenuResult& Result() const { return result_; }
    int Tab() const { return tab_; }
    void SetTab(int tab);
    /// 0.9.5: the tabs that are shown, in the order L1/R1 walk them (the LOG tab is not there in a public build and the
    /// EXPERIMENTAL tab only when it has options).
    const std::vector<int>& Tabs() const { return tabs_; }
    /// 0.9.5: the options a tab lists (for the tests).
    const std::vector<const Option*>& TabOptions(int tab) const { return options_[tab]; }
    /// 0.9.5: the controllers that are connected now (see SystemInfo::pad_connected). True when something changed.
    bool SetPadsConnected(const bool (&connected)[4]);
    /// The number of players in settings.txt (1 to 4) and the model chosen for one of them ("auto", "pro", ...).
    int Players() const;
    std::string PlayerType(int player) const;
    /// 0.9.5: the game's own settings screen (R2 on a game). For the tests: whether it is open, which game, and the options it lists.
    bool GameConfigOpen() const { return game_cfg_open_; }
    int GameConfigGame() const { return game_cfg_game_; }
    const std::vector<const Option*>& GameConfigOptions() const { return game_opts_; }

    // 0.9.5: the tab numbers are the order they are shown in.
    static constexpr int TabGames = 0, TabControls = 1, TabVideo = 2, TabCpu = 3, TabPerf = 4, TabSystem = 5, TabLog = 6,
                         TabAdvanced = 7;
    static constexpr int TabCount = 8;

private:
    Lang lang() const { return file_.Language(); }
    const char* T(const char* pt, const char* en) const { return lang() == Lang::Pt ? pt : en; }
    int RowCount(int tab) const;
    const Option* OptionAt(int tab, int row) const;
    void MoveRow(int delta);
    void ChangeValue(int delta);
    void Activate();
    void Save();
    void Press(std::uint32_t button);

    // 0.9.5: the settings of one game
    void OpenGameConfig();
    void CloseGameConfig();
    void PressGameConfig(std::uint32_t button);
    void ChangeGameOption(int delta);
    void ActivateGameConfig();
    void ClearGameOption();
    void SaveGameConfig();
    int GameConfigRows() const { return int(game_opts_.size()) + 1; } // the options, then "clear this game's settings"
    void DrawGameConfig(Canvas& c) const;
    std::string TitleOf(const GameEntry& game) const;

    void DrawBackground(Canvas& c) const;
    void DrawHeader(Canvas& c) const;
    void DrawTabs(Canvas& c) const;
    void DrawGames(Canvas& c) const;
    void DrawGameCard(Canvas& c, int index) const;
    void DrawControls(Canvas& c) const;
    void PressControls(std::uint32_t button);
    void ToggleControlsPlayer();
    void SetPlayers(int count);
    void ChangePlayerType(int player, int delta);
    /// Index of the colour theme in settings.txt (menu_theme), 0 when absent.
    int ThemeIndex() const;
    void DrawOptions(Canvas& c) const;
    void DrawSystem(Canvas& c) const;
    void DrawLog(Canvas& c) const;
    bool IsPresetRow(int tab, int row) const { return tab == TabVideo && row == 0; }
    void DrawHelp(Canvas& c, const std::vector<std::string>& lines) const;
    void DrawFooter(Canvas& c) const;

    std::vector<GameEntry> games_;
    SystemInfo info_;
    SettingsFile file_;
    SaveFn save_;
    std::vector<const Option*> options_[TabCount];
    std::vector<int> tabs_;
    // 0.9.5: the game's own settings screen
    std::vector<const Option*> game_opts_; // what a game's file may set (the options the menu shows, minus the ones that are global)
    bool game_cfg_open_ = false;
    int game_cfg_game_ = -1;
    SettingsFile game_file_;
    int game_sel_ = 0;
    int game_scroll_ = 0;
    bool game_confirm_clear_ = false;
    /// 0.9.5: the background (gradient, soft lights, thin orbits) is drawn once per theme and copied each time.
    mutable std::vector<std::uint32_t> background_;
    mutable int background_theme_ = -1;

    int tab_ = 0;
    int selected_[TabCount] = {};
    int scroll_[TabCount] = {};
    int last_game_ = 0;
    bool confirm_reset_ = false;
    bool confirm_clear_ = false;
    bool clear_ok_ = false;
    int log_source_ = 0; // LOG tab: 0 the previous session, 1 this one
    int save_state_ = 0; // 0 nothing saved yet, 1 the last save worked, 2 it failed
    bool started_ = false;

    std::uint32_t previous_ = 0;
    std::uint64_t repeat_at_ = 0;
    bool dirty_ = true;
    MenuResult result_;
};

} // namespace Menu
