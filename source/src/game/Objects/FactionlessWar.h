#pragma once

// Factionless war: quest mobs that stay at war. Factionless.PeaceFactions
// lifts every race to Friendly with the other side's factions, and that made
// the other side's quest mobs friendly too (the Hillsbrad militia, Kul Tiras
// at Tiragarde, Kalman Azureborn). A creature in
// tw_world.factionless_war_creature is hostile to a player who has a quest
// against it in the log, and friendly to everyone else. The table is
// sql/custom/205_factionless_war_creatures.sql.
//
// The war follows the quest, not the race, so every race can take every
// quest: a Horde player can still hand in to Deckmaster Darkhollow, and an
// Alliance player who takes Cutting Ranks can kill him.
//
// The peace list is the reverse. A creature in
// tw_world.factionless_peace_creature keeps its hostile template, and is
// friendly to a player who has the quest of its row in the log. Sharptalon
// is a hostile beast for the Ashenvale Hunt, and friendly to a player who
// has The Family Name (40800), whose objective is to talk to him.
//
// Defined in Object.cpp, beside GetReactionTo.

#include "Common.h"

class Creature;
class Player;
class WorldObject;

namespace FactionlessWar
{
    // The template a viewer at war is sent in place of the creature's own:
    // Monster. The client resolves its reaction from the template alone, so
    // it draws the creature red and allows the attack.
    uint32 const ViewerFactionTemplate = 14;

    // The template a viewer at peace is sent: Friendly. The client draws the
    // creature green and offers its gossip.
    uint32 const PeaceFactionTemplate = 35;

    // Reads factionless_peace_creature, then factionless_war_creature, and
    // finds the quests against the war creatures. Run after the quests,
    // creature templates and factions load. False when the war table is
    // missing. A missing peace table leaves the peace list empty.
    bool Load();

    // True when target is a listed creature and player has a quest against
    // it in the log, not yet handed in. A companion counts its owner's log.
    // False while Factionless.Enable is off.
    bool AtWar(Player const* player, WorldObject const* target);

    // True when target is on the peace list and player has the quest of its
    // row in the log, not yet handed in. A companion counts its owner's log.
    // False while Factionless.Enable is off.
    bool AtPeace(Player const* player, WorldObject const* target);

    // True when the creature is on either list, or was summoned by a war
    // creature. Player::UpdateForQuestWorldObjects resends the faction field
    // of these when the quest log changes.
    bool IsListedCreature(Creature const* creature);

    uint32 CreatureCount();
    uint32 QuestCount();
    uint32 CampCount();
    uint32 PeaceCount();
}
