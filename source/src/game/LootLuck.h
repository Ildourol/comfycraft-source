#pragma once

// Quest drops roll for each player who needs them, with optional bad-luck
// protection. Defined in LootMgr.cpp.
//
// At a creature kill, every eligible looter (the killer, or each group member
// in reward range) who holds a quest that needs an item rolls for it alone. A
// hit puts a copy in the corpse that only that player can see. A normal drop
// that a quest needs also rolls once for the looters who do not need it.
// Quest-only items in a loot group leave the group's shared roll and roll this
// way at their own chance.
//
// PRD (pseudo-random distribution), when the switch is on: the chance starts
// low and rises with each miss, with a step chosen so the average stays at the
// table chance. Each player keeps one count for each item, as missed chance (a
// miss on a 5% mob adds 5), so killing a fast low-chance mob first does not
// buy a better roll on a slow high-chance one. The count is in memory: a drop
// or a logout resets it.
//
// The switch is tw_char.loot_luck, id 1 (sql/custom/160_loot_luck.sql).

#include "Common.h"

namespace LootLuck
{
    // True when PRD is on.
    bool Enabled();

    // Re-reads the row. Returns the stored setting.
    bool Load();

    // Writes the row and applies it at once. False when the table is missing.
    bool Set(bool on, char const* changedBy);

    // One line in the loot log for each quest roll. In memory, off at start.
    bool Logging();
    void SetLogging(bool on);

    // Players with a miss count, and the counts they hold.
    void Counts(uint32& players, uint32& items);

    // A player's missed chance for an item, in percent. 0 when none.
    float Missed(uint32 playerGuidLow, uint32 itemId);

    // Drops a player's counts. Called at logout.
    void Forget(uint32 playerGuidLow);
}
