#pragma once

// Arrival on a gameobject floor. Defined in ArrivalFloor.cpp.
//
// A building placed as a gameobject (a MAP_OBJECT WMO) is solid on the server
// from the moment its grid loads, because its model is in the map's dynamic
// tree. The client is slower. After a loading screen it applies gravity before
// it has loaded the building, so the player and the creatures inside drop: some
// a yard into the floor, some to the terrain under it. Seen in the Darnassus zoo
// (map 1), 2026-10-27.
//
// Two steps for the player, both started when the client is in the world:
//
// - THE PLAYER IS HELD FIRST. When the arrival point is a gameobject floor, the
//   server sends the player's own client a heartbeat every ARRIVAL_HOLD_EVERY ms
//   for ARRIVAL_HOLD_MS: the client's current x, y and facing, with the arrival
//   floor's z. The hold ends early when the player walks or jumps. Because the
//   x and y are the client's own, a moving player is not pulled back.
//
// - THEN CORRECTED. For the rest of ARRIVAL_WATCH_MS the server checks
//   the player's reported position. If the player is below a gameobject floor
//   that was under them at arrival height, they are put on that floor. The
//   packet keeps the x, y and facing the client reported, so only z changes.
//   A load slower than the hold ends here.
//
// The hold ends at the first walk or jump. The correction ends at the first
// verified step: the player has moved, is on the ground, and the client's z is
// on a floor the server has under them. Under a gameobject floor at arrival
// height, only a gameobject surface counts, so the terrain under a house does
// not. While a player who has moved is in the air, the correction waits; a
// fall in place is corrected in the air.
//
// Map 28 is skipped: HouseMgr has its own arrival handling there.
//
// THE CREATURE BEAT. The server sends a creature's position once, in the create
// packet, and again only when it moves. The client applies gravity to a
// creature before it has loaded the building under it, after a loading screen
// and also when the player walks in from far away. So every BEAT_EVERY ms, each
// idle creature and each still bot NPC (a Player, BotNpcMgr) within 60 yd of a
// real player gets its own position sent again as a teleport packet. Nothing is
// relocated on the server.
//
// Modes, set with `.floorbeat`: off; plain (one packet at the creature's z,
// the default); lift (one packet 0.05 yd up, then one at z). Lift makes some
// creatures hop on each beat; plain showed no hops in the zoo (2026-11-04).

#include "Common.h"

class Map;
class Player;

namespace ArrivalFloor
{
    // The client has left a loading screen (CMSG_SET_ACTIVE_MOVER, the
    // worldport ack) or acked a near teleport.
    void Arm(Player* player);

    // Called from Map::Update.
    void Update(Map* map, uint32 diff);

    enum class BeatMode : uint8 { Off = 0, Plain = 1, Lift = 2 };

    void     SetBeat(BeatMode mode, uint32 everyMs);
    BeatMode GetBeatMode();
    uint32   GetBeatEveryMs();

    // The last beat on any map: creatures and bot NPCs sent, and the real
    // players they were near.
    void     GetLastBeat(uint32& creatures, uint32& botNpcs, uint32& players, uint32& mapId);
}
