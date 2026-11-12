// SPDX-License-Identifier: MIT
// The game's own UI font, decoded at run time from the user's extracted files. Nothing from the game is copied into the repo.
#pragma once
#include <string>
#include <vector>

namespace snap::ui {

// Decodes AsiaKNGD-E.bfttf (a Switch shared-font container: 8-byte header, then the TTF XORed with a repeating 4-byte key) from the
// extracted game files. Looks in env SNAP_GAME_FONT (a .bfttf path), then <romfs parent>/drp-v262144/..., then ./extracted/....
// Returns false when the file is missing or does not decode to a TrueType font; callers then keep their current font.
bool game_font_ttf(std::vector<unsigned char>& out);
const char* game_font_family();   // GDI/ImGui name of that font ("AsiaKNGD-E")

}  // namespace snap::ui
