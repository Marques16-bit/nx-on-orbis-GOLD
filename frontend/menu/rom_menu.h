// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: the menu shown before the emulator starts (the games and the settings; see
// menu_ui.h for the screens). This is the console side: video-out and the DualShock 4.

#pragma once

#include <vector>

#include "menu/menu_ui.h"

enum class MenuChoice {
    Failed,   // the display could not be opened: nothing was shown
    Game,     // `game` is an index into the list given
    Homebrew, // the built-in Homebrew Menu
};

struct MenuOutcome {
    MenuChoice choice = MenuChoice::Failed;
    int game = -1;
};

/// Shows the menu until a game (or the Homebrew Menu) is picked. Options changed in it are saved
/// to settings.txt as they change, so they apply to the game that is about to start.
MenuOutcome RunMainMenu(const std::vector<Menu::GameEntry>& games, int initial,
                        const Menu::SystemInfo& info);
