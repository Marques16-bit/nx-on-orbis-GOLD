// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: the menu. See menu_ui.h.

#include <algorithm>
#include <cstdio>

#include "menu/menu_ui.h"

namespace Menu {

namespace {

constexpr int Margin = 120;
constexpr int ContentTop = 225;
constexpr int RowHeight = 56;
// Kept inside the middle 90% of the screen: a TV's overscan may cut the edges.
constexpr int VisibleRows = 11;
constexpr int HelpTop = 868;
constexpr int FooterTop = 992;
constexpr int FooterCells = (1920 - 2 * Margin) / 20;

constexpr int TabGames = 0, TabVideo = 1, TabCpu = 2, TabAdvanced = 3, TabSystem = 4;

/// File name without its extension.
std::string DisplayName(const std::string& file) {
    std::string name = file;
    if (const auto dot = name.find_last_of('.'); dot != std::string::npos && dot > 0) {
        name.erase(dot);
    }
    return name;
}

std::string FormatSize(std::uint64_t bytes, Lang lang) {
    char text[32];
    const double gib = double(bytes) / double(1ull << 30);
    if (gib >= 1.0) {
        std::snprintf(text, sizeof(text), "%.1f GiB", gib);
    } else {
        std::snprintf(text, sizeof(text), "%.0f MiB", double(bytes) / double(1ull << 20));
    }
    std::string out = text;
    if (lang == Lang::Pt) {
        std::replace(out.begin(), out.end(), '.', ',');
    }
    return out;
}

/// Cuts a UTF-8 string to `cells` characters, with "…" when something was cut.
std::string Clip(const std::string& text, int cells) {
    if (TextCells(text) <= cells) {
        return text;
    }
    std::string out;
    int count = 0;
    for (size_t i = 0; i < text.size() && count < cells - 1;) {
        size_t len = 1;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0xF0) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC0) {
            len = 2;
        }
        out.append(text, i, len);
        i += len;
        ++count;
    }
    return out + "…";
}

/// Word-wraps to `cells` characters per line, at most `max_lines` lines (the last one clipped).
std::vector<std::string> Wrap(const std::string& text, int cells, size_t max_lines) {
    std::vector<std::string> lines;
    std::string current;
    size_t i = 0;
    while (i < text.size()) {
        const auto space = text.find(' ', i);
        const std::string word = text.substr(i, space == std::string::npos ? space : space - i);
        i = space == std::string::npos ? text.size() : space + 1;
        const std::string tried = current.empty() ? word : current + " " + word;
        if (!current.empty() && TextCells(tried) > cells) {
            lines.push_back(current);
            current = word;
        } else {
            current = tried;
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    if (lines.size() > max_lines) {
        lines.resize(max_lines);
        lines.back() = Clip(lines.back() + " …", cells);
    }
    return lines;
}

void DrawRow(Canvas& c, int y, bool selected, bool modified, const std::string& label,
             const std::string& value, std::uint32_t value_color, bool arrows) {
    constexpr int Width = 1920 - 2 * Margin;
    if (selected) {
        c.RoundRect(Margin, y - 4, Width, RowHeight - 4, 10, Color::Selected);
        c.Fill(Margin, y + 6, 6, RowHeight - 24, Color::Teal);
    }
    if (modified) {
        c.Print(Margin + 16, y, "•", Color::Teal);
    }
    c.Print(Margin + 46, y, label, selected ? 0xFFFFFFFF : Color::Text);
    std::string shown = value;
    if (arrows) {
        shown = "◀ " + value + " ▶";
    }
    c.Print(1920 - Margin - 30 - TextWidth(shown), y, shown, value_color);
}

} // Anonymous namespace

MenuUi::MenuUi(std::vector<GameEntry> games, int initial_game, SystemInfo info,
               const std::string& settings_text, SaveFn save)
    : games_(std::move(games)), info_(std::move(info)), file_(settings_text), save_(std::move(save)) {
    for (const Option& option : Catalog()) {
        switch (option.group) {
        case Group::Video:
            options_[TabVideo].push_back(&option);
            break;
        case Group::Cpu:
            options_[TabCpu].push_back(&option);
            break;
        case Group::Advanced:
            options_[TabAdvanced].push_back(&option);
            break;
        case Group::System:
            options_[TabSystem].push_back(&option); // the language; row 1 is "restore defaults"
            break;
        }
    }
    const int count = int(games_.size());
    last_game_ = count > 0 ? std::clamp(initial_game, 0, count - 1) : 0;
    selected_[TabGames] = count > 0 ? last_game_ : 0;
}

void MenuUi::SetTab(int tab) {
    tab_ = (tab % TabCount + TabCount) % TabCount;
    confirm_reset_ = false;
    dirty_ = true;
}

int MenuUi::RowCount(int tab) const {
    switch (tab) {
    case TabGames:
        return int(games_.size()) + 1; // the games, then the Homebrew Menu
    case TabSystem:
        return 2;
    default:
        return int(options_[tab].size());
    }
}

const Option* MenuUi::OptionAt(int tab, int row) const {
    if (tab == TabGames || row < 0 || row >= int(options_[tab].size())) {
        return nullptr;
    }
    return options_[tab][size_t(row)];
}

void MenuUi::Save() {
    if (save_) {
        save_(file_.Text());
    }
}

void MenuUi::MoveRow(int delta) {
    const int count = RowCount(tab_);
    if (count <= 0) {
        return;
    }
    int& selected = selected_[tab_];
    selected = (selected + delta + count) % count;
    int& scroll = scroll_[tab_];
    scroll = std::clamp(scroll, std::max(selected - VisibleRows + 1, 0), selected);
    if (tab_ == TabGames && selected < int(games_.size())) {
        last_game_ = selected;
    }
    confirm_reset_ = false;
    dirty_ = true;
}

void MenuUi::ChangeValue(int delta) {
    if (tab_ == TabGames) {
        // Left/Right page through a long list.
        const int count = RowCount(tab_);
        int& selected = selected_[tab_];
        selected = std::clamp(selected + delta * VisibleRows, 0, count - 1);
        scroll_[tab_] = std::clamp(scroll_[tab_], std::max(selected - VisibleRows + 1, 0), selected);
        if (selected < int(games_.size())) {
            last_game_ = selected;
        }
        dirty_ = true;
        return;
    }
    if (const Option* option = OptionAt(tab_, selected_[tab_])) {
        const int n = int(option->choices.size());
        file_.SetChoice(*option, (file_.ChoiceIndex(*option) + delta + n) % n);
        Save();
        confirm_reset_ = false;
        dirty_ = true;
    }
}

void MenuUi::Activate() {
    if (tab_ == TabGames) {
        const int row = selected_[TabGames];
        result_.kind = row < int(games_.size()) ? MenuResult::Kind::Play : MenuResult::Kind::Homebrew;
        result_.game = row < int(games_.size()) ? row : -1;
        return;
    }
    if (tab_ == TabSystem && selected_[TabSystem] == 1) {
        if (!confirm_reset_) {
            confirm_reset_ = true;
        } else {
            file_.ResetAll();
            Save();
            confirm_reset_ = false;
        }
        dirty_ = true;
        return;
    }
    ChangeValue(+1);
}

void MenuUi::Press(std::uint32_t button) {
    switch (button) {
    case Pad::Up:
        MoveRow(-1);
        break;
    case Pad::Down:
        MoveRow(+1);
        break;
    case Pad::Left:
        ChangeValue(-1);
        break;
    case Pad::Right:
        ChangeValue(+1);
        break;
    default:
        break;
    }
}

bool MenuUi::Update(std::uint32_t held, std::uint64_t now_us) {
    const std::uint32_t pressed = held & ~previous_;
    previous_ = held;

    if (pressed & Pad::L1) {
        SetTab(tab_ - 1);
    }
    if (pressed & Pad::R1) {
        SetTab(tab_ + 1);
    }
    if (pressed & Pad::Cross) {
        Activate();
    }
    if (pressed & Pad::Circle) {
        if (tab_ != TabGames) {
            SetTab(TabGames);
        }
    }
    if (pressed & Pad::Triangle) {
        if (const Option* option = OptionAt(tab_, selected_[tab_])) {
            file_.SetChoice(*option, option->default_index);
            Save();
            dirty_ = true;
        }
    }
    if ((pressed & Pad::Options) && !games_.empty()) {
        result_.kind = MenuResult::Kind::Play; // the game last highlighted, from any tab
        result_.game = last_game_;
    }

    // Directions: once when pressed, then repeating while held (0.4 s, then every 0.11 s).
    constexpr std::uint32_t Directions = Pad::Up | Pad::Down | Pad::Left | Pad::Right;
    const std::uint32_t down = held & Directions;
    std::uint32_t fire = pressed & Directions;
    if (fire != 0) {
        repeat_at_ = now_us + 400000;
    } else if (down != 0 && now_us >= repeat_at_) {
        fire = down;
        repeat_at_ = now_us + 110000;
    }
    for (std::uint32_t button : {Pad::Up, Pad::Down, Pad::Left, Pad::Right}) {
        if (fire & button) {
            Press(button);
            break; // one direction per step
        }
    }

    const bool redraw = dirty_ || !started_;
    dirty_ = false;
    started_ = true;
    return redraw;
}

void MenuUi::DrawHeader(Canvas& c) const {
    c.FillGradient(0, 0, c.width, c.height, Color::BackgroundTop, Color::BackgroundBottom);
    // The mark: a planet with its orbit and one satellite.
    const float cx = Margin + 52.0f, cy = 70.0f;
    c.Ring(cx, cy, 44.0f, 37.0f, Color::Teal);
    c.Disc(cx, cy, 21.0f, Color::Accent);
    c.Disc(cx + 31.0f, cy - 31.0f, 8.5f, Color::Text);
    c.Print(Margin + 130, 28, "NX on Orbis", Color::Text, 2);
    const std::string version = info_.build_id;
    c.Print(1920 - Margin - TextWidth(version), 52, version, Color::Dim);
    c.Fill(Margin, 130, 1920 - 2 * Margin, 3, Color::Panel);
}

void MenuUi::DrawTabs(Canvas& c) const {
    const char* names_pt[TabCount] = {"JOGOS", "VÍDEO", "CPU E MEMÓRIA", "AVANÇADO", "SISTEMA"};
    const char* names_en[TabCount] = {"GAMES", "VIDEO", "CPU & MEMORY", "ADVANCED", "SYSTEM"};
    int x = Margin;
    for (int i = 0; i < TabCount; ++i) {
        const std::string name = lang() == Lang::Pt ? names_pt[i] : names_en[i];
        const int w = TextWidth(name) + 56;
        if (i == tab_) {
            c.RoundRect(x, 148, w, 56, 12, Color::Accent);
            c.Print(x + 28, 156, name, 0xFFFFFFFF);
        } else {
            c.RoundRect(x, 148, w, 56, 12, Color::Panel);
            c.Print(x + 28, 156, name, Color::Dim);
        }
        x += w + 14;
    }
}

void MenuUi::DrawGames(Canvas& c) const {
    const Lang l = lang();
    int top = ContentTop;
    if (games_.empty()) {
        c.RoundRect(Margin, top, 1920 - 2 * Margin, 3 * RowHeight, 12, Color::Panel);
        c.Print(Margin + 30, top + 10, T("Nenhum jogo encontrado.", "No games found."), Color::Warn);
        c.Print(Margin + 30, top + 10 + 52,
                T("Copie arquivos .nsp ou .xci para ", "Copy .nsp or .xci files to ") + std::string(info_.data_dir + "/roms"),
                Color::Text);
        c.Print(Margin + 30, top + 10 + 100,
                T("e abra o app de novo. Veja a aba SISTEMA para conferir chaves e firmware.",
                  "and open the app again. See the SYSTEM tab to check keys and firmware."),
                Color::Dim);
        top += 3 * RowHeight + 14;
    }
    const int count = RowCount(TabGames);
    const int rows = games_.empty() ? VisibleRows - 3 : VisibleRows;
    const int first = std::clamp(scroll_[TabGames], 0, std::max(count - rows, 0));
    for (int r = 0; r < rows && first + r < count; ++r) {
        const int index = first + r;
        const bool is_game = index < int(games_.size());
        const std::string size = is_game ? FormatSize(games_[size_t(index)].bytes, l) : std::string();
        const int name_cells = (1920 - 2 * Margin - 46 - 30 - 40) / GlyphWidth - TextCells(size) - 1;
        const std::string name = is_game ? Clip(DisplayName(games_[size_t(index)].name), name_cells)
                                         : std::string(T("Homebrew Menu (teste interno)", "Homebrew Menu (built-in test)"));
        const bool selected = index == selected_[TabGames];
        const int y = top + r * RowHeight;
        if (selected) {
            c.RoundRect(Margin, y - 4, 1920 - 2 * Margin, RowHeight - 4, 10, Color::Selected);
            c.Fill(Margin, y + 6, 6, RowHeight - 24, Color::Teal);
        }
        c.Print(Margin + 46, y, name, selected ? 0xFFFFFFFF : (is_game ? Color::Text : Color::Dim));
        if (is_game) {
            c.Print(1920 - Margin - 30 - TextWidth(size), y, size, Color::Dim);
        }
    }
    if (first > 0) {
        c.Print(1920 - Margin - GlyphWidth, top - 36, "▲", Color::Dim);
    }
    if (first + rows < count) {
        c.Print(1920 - Margin - GlyphWidth, top + rows * RowHeight, "▼", Color::Dim);
    }
}

void MenuUi::DrawOptions(Canvas& c) const {
    const Lang l = lang();
    const auto& options = options_[tab_];
    const int count = int(options.size());
    const int first = std::clamp(scroll_[tab_], 0, std::max(count - VisibleRows, 0));
    for (int r = 0; r < VisibleRows && first + r < count; ++r) {
        const Option& option = *options[size_t(first + r)];
        const bool selected = first + r == selected_[tab_];
        const bool changed = !file_.IsDefault(option);
        DrawRow(c, ContentTop + r * RowHeight, selected, changed, option.Label(l),
                option.ChoiceLabel(file_.ChoiceIndex(option), l),
                changed ? Color::Teal : Color::Text, selected);
    }
    if (first > 0) {
        c.Print(1920 - Margin - GlyphWidth, ContentTop - 36, "▲", Color::Dim);
    }
    if (first + VisibleRows < count) {
        c.Print(1920 - Margin - GlyphWidth, ContentTop + VisibleRows * RowHeight, "▼", Color::Dim);
    }
}

void MenuUi::DrawSystem(Canvas& c) const {
    const Lang l = lang();
    const Option& language = *options_[TabSystem][0];
    DrawRow(c, ContentTop, selected_[TabSystem] == 0, !file_.IsDefault(language), language.Label(l),
            language.ChoiceLabel(file_.ChoiceIndex(language), l),
            file_.IsDefault(language) ? Color::Text : Color::Teal, selected_[TabSystem] == 0);
    const std::string reset = confirm_reset_ ? T("Aperte X de novo para confirmar", "Press X again to confirm")
                                             : T("Restaurar todos os padrões", "Restore all defaults");
    {
        const bool selected = selected_[TabSystem] == 1;
        const int y = ContentTop + RowHeight;
        if (selected) {
            c.RoundRect(Margin, y - 4, 1920 - 2 * Margin, RowHeight - 4, 10, Color::Selected);
            c.Fill(Margin, y + 6, 6, RowHeight - 24, Color::Teal);
        }
        c.Print(Margin + 46, y, reset, confirm_reset_ ? Color::Warn : (selected ? 0xFFFFFFFF : Color::Text));
    }

    // Read-only facts.
    const int label_x = Margin + 46, value_x = Margin + 46 + 24 * GlyphWidth;
    const int max_cells = (1920 - Margin - value_x) / GlyphWidth;
    int y = ContentTop + 2 * RowHeight + 40;
    c.Fill(Margin, y - 22, 1920 - 2 * Margin, 2, Color::Panel);
    const auto row = [&](const char* label, const std::string& value, std::uint32_t color) {
        c.Print(label_x, y, label, Color::Dim);
        c.Print(value_x, y, Clip(value, max_cells), color);
        y += 50;
    };
    row(T("Versão", "Version"), info_.build_id, Color::Text);
    row(T("Chaves (prod.keys)", "Keys (prod.keys)"),
        info_.keys_found ? std::string("✓ ") + T("encontradas", "found")
                         : std::string("✗ ") + T("não encontradas: copie para ", "not found: copy to ") + info_.data_dir + "/keys",
        info_.keys_found ? Color::Good : Color::Bad);
    char firmware[64];
    std::snprintf(firmware, sizeof(firmware), "✓ %d %s", info_.firmware_files, T("arquivos .nca", ".nca files"));
    row("Firmware",
        info_.firmware_files > 0 ? std::string(firmware)
                                 : std::string("✗ ") + T("nenhum arquivo: copie para ", "no files: copy to ") + info_.data_dir + "/firmware",
        info_.firmware_files > 0 ? Color::Good : Color::Warn);
    row(T("Jogos", "Games"), std::to_string(games_.size()) + T(" em ", " in ") + info_.data_dir + "/roms", Color::Text);
    row(T("Memória direta livre", "Free direct memory"), std::to_string(info_.free_memory_mib) + " MiB", Color::Text);
    row(T("Pasta de dados", "Data folder"), info_.data_dir, Color::Text);
    row("Logs", "boot.log, mesa.log, log/eden_log.txt", Color::Text);
}

void MenuUi::DrawHelp(Canvas& c, const std::vector<std::string>& lines) const {
    c.RoundRect(Margin, HelpTop, 1920 - 2 * Margin, 110, 12, Color::Panel);
    for (size_t i = 0; i < lines.size() && i < 2; ++i) {
        c.Print(Margin + 26, HelpTop + 12 + int(i) * 48, lines[i], Color::Dim);
    }
}

void MenuUi::DrawFooter(Canvas& c) const {
    const char* text = T("▲▼ escolher   ◀▶ alterar   X próximo valor   Triângulo: padrão   O: voltar   L1/R1: abas",
                         "▲▼ choose   ◀▶ change   X next value   Triangle: default   O: back   L1/R1: tabs");
    if (tab_ == TabGames) {
        text = T("▲▼ escolher   ◀▶ pular página   X jogar   L1/R1: abas   OPTIONS: jogar o último",
                 "▲▼ choose   ◀▶ page   X play   L1/R1: tabs   OPTIONS: play the last one");
    }
    c.Print(Margin, FooterTop, Clip(text, FooterCells), Color::Dim);
}

void MenuUi::Draw(Canvas& c) const {
    DrawHeader(c);
    DrawTabs(c);
    std::vector<std::string> help;
    constexpr int HelpCells = (1920 - 2 * Margin - 52) / GlyphWidth;
    if (tab_ == TabGames) {
        DrawGames(c);
        help = Wrap(T("Escolha um jogo e aperte X. As opções das outras abas valem na hora de iniciar.",
                      "Pick a game and press X. The options in the other tabs apply when it starts."),
                    HelpCells, 2);
    } else if (tab_ == TabSystem) {
        DrawSystem(c);
        if (const Option* option = OptionAt(TabSystem, selected_[TabSystem])) {
            help = Wrap(option->Help(lang()), HelpCells, 2);
        } else {
            help = Wrap(T("Volta todas as opções para o padrão desta versão. Os jogos e o idioma não mudam.",
                          "Sets every option back to this version's default. Games and language are not touched."),
                        HelpCells, 2);
        }
    } else {
        DrawOptions(c);
        if (const Option* option = OptionAt(tab_, selected_[tab_])) {
            help = Wrap(option->Help(lang()), HelpCells, 2);
        }
    }
    DrawHelp(c, help);
    DrawFooter(c);
}

} // namespace Menu
