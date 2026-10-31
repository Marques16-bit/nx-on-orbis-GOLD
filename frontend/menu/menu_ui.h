// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: the menu shown before the emulator starts: the game list and the settings, in tabs.
//
// This part knows nothing about the console: it takes the pad's buttons and draws on a Canvas, so
// it is tested on a PC (tests/menu_test.cpp), including screenshots of every tab. The console side
// (video-out, the pad) is rom_menu.cpp.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
inline constexpr std::uint32_t L1 = 0x0400;
inline constexpr std::uint32_t R1 = 0x0800;
inline constexpr std::uint32_t Triangle = 0x1000;
inline constexpr std::uint32_t Circle = 0x2000;
inline constexpr std::uint32_t Cross = 0x4000;
} // namespace Pad

struct GameEntry {
    std::string name; // file name
    std::uint64_t bytes = 0;
};

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

    static constexpr int TabCount = 7;

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

    void DrawHeader(Canvas& c) const;
    void DrawTabs(Canvas& c) const;
    void DrawGames(Canvas& c) const;
    void DrawOptions(Canvas& c) const;
    void DrawSystem(Canvas& c) const;
    void DrawLog(Canvas& c) const;
    bool IsPresetRow(int tab, int row) const { return tab == 1 && row == 0; }
    void DrawHelp(Canvas& c, const std::vector<std::string>& lines) const;
    void DrawFooter(Canvas& c) const;

    std::vector<GameEntry> games_;
    SystemInfo info_;
    SettingsFile file_;
    SaveFn save_;
    std::vector<const Option*> options_[TabCount];

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
