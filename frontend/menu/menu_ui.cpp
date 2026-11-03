// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: the menu. See menu_ui.h.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "menu/menu_ui.h"

#include "menu/cover_loader.h"

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

constexpr int TabGames = 0, TabVideo = 1, TabCpu = 2, TabAdvanced = 3, TabSystem = 4, TabLog = 5, TabPerf = 6;

using Covers::DisplayName;
using Covers::TitleIdOf;

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

/// The first letters of the first two words of a title, upper case ("Super Mario Maker 2" -> "SM").
std::string Initials(const std::string& title, int count) {
    std::string out;
    bool word_start = true;
    int words = 0;
    for (size_t i = 0; i < title.size() && words < count;) {
        size_t len = 1;
        const unsigned char ch = static_cast<unsigned char>(title[i]);
        if (ch >= 0xF0) len = 4; else if (ch >= 0xE0) len = 3; else if (ch >= 0xC0) len = 2;
        if (ch == ' ' || ch == '-' || ch == ':') {
            word_start = true;
        } else if (word_start) {
            out += ch < 0x80 ? std::string(1, char(std::toupper(ch))) : title.substr(i, len);
            ++words;
            word_start = false;
        }
        i += len;
    }
    return out;
}

/// Hue (0..1) from the title: the same game always gets the same colours.
float HueOf(const std::string& key) {
    std::uint32_t h = 2166136261u;
    for (const char ch : key) {
        h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    }
    return float(h % 3600u) / 3600.0f;
}

std::uint32_t FromHsv(float h, float s, float v) {
    const float f = h * 6.0f - std::floor(h * 6.0f);
    const float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    float r = v, g = t, b = p;
    switch (int(std::floor(h * 6.0f)) % 6) {
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
    default: break;
    }
    return 0xFF000000u | (std::uint32_t(r * 255.0f + 0.5f) << 16) | (std::uint32_t(g * 255.0f + 0.5f) << 8) | std::uint32_t(b * 255.0f + 0.5f);
}

/// A game's picture in a size x size box at (x, y): its cover, or a tile (a gradient with the initials)
/// when it has none. `image` is the cover or the thumbnail, already fitted to the box.
void DrawCover(Canvas& c, int x, int y, int size, const Image::Pixels& image, const std::string& title, const std::string& key) {
    const int radius = std::max(6, size / 14);
    if (image.Ok()) {
        c.RoundRect(x, y, size, size, radius, 0xFF0E1530);
        const int ix = x + (size - image.w) / 2, iy = y + (size - image.h) / 2;
        const bool fills = image.w >= size - 1 && image.h >= size - 1;
        c.DrawImage(ix, iy, image.px.data(), image.w, image.h, fills ? radius : std::max(2, radius / 3));
        return;
    }
    const float hue = HueOf(key);
    c.RoundRectGradient(x, y, size, size, radius, FromHsv(hue, 0.55f, 0.82f), FromHsv(std::fmod(hue + 0.07f, 1.0f), 0.70f, 0.38f));
    const std::string letters = Initials(title, size >= 200 ? 2 : 1);
    const int scale = size >= 300 ? 4 : (size >= 120 ? 2 : 1);
    const int w = TextWidth(letters, scale), h = GlyphHeight * scale;
    if (w <= size && h <= size) {
        c.Print(x + (size - w) / 2, y + (size - h) / 2 - (size >= 120 ? 4 : 0), letters, 0xFFFFFFFF, scale);
    }
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
        case Group::Perf:
            options_[TabPerf].push_back(&option);
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
    confirm_clear_ = false;
    dirty_ = true;
}

int MenuUi::RowCount(int tab) const {
    switch (tab) {
    case TabGames:
        return int(games_.size()) + 1; // the games, then the Homebrew Menu
    case TabSystem:
        return int(options_[TabSystem].size()) + 2; // language, colour, then restore defaults and clear the shader cache
    case TabVideo:
        return int(options_[tab].size()) + 1; // the quick profile, then the options
    default:
        return int(options_[tab].size());
    }
}

const Option* MenuUi::OptionAt(int tab, int row) const {
    if (tab == TabVideo) {
        --row; // row 0 is the quick profile
    }
    if (tab == TabGames || row < 0 || row >= int(options_[tab].size())) {
        return nullptr;
    }
    return options_[tab][size_t(row)];
}

void MenuUi::Save() {
    if (save_) {
        save_state_ = save_(file_.Text()) ? 1 : 2;
    }
}

void MenuUi::MoveRow(int delta) {
    const int count = RowCount(tab_);
    if (count <= 0) {
        return;
    }
    int& selected = selected_[tab_];
    selected = (selected + delta + count) % count;
    confirm_clear_ = false;
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
    if (tab_ == TabLog) {
        log_source_ ^= 1;
        dirty_ = true;
        return;
    }
    if (IsPresetRow(tab_, selected_[tab_])) {
        const int n = int(Presets().size());
        const int current = file_.MatchPreset();
        // From a custom mix, Right goes to the first profile and Left to the last.
        const int next = current < 0 ? (delta > 0 ? 0 : n - 1) : (current + delta + n) % n;
        file_.ApplyPreset(next);
        Save();
        confirm_reset_ = false;
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
    const int system_options = int(options_[TabSystem].size());
    if (tab_ == TabSystem && selected_[TabSystem] == system_options + 1) {
        if (!confirm_clear_) {
            confirm_clear_ = true;
        } else {
            clear_ok_ = info_.clear_shader_cache ? info_.clear_shader_cache() : false;
            if (clear_ok_) {
                info_.shader_cache_games = 0;
                info_.shader_cache_mib = 0;
            }
            confirm_clear_ = false;
        }
        dirty_ = true;
        return;
    }
    if (tab_ == TabSystem && selected_[TabSystem] == system_options) {
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
        if (IsPresetRow(tab_, selected_[tab_])) {
            file_.ApplyPreset(0);
            Save();
            dirty_ = true;
        } else if (const Option* option = OptionAt(tab_, selected_[tab_])) {
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
    c.Print(1920 - Margin - TextWidth(version), 40, version, Color::Dim);
    if (save_state_ != 0) {
        const std::string status = save_state_ == 1 ? std::string("✓ ") + T("opções salvas", "options saved")
                                                     : std::string("✗ ") + T("NÃO consegui salvar as opções (aba SISTEMA)", "could NOT save the options (SYSTEM tab)");
        c.Print(1920 - Margin - TextWidth(status), 84, status, save_state_ == 1 ? Color::Good : Color::Bad);
    }
    c.Fill(Margin, 130, 1920 - 2 * Margin, 3, Color::Panel);
    c.Fill(Margin, 130, 260, 3, Color::Accent); // a stretch of the theme colour under the title
}

int MenuUi::ThemeIndex() const {
    const Option* option = FindOption("menu_theme");
    return option != nullptr ? file_.ChoiceIndex(*option) : 0;
}

void MenuUi::DrawTabs(Canvas& c) const {
    const char* names_pt[TabCount] = {"JOGOS", "VÍDEO", "CPU E MEMÓRIA", "AVANÇADO", "SISTEMA", "LOG", "DESEMPENHO"};
    const char* names_en[TabCount] = {"GAMES", "VIDEO", "CPU & MEMORY", "ADVANCED", "SYSTEM", "LOG", "PERFORMANCE"};
    const Icon icons[TabCount] = {Icon::Gamepad, Icon::Monitor, Icon::Chip, Icon::Sliders, Icon::Gear, Icon::Document, Icon::Gauge};
    int x = Margin;
    for (int i = 0; i < TabCount; ++i) {
        const std::string name = lang() == Lang::Pt ? names_pt[i] : names_en[i];
        const int w = TextWidth(name) + 76;
        const bool current = i == tab_;
        const std::uint32_t back = current ? Color::Accent : Color::Panel;
        c.RoundRect(x, 148, w, 56, 12, back);
        DrawIcon(c, icons[i], float(x + 18 + 15), 176.0f, 30.0f, current ? 0xFFFFFFFF : Color::Dim, back);
        c.Print(x + 18 + 30 + 10, 156, name, current ? 0xFFFFFFFF : Color::Dim);
        x += w + 14;
    }
}

void MenuUi::DrawGameCard(Canvas& c, int index) const {
    // The right-hand card: the cover of the selected game, its title and what is known about it.
    constexpr int CardX = 1040, CardW = 1920 - Margin - CardX, CardY = ContentTop - 10, CardH = HelpTop - 12 - CardY;
    c.RoundRect(CardX, CardY, CardW, CardH, 16, Color::Panel);
    const bool is_game = index >= 0 && index < int(games_.size());
    const GameEntry* game = is_game ? &games_[size_t(index)] : nullptr;
    const std::string title = is_game ? DisplayName(game->name) : std::string(T("Teste do emulador", "Emulator test"));
    const std::string id = is_game ? TitleIdOf(game->name) : std::string();
    const int cover_x = CardX + (CardW - CoverSize) / 2, cover_y = CardY + 22;
    if (is_game && game->cover.Ok()) {
        // The frame hugs the picture, whatever its proportions (a portrait box art, a wide banner).
        const Image::Pixels& img = game->cover;
        const int ix = cover_x + (CoverSize - img.w) / 2, iy = cover_y + (CoverSize - img.h) / 2;
        c.RoundRect(ix - 6, iy - 6, img.w + 12, img.h + 12, 20, Color::Accent);
        c.DrawImage(ix, iy, img.px.data(), img.w, img.h, 14);
    } else {
        // a soft frame behind the generated tile
        c.RoundRect(cover_x - 6, cover_y - 6, CoverSize + 12, CoverSize + 12, 22, Color::Accent);
        DrawCover(c, cover_x, cover_y, CoverSize, Image::Pixels{}, title, id.empty() ? title : id);
    }
    constexpr int CardCells = (CardW - 60) / GlyphWidth;
    const std::vector<std::string> lines = Wrap(title, CardCells, 2);
    int y = cover_y + CoverSize + 22;
    for (const std::string& line : lines) {
        c.Print(CardX + (CardW - TextWidth(line)) / 2, y, line, 0xFFFFFFFF);
        y += 40;
    }
    std::string info;
    if (is_game) {
        info = FormatSize(game->bytes, lang());
        if (!id.empty()) {
            info += "   ·   " + id;
        }
    } else {
        info = T("Homebrew Menu (teste interno)", "Homebrew Menu (built-in test)");
    }
    info = Clip(info, CardCells);
    c.Print(CardX + (CardW - TextWidth(info)) / 2, y + 6, info, Color::Dim);
    if (is_game && !game->cover.Ok()) {
        const std::string hint = Clip(T("Sem capa no jogo: use covers/", "No cover in the game: use covers/"), CardCells);
        c.Print(CardX + (CardW - TextWidth(hint)) / 2, CardY + CardH - 46, hint, Color::Dim);
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
    } else {
        DrawGameCard(c, selected_[TabGames]);
    }
    // The list narrows when the card takes the right-hand side.
    const int list_w = games_.empty() ? 1920 - 2 * Margin : 1040 - 30 - Margin;
    const int count = RowCount(TabGames);
    const int rows = games_.empty() ? VisibleRows - 3 : VisibleRows;
    const int first = std::clamp(scroll_[TabGames], 0, std::max(count - rows, 0));
    for (int r = 0; r < rows && first + r < count; ++r) {
        const int index = first + r;
        const bool is_game = index < int(games_.size());
        const std::string size = is_game ? FormatSize(games_[size_t(index)].bytes, l) : std::string();
        const int text_x = Margin + 20 + ThumbSize + 18;
        const int name_cells = (list_w - (text_x - Margin) - 24 - TextWidth(size)) / GlyphWidth - 1;
        const std::string title = is_game ? DisplayName(games_[size_t(index)].name) : std::string(T("Teste do emulador (Homebrew Menu)", "Emulator test (Homebrew Menu)"));
        const std::string name = Clip(title, name_cells);
        const bool selected = index == selected_[TabGames];
        const int y = top + r * RowHeight;
        if (selected) {
            c.RoundRect(Margin, y - 4, list_w, RowHeight - 4, 10, Color::Selected);
            c.Fill(Margin, y + 6, 6, RowHeight - 24, Color::Teal);
        }
        const std::string id = is_game ? TitleIdOf(games_[size_t(index)].name) : std::string();
        DrawCover(c, Margin + 20, y - 1, ThumbSize, is_game ? games_[size_t(index)].thumb : Image::Pixels{}, title,
                  id.empty() ? title : id);
        c.Print(text_x, y, name, selected ? 0xFFFFFFFF : (is_game ? Color::Text : Color::Dim));
        if (is_game) {
            c.Print(Margin + list_w - 24 - TextWidth(size), y, size, Color::Dim);
        }
    }
    if (first > 0) {
        c.Print(Margin + list_w - GlyphWidth - 4, top - 36, "▲", Color::Dim);
    }
    if (first + rows < count) {
        c.Print(Margin + list_w - GlyphWidth - 4, top + rows * RowHeight, "▼", Color::Dim);
    }
}

void MenuUi::DrawOptions(Canvas& c) const {
    const Lang l = lang();
    const int count = RowCount(tab_);
    const int first = std::clamp(scroll_[tab_], 0, std::max(count - VisibleRows, 0));
    for (int r = 0; r < VisibleRows && first + r < count; ++r) {
        const int row = first + r;
        const bool selected = row == selected_[tab_];
        const int y = ContentTop + r * RowHeight;
        if (IsPresetRow(tab_, row)) {
            const int match = file_.MatchPreset();
            const std::string value = match >= 0 ? Presets()[size_t(match)].Name(l) : T("Personalizado", "Custom");
            DrawRow(c, y, selected, match != 0, T("Perfil rápido", "Quick profile"), value,
                    match != 0 ? Color::Teal : Color::Text, selected);
            continue;
        }
        const Option& option = *OptionAt(tab_, row);
        const bool changed = !file_.IsDefault(option);
        DrawRow(c, y, selected, changed, option.Label(l), option.ChoiceLabel(file_.ChoiceIndex(option), l),
                changed ? Color::Teal : Color::Text, selected);
    }
    if (first > 0) {
        c.Print(1920 - Margin - GlyphWidth, ContentTop - 36, "▲", Color::Dim);
    }
    if (first + VisibleRows < count) {
        c.Print(1920 - Margin - GlyphWidth, ContentTop + VisibleRows * RowHeight, "▼", Color::Dim);
    }
}

void MenuUi::DrawLog(Canvas& c) const {
    const bool previous = log_source_ == 0;
    const std::vector<std::string>& lines = previous ? info_.log_previous : info_.log_current;
    const std::string title = std::string("◀ ") +
        (previous ? T("Sessão anterior (boot.old.log)", "Previous session (boot.old.log)")
                  : T("Esta sessão, até agora (boot.log)", "This session, so far (boot.log)")) + " ▶";
    c.Print(Margin + 46, ContentTop - 6, title, Color::Teal);
    constexpr int LineHeight = 44;
    constexpr int MaxLines = 13;
    constexpr int Cells = (1920 - 2 * Margin - 60) / GlyphWidth;
    if (lines.empty()) {
        c.Print(Margin + 46, ContentTop + 60,
                previous ? T("Nada ainda: é a primeira vez que o app abre, ou a pasta não aceita gravação.",
                             "Nothing yet: first time the app opens, or the folder cannot be written.")
                         : T("O log desta sessão ainda está vazio.", "This session's log is still empty."),
                Color::Warn);
        return;
    }
    const size_t from = lines.size() > size_t(MaxLines) ? lines.size() - size_t(MaxLines) : 0;
    for (size_t i = from; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const bool bad = line.find("!!") != std::string::npos || line.find("BROKEN") != std::string::npos ||
                         line.find("FAILED") != std::string::npos || line.find("failed") != std::string::npos;
        c.Print(Margin + 46, ContentTop + 52 + int(i - from) * LineHeight, Clip(line, Cells),
                bad ? Color::Bad : Color::Text);
    }
}

void MenuUi::DrawSystem(Canvas& c) const {
    const Lang l = lang();
    const int system_options = int(options_[TabSystem].size());
    for (int i = 0; i < system_options; ++i) {
        const Option& option = *options_[TabSystem][size_t(i)];
        const int y = ContentTop + i * RowHeight;
        const int choice = file_.ChoiceIndex(option);
        DrawRow(c, y, selected_[TabSystem] == i, !file_.IsDefault(option), option.Label(l), option.ChoiceLabel(choice, l),
                file_.IsDefault(option) ? Color::Text : Color::Teal, selected_[TabSystem] == i);
        if (std::string_view(option.key) == "menu_theme") {
            // a swatch of the colour on the left of the value
            const std::string shown = "◀ " + std::string(option.ChoiceLabel(choice, l)) + " ▶";
            c.Disc(float(1920 - Margin - 30 - TextWidth(shown) - 30), float(y + 18), 13.0f, ThemeAccent(choice));
        }
    }
    const std::string reset = confirm_reset_ ? T("Aperte X de novo para confirmar", "Press X again to confirm")
                                             : T("Restaurar todos os padrões", "Restore all defaults");
    {
        const bool selected = selected_[TabSystem] == system_options;
        const int y = ContentTop + system_options * RowHeight;
        if (selected) {
            c.RoundRect(Margin, y - 4, 1920 - 2 * Margin, RowHeight - 4, 10, Color::Selected);
            c.Fill(Margin, y + 6, 6, RowHeight - 24, Color::Teal);
        }
        c.Print(Margin + 46, y, reset, confirm_reset_ ? Color::Warn : (selected ? 0xFFFFFFFF : Color::Text));
    }

    {
        const bool selected = selected_[TabSystem] == system_options + 1;
        const int y = ContentTop + (system_options + 1) * RowHeight;
        const std::string text = confirm_clear_ ? T("Aperte X de novo: apagar o cache de shaders de todos os jogos", "Press X again: delete every game's shader cache")
                                                : T("Limpar o cache de shaders (se um jogo não abre)", "Clear the shader cache (if a game will not open)");
        if (selected) {
            c.RoundRect(Margin, y - 4, 1920 - 2 * Margin, RowHeight - 4, 10, Color::Selected);
            c.Fill(Margin, y + 6, 6, RowHeight - 24, Color::Teal);
        }
        c.Print(Margin + 46, y, text, confirm_clear_ ? Color::Warn : (selected ? 0xFFFFFFFF : Color::Text));
    }
    // Read-only facts.
    const int label_x = Margin + 46, value_x = Margin + 46 + 24 * GlyphWidth;
    const int max_cells = (1920 - Margin - value_x) / GlyphWidth;
    int y = ContentTop + (system_options + 2) * RowHeight + 24;
    c.Fill(Margin, y - 18, 1920 - 2 * Margin, 2, Color::Panel);
    const auto row = [&](const char* label, const std::string& value, std::uint32_t color) {
        c.Print(label_x, y, label, Color::Dim);
        c.Print(value_x, y, Clip(value, max_cells), color);
        y += 38;
    };
    row(T("Versão", "Version"), info_.build_id, Color::Text);
    {
        // Can the app write in its data folder? Without it there is no boot.log and no settings.txt.
        std::string value;
        std::uint32_t color = Color::Good;
        if (!info_.data_writable) {
            value = std::string("✗ ") + T("sem permissão de gravação em ", "cannot write to ") + info_.data_dir;
            color = Color::Bad;
        } else if (info_.log_bytes >= 0) {
            value = "✓ ok, boot.log " + std::to_string((info_.log_bytes + 1023) / 1024) + " KiB";
        } else {
            value = std::string("✓ ok, ") + T("mas o boot.log não existe", "but boot.log does not exist");
            color = Color::Warn;
        }
        row(T("Gravação", "Writing"), value, color);
    }
    row("settings.txt",
        info_.settings_bytes >= 0 ? std::string("✓ ") + std::to_string(info_.settings_bytes) + T(" bytes: as opções ficam salvas aqui", " bytes: your options are kept here")
                                  : std::string(T("ainda não existe (nenhuma opção alterada)", "does not exist yet (no option changed)")),
        info_.settings_bytes >= 0 ? Color::Good : Color::Dim);
    row(T("Chaves (prod.keys)", "Keys (prod.keys)"),
        info_.keys_found ? std::string("✓ ") + T("encontradas", "found")
                         : std::string("✗ ") + T("não encontradas: copie para ", "not found: copy to ") + info_.data_dir + "/keys",
        info_.keys_found ? Color::Good : Color::Bad);
    std::string firmware;
    std::uint32_t firmware_color = Color::Good;
    if (info_.firmware_installed > 0) {
        firmware = "✓ " + std::to_string(info_.firmware_installed) + T(" arquivos .nca instalados", " .nca files installed");
    } else if (info_.firmware_files > 0) {
        firmware = "✓ " + std::to_string(info_.firmware_files) + T(" arquivos .nca (instalados ao iniciar um jogo)", " .nca files (installed when a game starts)");
    } else if (info_.firmware_zips > 0) {
        firmware = std::string("✓ ") + T("firmware em .zip (instalado ao iniciar um jogo)", "firmware as a .zip (installed when a game starts)");
    } else {
        firmware = std::string("✗ ") + T("nenhum arquivo: copie o .zip ou os .nca para ", "no files: copy the .zip or the .nca files to ") + info_.data_dir + "/firmware";
        firmware_color = Color::Warn;
    }
    row("Firmware", firmware, firmware_color);
    row(T("Jogos", "Games"), std::to_string(games_.size()) + T(" em ", " in ") + info_.data_dir + "/roms", Color::Text);
    if (info_.memory_budget_mib > 0) {
        row(T("Memória direta", "Direct memory"),
            std::to_string(info_.free_memory_mib) + T(" MiB livres de ", " MiB free of ") + std::to_string(info_.memory_budget_mib) + " MiB (" +
                info_.memory_tier + ")",
            Color::Text);
        row(T("Plano de memória", "Memory plan"), info_.memory_plan, Color::Text);
    } else {
        row(T("Memória direta livre", "Free direct memory"), std::to_string(info_.free_memory_mib) + " MiB", Color::Text);
    }
    row(T("Cache de shaders", "Shader cache"),
        info_.shader_cache_games > 0 ? std::string("✓ ") + std::to_string(info_.shader_cache_games) + T(" jogos, ", " games, ") +
                                           std::to_string(info_.shader_cache_mib) + T(" MiB: um por jogo, ao jogar", " MiB: one per game, filled as you play")
                                     : std::string(T("vazio: é criado e preenchido enquanto você joga (um por jogo)", "empty: made and filled as you play (one per game)")),
        info_.shader_cache_games > 0 ? Color::Good : Color::Dim);
    row(T("Dados e logs", "Data and logs"), info_.data_dir + "  (boot.log, mesa.log, log/)", Color::Text);
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
    } else if (tab_ == TabLog) {
        text = T("◀▶ trocar de sessão   L1/R1: abas   O: voltar", "◀▶ switch session   L1/R1: tabs   O: back");
    }
    c.Print(Margin, FooterTop, Clip(text, FooterCells), Color::Dim);
}

void MenuUi::Draw(Canvas& c) const {
    ApplyTheme(ThemeIndex());
    DrawHeader(c);
    DrawTabs(c);
    std::vector<std::string> help;
    constexpr int HelpCells = (1920 - 2 * Margin - 52) / GlyphWidth;
    if (tab_ == TabGames) {
        DrawGames(c);
        const bool on_homebrew = selected_[TabGames] >= int(games_.size());
        help = Wrap(on_homebrew
                        ? T("Teste interno. A mensagem \"launchInit() failed\" é ESPERADA (o Homebrew Menu precisa do nx-hbloader): se ela aparece, o emulador roda. OPTIONS sai.",
                            "Built-in test. The message \"launchInit() failed\" is EXPECTED (the Homebrew Menu needs nx-hbloader): if it shows, the emulator runs. OPTIONS quits.")
                        : T("Escolha um jogo e aperte X. As opções das outras abas valem ao iniciar. A capa vem do próprio jogo; para trocar, ponha uma imagem em covers/ (ID do título ou nome do jogo).",
                            "Pick a game and press X. The options in the other tabs apply at start. The cover comes from the game itself; to replace it, put a picture in covers/ (title id or game file name)."),
                    HelpCells, 2);
    } else if (tab_ == TabLog) {
        DrawLog(c);
        help = Wrap(T("◀▶ troca entre a sessão anterior e a atual. Depois de um travamento, abra o app de novo e fotografe esta tela para relatar o problema.",
                      "◀▶ switches between the previous session and this one. After a crash, open the app again and photograph this screen to report it."),
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
        if (IsPresetRow(tab_, selected_[tab_])) {
            const int match = file_.MatchPreset();
            help = Wrap(match >= 0 ? Presets()[size_t(match)].Help(lang())
                                   : T("Mistura personalizada de opções. ◀▶ aplica um perfil pronto: Padrão, Leve ou Compatibilidade.",
                                       "A custom mix of options. ◀▶ applies a ready profile: Default, Light or Compatibility."),
                        HelpCells, 2);
        } else if (const Option* option = OptionAt(tab_, selected_[tab_])) {
            help = Wrap(option->Help(lang()), HelpCells, 2);
        }
    }
    DrawHelp(c, help);
    DrawFooter(c);
}

} // namespace Menu
