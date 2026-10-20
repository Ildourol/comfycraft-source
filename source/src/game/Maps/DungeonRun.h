#pragma once

// The dungeon run timer. Defined in DungeonRun.cpp; the boss lists are in
// DungeonBosses.h.
//
// A run starts when the first player enters a fresh instance of a dungeon that
// has a boss list. Each boss kill sends a split to everyone in the instance.
// The run ends when every required boss is dead: a chat line gives the time
// and the group size, and the run is saved to tw_char (dungeon_run,
// dungeon_run_boss, dungeon_run_member; sql/custom/161_dungeon_runs.sql).
//
// On a split map (Scarlet Monastery, Dire Maul, Blackrock Spire, Stratholme)
// the first boss kill picks the wing, and kills in other wings are ignored.
//
// Runs are in memory only. Instance ids are packed again at startup, so a run
// cannot be found again after a restart: an instance with kills from before a
// restart is not timed.

#include "Common.h"

class Creature;
class DungeonMap;
class Player;

namespace DungeonRun
{
    // Probes the tables. Called once at startup.
    void Load();

    // False when the tables are missing. Nothing is then written.
    bool TablesPresent();

    // Called from DungeonMap::Add, after the player is in the map.
    void OnPlayerEnter(DungeonMap* map, Player* player);

    // Called from the kill path in Unit.cpp for every creature that dies.
    void OnCreatureKilled(Creature* creature);

    // Called when an instance's persistent state is deleted (reset or unload).
    void OnInstanceGone(uint32 mapId, uint32 instanceId);
}
