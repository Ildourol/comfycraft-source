#pragma once

// The server's level cap: the highest level XP can take a character to, read
// from tw_char.level_cap (sql/custom/134_level_cap.sql). It sits under
// MaxPlayerLevel, which sizes the level stat tables at startup and so cannot
// change while the server runs. The cap can.
//
// It stops XP and nothing else. A character above the cap keeps its level.
// Defined in Player.cpp, beside GiveXP.

#include "Common.h"

namespace LevelCap
{
    // The cap, or MaxPlayerLevel when there is none.
    uint32 Get();

    // Re-reads the row. Returns the stored cap, 0 for none.
    uint32 Load();

    // Writes the row and applies it at once. 0 is no cap. False when the
    // table is missing.
    bool Set(uint32 level, char const* changedBy);
}
