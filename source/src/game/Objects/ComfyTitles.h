#pragma once

// ComfyCraft's own titles. A title id is a byte the client looks up as
// PVP_MEDAL<id>, a string that patch-V sets (patches/titles.lua). Octo's
// client uses 1-67 and 90-93, and Player::AwardTitle takes an int8, so ours
// stay between 94 and 127.
//
// Its own header, not the PlayerTitles enum in Player.h: this one reaches
// Player.cpp and HonorMgr.cpp, Player.h reaches nearly every object.

#include "Common.h"

// The alpha reward. The client draws a title under the name, but this one
// goes in front of it ("Unpaid Intern Luf"), in the PvP rank's place. While
// it is the active title, the title byte is 0 and the rank byte is
// UNPAID_INTERN_RANK. The honor system stops at rank 18, so 19 is ours, and
// patch-V names rank 19 "Unpaid Intern". The real rank comes back when the
// player picks another title.
constexpr uint8 TITLE_UNPAID_INTERN = 120;
constexpr uint8 UNPAID_INTERN_RANK  = 19;

// During the alpha every player's character gets it at login
// (WorldSession::HandlePlayerLogin). Set to false when the alpha ends:
// characters that have it keep it.
constexpr bool UNPAID_INTERN_AT_LOGIN = true;

// PLAYER_BYTES_3 byte 2: the title shown under the name.
inline uint8 ComfyTitleUnderName(uint8 activeTitle)
{
    return activeTitle == TITLE_UNPAID_INTERN ? 0 : activeTitle;
}

// PLAYER_BYTES_3 byte 3: the PvP rank shown in front of the name.
inline uint8 ComfyRankBeforeName(uint8 activeTitle, uint8 honorRank)
{
    return activeTitle == TITLE_UNPAID_INTERN ? UNPAID_INTERN_RANK : honorRank;
}
