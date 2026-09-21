/*
 * ComfyCraft -- dialog destinations.
 *
 * Named places a gossip button can SEND A PLAYER to, grouped into lists so a
 * button can pick one at random. `dialog_dest_list` and `dialog_dest`, written
 * by `.dest` in game and read by SCRIPT_COMMAND_TELEPORT_RANDOM (100).
 *
 * WHY THIS IS NOT dialog_spot, in one line: a spot is where an NPC walks and a
 * script stores its coordinates; a destination is where a player lands and a
 * script stores the LIST, because the pick has to happen when the button is
 * pressed. sql/custom/105 has the long version.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef COMFY_DIALOG_DEST_H
#define COMFY_DIALOG_DEST_H

#include "Common.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// One place, as the teleport needs it. `name` is carried for the log line when
// something goes wrong; nothing in the hot path reads it.
struct DialogDestPoint
{
    uint32 map;
    float x, y, z, o;
    std::string name;
};

// One list, in the order the rows came back. Order is not meaningful -- the
// pick is uniform -- but a stable one keeps a log line reproducible.
struct DialogDestList
{
    std::string name;
    std::vector<DialogDestPoint> points;
};

/*
 * EVERY LIST, AS ONE IMMUTABLE BLOCK.
 *
 * The same shape LuaSceneData has and for the same reason: maps update in
 * PARALLEL here, so `.dest` and `reload dialog_dests` cannot edit a container a
 * map thread may be reading. A reload builds a whole new block and swaps the
 * pointer; a reader takes a copy of the shared_ptr and keeps the block it
 * started with alive until it is done with it.
 *
 * There is no `generation` here because nothing caches anything derived from
 * this. A teleport reads the block, picks a point and is finished with it
 * inside one call.
 */
struct DialogDestData
{
    std::unordered_map<uint32, DialogDestList> lists;   // dialog_dest_list.id -> its places
};

namespace DialogDest
{
    // Read both tables into a fresh block and swap it in. Startup, `.dest`, and
    // `reload dialog_dests` -- all on the world thread.
    void LoadAll();

    // The current block, safe to hold across anything.
    std::shared_ptr<DialogDestData const> Data();
}

#endif
