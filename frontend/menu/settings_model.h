// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: what the menu can change, and the settings.txt it changes it in.
//
// settings.txt is the file main.cpp reads at start ("key=value" per line, '#' starts a comment).
// The menu edits only the lines of the options it knows: comments, blank lines and any other
// line (for example a hand-written env=...) are kept as they are. An option set back to its
// default has its line removed, so "default" always means "whatever this version defaults to".

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 0.9.5: what the build is for. 0 = the test builds (the experimental options have their own tab and the LOG tab is
// there); 1 = the release for everyone (neither is shown). Set with -DEDEN_PS4_PUBLIC=1 (frontend.cmake: EDEN_PS4_PUBLIC).
#ifndef EDEN_PS4_PUBLIC
#define EDEN_PS4_PUBLIC 0
#endif

namespace Menu {

inline constexpr bool PublicBuild = EDEN_PS4_PUBLIC != 0;

enum class Lang { Pt, En };
enum class Group { Video, Cpu, Advanced, System, Perf, Controls };

/// 0.9.5: who gets to see an option in the menu.
///   Normal       - always shown, in the tab of its group.
///   Experimental - only in the test builds, in the EXPERIMENTAL tab (a release build hides them; `show_advanced=on`
///                  in settings.txt shows them anyway). Things that change what a game sees or that were never proven.
///   Mandatory    - never shown (unless `show_advanced=on`): the value this port needs is the default, and switching
///                  it makes the port worse (the JIT pack, the colour fixes, the HUD's anti-flicker timing...).
/// A line written by hand in settings.txt for any of them still works, as before.
enum class Tier { Normal, Experimental, Mandatory };
Tier TierOf(std::string_view key);

struct Choice {
    const char* value; // as written in settings.txt
    const char* pt;
    const char* en;
};

struct Option {
    /// settings.txt key. "env:NAME" is the line "env=NAME=value" (a driver environment variable).
    const char* key;
    Group group;
    const char* label_pt;
    const char* label_en;
    const char* help_pt;
    const char* help_en;
    std::vector<Choice> choices;
    int default_index;

    const char* Label(Lang lang) const { return lang == Lang::Pt ? label_pt : label_en; }
    const char* Help(Lang lang) const { return lang == Lang::Pt ? help_pt : help_en; }
    const char* ChoiceLabel(int index, Lang lang) const {
        return lang == Lang::Pt ? choices[size_t(index)].pt : choices[size_t(index)].en;
    }
};

/// Every option, in the order the menu shows them.
const std::vector<Option>& Catalog();
const Option* FindOption(std::string_view key);

/// A quick profile: several options set at once, for trying combinations when a game will not open.
struct Preset {
    const char* pt;
    const char* en;
    const char* help_pt;
    const char* help_en;
    /// The options it sets (the others of the profiles' common set go back to their defaults).
    std::vector<std::pair<const char*, const char*>> values;
    const char* Name(Lang lang) const { return lang == Lang::Pt ? pt : en; }
    const char* Help(Lang lang) const { return lang == Lang::Pt ? help_pt : help_en; }
};

/// The profiles, "Default" first.
const std::vector<Preset>& Presets();

class SettingsFile {
public:
    explicit SettingsFile(const std::string& text = {});

    /// The whole file again, as it should be written.
    std::string Text() const;
    /// The value of a key, or "" when the file has no such line.
    std::string Get(std::string_view key) const;
    void Set(std::string_view key, std::string_view value);
    void Erase(std::string_view key);

    /// Index of the option's current value; its default when the line is absent or not a choice.
    int ChoiceIndex(const Option& option) const;
    /// Writes the choice, or removes the line when it is the default.
    void SetChoice(const Option& option, int index);
    bool IsDefault(const Option& option) const;
    /// Removes the lines of every option of the catalog (not the language).
    void ResetAll();
    Lang Language() const;

    /// Index of the profile the options match exactly, or -1 when they are a custom mix.
    int MatchPreset() const;
    /// Sets every option a profile touches: the ones it names to its values, the rest to default.
    void ApplyPreset(int index);

private:
    struct Line {
        std::string raw;
        std::string key;   // normalised ("env:NAME" for env lines); empty for comments and blanks
        std::string value;
    };
    static Line Parse(const std::string& raw);

    std::vector<Line> lines_;
};

} // namespace Menu
