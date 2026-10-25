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
#include <vector>

namespace Menu {

enum class Lang { Pt, En };
enum class Group { Video, Cpu, Advanced, System };

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
