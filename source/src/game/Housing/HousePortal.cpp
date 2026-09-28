/*
 * The way in.
 *
 * `.house go` works, but typing a command is not an entrance. This puts a
 * portal in the world -- in the doorway of a real house, wherever you choose --
 * and walking into it takes you to your own instance. A second portal inside
 * the house walks you back out to the same doorway.
 *
 * Three things decided the shape of this, all of them found by reading:
 *
 *  - AN AREATRIGGER CANNOT BE USED, even though that is exactly what a real
 *    dungeon entrance is. CMSG_AREATRIGGER is sent by the CLIENT when it walks
 *    into a trigger box listed in its own AreaTrigger.dbc, and the server only
 *    ever reacts to it (MiscHandler.cpp:770). There is no way to add one, so
 *    trigger locations are stuck wherever Blizzard put them. ScriptMgr's
 *    OnAreaTrigger hook and the `scripted_areatrigger` table are real and
 *    usable -- they are just useless for a portal that has to go somewhere new.
 *
 *  - GAMEOBJECTS TICK, which is what makes walk-through possible anyway.
 *    GameObject::Update calls AI()->UpdateAI(diff) for every gameobject in a
 *    loaded grid (GameObject.cpp:338), and GameObjectAI is bound by
 *    gameobject_template.script_name through ScriptMgr::GetGameObjectAI. So the
 *    portal watches for somebody standing in it. Polling rather than a callback,
 *    because no "a player entered my radius" event exists for a gameobject.
 *
 *  - THE SEARCH MUST BE GRID-LIMITED. The obvious loop over map->GetPlayers()
 *    is what the edit handles do, and it is right there because a house holds
 *    one player. The world portal sits on a continent carrying 300 random bots,
 *    where a full scan four times a second would be a real cost for nothing.
 *    Cell::VisitWorldObjects only ever looks at the cells within range.
 *
 * The entrance is a persistent spawn registered with sObjectMgr's GLOBAL grid
 * set, not a house instance's -- it belongs to the world, so it loads with the
 * world's grids exactly like any tw_world.gameobject row. The exit portal inside
 * each house is the opposite: a temporary summon, re-made whenever the instance
 * loads, so it needs no carved-out guid block. Same reasoning as the handles.
 */

#include "Housing/HouseMgr.h"

#include "CellImpl.h"
#include "Chat.h"
#include "Database/DatabaseEnv.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "Nostalrius.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "Timer.h"
#include "WorldSession.h"

#include <cctype>
#include <sstream>
#include <string>

namespace
{
    // Stepping out of a portal must not immediately step back into it. The
    // arrival offsets below already land you clear of the ring, so this is the
    // belt to their braces -- and it also stops a refused trip (in combat, say)
    // reprinting its reason four times a second.
    time_t const PORTAL_COOLDOWN_SEC = 5;
}

//== where the portals are ===================================================

// A row present means placed. There is deliberately no "map 0 means unset"
// convention: map 0 is Eastern Kingdoms, the single most likely place of all to
// want a doorway to be.
void HouseMgr::LoadPortals()
{
    m_portals.clear();

    std::unique_ptr<QueryResult> result(CharacterDatabase.Query(
        "SELECT id, map, x, y, z, o, name, template_id FROM house_portal ORDER BY id"));
    if (!result)
        return;

    do
    {
        Field* f = result->Fetch();
        HousePortal p;
        p.id   = f[0].GetUInt32();
        p.map  = f[1].GetUInt32();
        p.x    = f[2].GetFloat();
        p.y    = f[3].GetFloat();
        p.z    = f[4].GetFloat();
        p.o    = f[5].GetFloat();
        p.name = f[6].GetCppString();
        p.templateId = f[7].GetUInt32();

        // LoadTemplates ran first, so this catches a template deleted out from
        // under a door by hand-edited SQL. Loaded as untemplated rather than
        // dropped: the door still works, it just claims an empty house.
        if (p.templateId && !GetTemplate(p.templateId))
        {
            sLog.outErrorDb("Housing: entrance %u names template %u, which does not exist; loaded untemplated.",
                            p.id, p.templateId);
            p.templateId = 0;
        }

        // An id is an ADDRESS here, not merely a key -- the gameobject guid is
        // derived from it -- so one past the block would land silently on
        // somebody else furniture. Refuse rather than collide.
        if (!p.id || p.id > HOUSE_PORTAL_MAX_ID)
        {
            sLog.outError("Housing: entrance portal id %u is outside 1..%u and was skipped; "
                          "its guid would collide with furniture.", p.id, HOUSE_PORTAL_MAX_ID);
            continue;
        }

        m_portals[p.id] = p;
    }
    while (result->NextRow());

    sLog.outString(">> Housing: %u entrance portal(s).", uint32(m_portals.size()));
}

HousePortal const* HouseMgr::GetPortal(uint32 id) const
{
    std::map<uint32, HousePortal>::const_iterator itr = m_portals.find(id);
    return itr == m_portals.end() ? nullptr : &itr->second;
}

std::vector<HousePortal const*> HouseMgr::GetPortals() const
{
    std::vector<HousePortal const*> out;
    for (std::map<uint32, HousePortal>::const_iterator itr = m_portals.begin(); itr != m_portals.end(); ++itr)
        out.push_back(&itr->second);
    return out;
}

// Nearest on the SAME MAP. Distance between maps is meaningless, and comparing
// it anyway would cheerfully hand somebody in Ironforge a door in Kalimdor.
HousePortal const* HouseMgr::GetNearestPortal(Player* player) const
{
    if (!player)
        return nullptr;

    HousePortal const* best = nullptr;
    float bestDist = 0.0f;

    for (std::map<uint32, HousePortal>::const_iterator itr = m_portals.begin(); itr != m_portals.end(); ++itr)
    {
        HousePortal const& p = itr->second;
        if (p.map != player->GetMapId())
            continue;

        float const dx = p.x - player->GetPositionX();
        float const dy = p.y - player->GetPositionY();
        float const dz = p.z - player->GetPositionZ();
        float const d  = dx * dx + dy * dy + dz * dz;

        if (!best || d < bestDist)
        {
            best = &p;
            bestDist = d;
        }
    }
    return best;
}

// "The entrance you are standing at" -- which is what all four bare-form
// commands mean, and what all four of them already SAID in their error text
// while the code meant "the nearest one anywhere on this map". That gap made
// .house entrance remove delete a door across the zone without a word.
HousePortal const* HouseMgr::GetPortalNear(Player* player) const
{
    HousePortal const* p = GetNearestPortal(player);   // null-checks the player
    if (!p)
        return nullptr;

    // GetDistance, not the raw squared distance the search above uses, because
    // this is the measure `.house object` already means by "nearest" and the
    // one `entrance where` prints. Two ways of measuring the same yard is how
    // a report ends up disagreeing with the rule it is reporting on.
    return player->GetDistance(p->x, p->y, p->z) <= HOUSE_PORTAL_NEAR ? p : nullptr;
}

bool HouseMgr::AddPortal(Player* player, std::string& name, uint32 templateId, uint32& newId, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }
    if (!player)
        return false;

    if (player->GetMapId() == HOUSE_MAP_ID)
    {
        error = "An entrance has to be out in the world, not inside a house.";
        return false;
    }
    if (m_portals.size() >= HOUSE_PORTAL_MAX_ID)
    {
        error = "There is no room for another entrance.";
        return false;
    }
    if (templateId && !GetTemplate(templateId))
    {
        error = "There is no template with that id.";
        return false;
    }

    HousePortal p;
    p.templateId = templateId;

    // Ahead of the player rather than under their feet, facing back at them, so
    // that placing it and walking forward is the thing that uses it. Same
    // reasoning and the same distance as .house place.
    float const face = player->GetOrientation();
    p.map  = player->GetMapId();
    p.x    = player->GetPositionX() + cos(face) * HOUSE_PORTAL_PLACE_DISTANCE;
    p.y    = player->GetPositionY() + sin(face) * HOUSE_PORTAL_PLACE_DISTANCE;
    p.z    = player->GetPositionZ();
    p.o    = face + M_PI_F;                     // looking back the way you came
    p.name = name;

    std::string safeName = name;
    CharacterDatabase.escape_string(safeName);

    // DirectPExecute, not PExecute: the id is read back on the very next lines
    // and the queued form would race it. Same trap CreateHouse documents.
    CharacterDatabase.DirectPExecute(
        "INSERT INTO house_portal (map, x, y, z, o, name, created_at, template_id) "
        "VALUES (%u, %f, %f, %f, %f, '%s', " UI64FMTD ", %u)",
        p.map, p.x, p.y, p.z, p.o, safeName.c_str(), uint64(time(nullptr)), p.templateId);

    std::unique_ptr<QueryResult> result(CharacterDatabase.Query("SELECT MAX(id) FROM house_portal"));
    if (!result)
    {
        error = "Could not save the entrance.";
        return false;
    }

    p.id = result->Fetch()[0].GetUInt32();
    if (!p.id || p.id > HOUSE_PORTAL_MAX_ID)
    {
        error = "There is no room for another entrance.";
        return false;
    }

    m_portals[p.id] = p;
    newId = p.id;

    SpawnPortalObject(m_portals[p.id]);

    // Placing it three yards ahead would otherwise send the placer home on the
    // very next tick.
    m_portalCooldown[player->GetGUIDLow()] = time(nullptr) + PORTAL_COOLDOWN_SEC;
    return true;
}

// Moving one is its own verb now, .house entrance move -- adjustment is the
// common case: a yard left, turned
// to face the other way -- and a command whose bare form litters the world with
// duplicates every time you nudge it is the wrong default.
bool HouseMgr::MovePortal(Player* player, uint32 id, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }
    if (!player)
        return false;

    if (player->GetMapId() == HOUSE_MAP_ID)
    {
        error = "An entrance has to be out in the world, not inside a house.";
        return false;
    }

    std::map<uint32, HousePortal>::iterator itr = m_portals.find(id);
    if (itr == m_portals.end())
    {
        error = "There is no entrance with that id.";
        return false;
    }

    // Take the old object AND ITS GEAR down before the position changes, or both
    // teardowns look for them on the map they are about to stop being on. The
    // gear especially: UpdatePortalMarker only grows one that is missing, so a
    // marker left standing means the door moves and its gear does not -- an
    // orphan at the old spot, and no gear at all at the new one.
    DespawnPortalMarker(id, itr->second.map);
    DespawnPortalObject(itr->second);

    HousePortal& p = itr->second;
    float const face = player->GetOrientation();
    p.map = player->GetMapId();
    p.x   = player->GetPositionX() + cos(face) * HOUSE_PORTAL_PLACE_DISTANCE;
    p.y   = player->GetPositionY() + sin(face) * HOUSE_PORTAL_PLACE_DISTANCE;
    p.z   = player->GetPositionZ();
    p.o   = face + M_PI_F;

    CharacterDatabase.PExecute(
        "UPDATE house_portal SET map = %u, x = %f, y = %f, z = %f, o = %f WHERE id = %u",
        p.map, p.x, p.y, p.z, p.o, p.id);

    SpawnPortalObject(p);

    m_portalCooldown[player->GetGUIDLow()] = time(nullptr) + PORTAL_COOLDOWN_SEC;
    return true;
}

bool HouseMgr::RemovePortal(uint32 id, std::string& error)
{
    std::map<uint32, HousePortal>::iterator itr = m_portals.find(id);
    if (itr == m_portals.end())
    {
        error = "There is no entrance with that id.";
        return false;
    }

    // Before the erase, and in this order: both teardowns need the row to know
    // which map to look on, and DespawnPortalMarkers could never clean up after
    // us -- it finds a marker's map through GetPortal, which is about to return
    // null for this one. A gear missed here floats until the grid unloads it.
    DespawnPortalMarker(id, itr->second.map);
    DespawnPortalObject(itr->second);
    m_portals.erase(itr);

    CharacterDatabase.PExecute("DELETE FROM house_portal WHERE id = %u", id);

    // A house that called it home is now homeless rather than pointing at a door
    // that is not there. Its way out falls back to the hearthstone, which is the
    // same answer an unplaced portal always gave.
    CharacterDatabase.PExecute("UPDATE house SET portal_id = 0 WHERE portal_id = %u", id);
    for (std::map<uint32, House>::iterator h = m_houses.begin(); h != m_houses.end(); ++h)
        if (h->second.portalId == id)
            h->second.portalId = 0;

    return true;
}

bool HouseMgr::SetHomePortal(Player* player, uint32 portalId, std::string& error)
{
    if (!player)
        return false;

    if (portalId && !GetPortal(portalId))
    {
        error = "There is no entrance with that id.";
        return false;
    }

    House* house = GetHouseByAccount(player->GetSession()->GetAccountId());
    if (!house)
    {
        error = "You have no home yet. Claim one at an entrance first.";
        return false;
    }

    house->portalId = portalId;
    CharacterDatabase.PExecute("UPDATE house SET portal_id = %u WHERE id = %u", portalId, house->id);
    return true;
}

// Retro-binding: point an existing door at a template (or 0 = none). What was
// already claimed through it is untouched -- stamping copies, never references.
bool HouseMgr::SetPortalTemplate(uint32 portalId, uint32 templateId, std::string& error)
{
    auto itr = m_portals.find(portalId);
    if (itr == m_portals.end())
    {
        error = "There is no entrance with that id.";
        return false;
    }
    if (templateId && !GetTemplate(templateId))
    {
        error = "There is no template with that id.";
        return false;
    }

    itr->second.templateId = templateId;
    CharacterDatabase.PExecute("UPDATE house_portal SET template_id = %u WHERE id = %u", templateId, portalId);
    return true;
}

//== spawning them ===========================================================

void HouseMgr::SpawnPortals()
{
    if (!m_enabled)
        return;

    for (std::map<uint32, HousePortal>::iterator itr = m_portals.begin(); itr != m_portals.end(); ++itr)
        SpawnPortalObject(itr->second);
}

void HouseMgr::SpawnPortalObject(HousePortal const& p)
{
    if (!m_enabled || !p.id)
        return;

    uint32 const guid = HOUSE_PORTAL_GUID_MIN + p.id;

    GameObjectData& data = sObjectMgr.NewGOData(guid);
    data.id                = HOUSE_PORTAL_ENTRY;
    data.position.mapId    = p.map;
    data.position.x        = p.x;
    data.position.y        = p.y;
    data.position.z        = p.z;
    data.position.o        = p.o;
    // Facing is carried by the orientation alone, exactly as furniture does it.
    data.rotation0         = 0.0f;
    data.rotation1         = 0.0f;
    data.rotation2         = 0.0f;
    data.rotation3         = 0.0f;
    data.spawntimesecsmin  = 0;                 // never despawns
    data.spawntimesecsmax  = 0;
    data.animprogress      = GO_ANIMPROGRESS_DEFAULT;
    // CREATED READY, AND THAT IS WHAT MAKES IT CLICKABLE. Spawning it ACTIVE
    // cost most of a night. The 1.12 client decides whether an object is usable
    // from its CREATION block, and a goober that arrives already ACTIVE is one
    // it treats as spent: no cursor, and CMSG_GAMEOBJ_USE is never sent, so no
    // amount of server-side work can rescue it.
    //
    // Proven by elimination, not guessed. The edit handle and a plain
    // `.gobject add 100010` are both clickable and both go through
    // Create(..., GO_STATE_READY) -- SummonGameObject and the add command each
    // hardcode it. This was the only object in the system created ACTIVE, and
    // the only one that could not be clicked. The templates are otherwise
    // identical, down to every data field.
    //
    // The state still ends up ACTIVE: the AI flips it on its first tick, which
    // is also the only moment IsInWorld() is true and UpdateCollisionState will
    // actually run. Created READY, flipped after -- exactly what the handle
    // does, now for a reason rather than by luck of ordering.
    data.go_state          = GO_STATE_READY;
    data.spawn_flags       = 0;
    data.visibility_mod    = 0.0f;
    data.instanciatedContinentInstanceId = 0;

    // sObjectMgr set, NOT a MapPersistentState one: an entrance belongs to the
    // world and loads with the world grids for everybody. That is the single
    // line of difference from furniture, and it is the whole distinction
    // between an object that is yours and an object that is everyone else too.
    sObjectMgr.AddGameobjectToGrid(guid, &data);

    // If the grid is already up -- which it is whenever somebody is standing
    // there to watch it appear -- spawn it now rather than on the next load.
    Map* map = sMapMgr.FindMap(p.map);
    if (!map || !map->IsLoaded(p.x, p.y))
        return;

    GameObject* go = new GameObject;
    if (!go->LoadFromDB(guid, map))
    {
        sLog.outError("Housing: could not spawn entrance portal %u.", p.id);
        delete go;
        return;
    }
    map->Add(go);
}

void HouseMgr::DespawnPortalObject(HousePortal const& p)
{
    uint32 const guid = HOUSE_PORTAL_GUID_MIN + p.id;

    GameObjectData const* data = sObjectMgr.GetGOData(guid);
    if (!data)
        return;

    if (Map* map = sMapMgr.FindMap(p.map))
        if (GameObject* go = map->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, data->id, guid)))
            go->AddObjectToRemoveList();

    sObjectMgr.RemoveGameobjectFromGrid(guid, data);
    sObjectMgr.DeleteGOData(guid);
}

// The house-side portal. A temporary summon rather than a spawn, so it costs no
// guid block and simply comes back the next time the instance loads.
// The visit ending. See the header for why this is a poll and not a hook.
void HouseMgr::EvictLapsedVisitors(Map* map)
{
    if (!m_enabled || !map)
        return;

    House* house = GetHouseByInstance(map->GetInstanceId());
    if (!house)
        return;

    // COLLECTED FIRST, ACTED ON AFTER. TeleportTo removes the player from this
    // map, which mutates the very list being walked -- the same reason
    // DeleteTemplate builds its sweep from sObjectAccessor rather than from the
    // map's own references.
    std::vector<Player*> leaving;

    Map::PlayerList const& players = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* p = itr->getSource();
        if (!p || !p->GetSession())
            continue;

        // The owner is not a visitor, and their own alts are the same account.
        if (p->GetSession()->GetAccountId() == house->accountId)
            continue;

        // Only somebody riding a forced instance INTO THIS HOUSE. A player who
        // is here on a bind of their own is not a visitor at all, and a stale
        // override for a different house is somebody else's business.
        if (p->GetForcedInstanceId(HOUSE_MAP_ID) != house->instanceId)
            continue;

        if (p->GetSession()->GetSecurity() >= SEC_DEVELOPER)
            continue;

        // THE SAME QUESTION THE DOOR ASKS, asked again. Not "were they let in"
        // -- nothing recorded that -- but "would they be let in NOW", which is
        // the only form that cannot go stale.
        HousePartyHost host;
        if (GetPartyLeader(p, host) && host.leaderAccount == house->accountId)
            continue;

        leaving.push_back(p);
    }

    for (Player* p : leaving)
    {
        // COMBAT HOLDS THE EVICTION, IT DOES NOT CANCEL IT. This poll comes
        // back every HOUSE_EDIT_INTERVAL, so a refusal announced here would be
        // announced twice a second; saying nothing and moving them the moment
        // they are free is the same outcome a moment later. SendPlayerToPortal
        // refuses in combat too -- this guard is about the message, not the
        // travel.
        if (p->IsInCombat())
            continue;

        ChatHandler(p).SendSysMessage("The party has broken up, and the visit with it.");

        // OUT THROUGH THE DOOR, NOT TO A HEARTHSTONE. Being shown out of a
        // house should put you outside it, which is where the exit portal's
        // own Leave puts you -- and using that function rather than a second
        // teleport is what keeps the two ways a visit can end from drifting.
        // Its ladder is better at every rung: the door they walked in by
        // (RememberEntrance recorded it), then their own front door, then the
        // hearthstone only when there is nowhere else at all.
        //
        // CLEARED AFTER, NOT BEFORE. Clearing the forced instance ahead of a
        // teleport that did not happen would leave them standing inside with
        // the GetForcedInstanceId check above skipping them for good -- an
        // ex-guest the poll can no longer see.
        std::string ignored;
        SendPlayerToPortal(p, ignored);
        p->ClearForcedInstance();
    }
}

void HouseMgr::UpdateExitPortal(Map* map)
{
    if (!m_enabled || !map)
        return;

    // Only in an instance that is actually somebody's house. Any other copy of
    // map is not ours to furnish -- same check OnHouseMapCreated makes.
    House* house = GetHouseByInstance(map->GetInstanceId());
    if (!house)
        return;

    ObjectGuid& known = m_exitPortals[map->GetInstanceId()];
    if (!known.IsEmpty() && map->GetGameObject(known))
        return;                                 // still standing

    // respawnTime 0 is not "despawn immediately": GameObject::Update gates the
    // whole despawn branch on m_respawnTime > 0, so a zero here means the timer
    // is off and the portal stands until the instance unloads. The edit handles
    // pass a real lifetime because they SHOULD expire; this one should not.

    float x, y, z, o;
    GetExitPosition(*house, x, y, z, o);

    GameObject* go = map->SummonGameObject(HOUSE_EXIT_ENTRY, x, y, z, o,
                                           0.0f, 0.0f, 0.0f, 0.0f,
                                           0, WORLD_DEFAULT_OBJECT);
    if (!go)
        return;

    go->SetGoState(GO_STATE_ACTIVE);            // collision off -- see SpawnPortal
    known = go->GetObjectGuid();
}

// The single answer to "where does this house's way out stand". Both the summon
// above and `.house template exit where` read it, so they cannot drift apart.
void HouseMgr::GetExitPosition(House const& house, float& x, float& y, float& z, float& o) const
{
    if (house.exitSet)
    {
        x = house.exitX;
        y = house.exitY;
        z = house.exitZ;
        o = house.exitO;
        return;
    }

    // The original constant, still the answer for every house no template ever
    // placed one in: behind the arrival point, so you land facing away from it
    // and have to turn round to leave -- which is also what stops arriving
    // re-triggering it. A house created before exits existed, and any house
    // claimed from a template that never set one, behaves exactly as before.
    x = HOUSE_ENTRY_X - cos(HOUSE_ENTRY_O) * HOUSE_EXIT_OFFSET;
    y = HOUSE_ENTRY_Y - sin(HOUSE_ENTRY_O) * HOUSE_EXIT_OFFSET;
    z = HOUSE_ENTRY_Z;
    o = HOUSE_ENTRY_O;
}

// Standing HOUSE_EXIT_OFFSET back along the portal's own facing puts you exactly
// where whoever placed it was standing, looking away from it into the room --
// and far enough back that arriving never trips HOUSE_PORTAL_RANGE (2.0) and
// bounces you straight out again.
//
// With no exit chosen this returns HOUSE_ENTRY_* to the yard and the degree,
// because GetExitPosition's fallback is that point stepped back by the same
// offset. Houses predating exits land exactly where they always did.
void HouseMgr::GetArrivalPosition(House const& house, float& x, float& y, float& z, float& o) const
{
    float ex, ey, ez, eo;
    GetExitPosition(house, ex, ey, ez, eo);

    x = ex + cos(eo) * HOUSE_EXIT_OFFSET;
    y = ey + sin(eo) * HOUSE_EXIT_OFFSET;
    z = ez + HOUSE_ARRIVE_LIFT;                 // above the floor -- see the constant
    o = eo;                                     // looking into the room, not at the portal
}

// UpdateExitPortal only summons when nothing is standing, so moving the way out
// means taking the old one down first. Forgetting the guid alone is not enough:
// that leaves the old portal standing and puts a second one at the new spot,
// which is exactly the bug SetPortalLook had to fix for the same reason.
void HouseMgr::DespawnExitPortal(Map* map)
{
    if (!map)
        return;

    auto itr = m_exitPortals.find(map->GetInstanceId());
    if (itr == m_exitPortals.end())
        return;

    if (GameObject* go = map->GetGameObject(itr->second))
    {
        // Not SetLootState(GO_JUST_DEACTIVATED) -- that is the goober path and
        // would drag the respawn machinery in on the way out.
        go->SetRespawnTime(0);
        go->Delete();
    }
    m_exitPortals.erase(itr);
}

bool HouseMgr::SetExitPortal(Player* player, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }

    House* house = GetAuthoredTemplateAt(player, error);
    if (!house)
        return false;

    // Ahead of the author and facing back, exactly as SetPortal does it out in
    // the world. Both ends of the trip are placed the same way round, so
    // "stand where you want to walk from, and walk forward" is the one rule.
    float const face = player->GetOrientation();
    house->exitX   = player->GetPositionX() + cos(face) * HOUSE_PORTAL_PLACE_DISTANCE;
    house->exitY   = player->GetPositionY() + sin(face) * HOUSE_PORTAL_PLACE_DISTANCE;
    house->exitZ   = player->GetPositionZ();
    house->exitO   = face + M_PI_F;
    house->exitSet = true;

    // PExecute is fine here, unlike CreateHouse: nothing reads this row back.
    CharacterDatabase.PExecute(
        "UPDATE house SET exit_set = 1, exit_x = %f, exit_y = %f, exit_z = %f, exit_o = %f WHERE id = %u",
        house->exitX, house->exitY, house->exitZ, house->exitO, house->id);

    DespawnExitPortal(player->GetMap());        // the next map tick re-summons it

    // Placing it three yards ahead would otherwise put the placer through it on
    // the very next tick. Same guard, same reason, as SetPortal.
    m_portalCooldown[player->GetGUIDLow()] = time(nullptr) + PORTAL_COOLDOWN_SEC;
    return true;
}

bool HouseMgr::ClearExitPortal(Player* player, std::string& error)
{
    House* house = GetAuthoredTemplateAt(player, error);
    if (!house)
        return false;

    if (!house->exitSet)
    {
        error = "This template's way out is already where it started.";
        return false;
    }

    house->exitSet = false;
    house->exitX = house->exitY = house->exitZ = house->exitO = 0.0f;

    CharacterDatabase.PExecute(
        "UPDATE house SET exit_set = 0, exit_x = 0, exit_y = 0, exit_z = 0, exit_o = 0 WHERE id = %u",
        house->id);

    DespawnExitPortal(player->GetMap());
    return true;
}

//== walking through it ======================================================

// Called from both portal scripts. `leaving` is the only difference between the
// two directions, which is why one function serves both.
void HouseMgr::CheckPortal(GameObject* portal, bool leaving)
{
    if (!m_enabled || !portal)
        return;

    time_t const now = time(nullptr);
    uint32 const nowMs = WorldTimer::getMSTime();

    std::list<Player*> players;
    MaNGOS::AnyPlayerInObjectRangeCheck check(portal, HOUSE_PORTAL_RANGE);
    MaNGOS::PlayerListSearcher<MaNGOS::AnyPlayerInObjectRangeCheck> searcher(players, check);
    Cell::VisitWorldObjects(portal, searcher, HOUSE_PORTAL_RANGE);

    for (Player* player : players)
    {
        if (!player->IsInWorld() || player->IsTaxiFlying() || player->IsBeingTeleported())
            continue;

        // Random bots are Player objects like any other, and 300 of them roam
        // the continent the entrance stands on. One clipping the ring would be
        // given a house, an instance and a PERMANENT bind -- so the swirl would
        // slowly collect the bot population. A bot session has no socket, so
        // WorldSession.cpp:103 addresses it "<BOT>"; this is the same test
        // CharacterHandler.cpp:945 makes, and it needs no playerbot header,
        // which is the point -- src/game includes none.
        std::string const& addr = player->GetSession()->GetRemoteAddress();
        if (addr == "<BOT>" || addr == "disconnected/bot")
            continue;

        // ASKED PER PLAYER, INSIDE THE LOOP. One person having walk-through
        // off must not stop the person beside them being taken through, and
        // the poll is shared -- so the test belongs here and not around the
        // search. It stays after the bot check and before the cooldown for no
        // reason but cost: the cheap disqualifiers go first.
        if (!GetSetting(player, HOUSE_SETTING_WALK_IN))
            continue;

        // ONE KNOCK PER STEP INTO THE RING. After a knock here, standing in
        // the doorway does nothing, silently, until they walk out and back.
        // Checked BEFORE the cooldown, because it is what keeps the touch
        // alive: it has to see them on every poll, not one in twenty.
        //
        // A touch not seen for HOUSE_KNOCK_TOUCH_MS is a step that ended. That
        // is judged here as well as in the sweep below, because the sweep only
        // runs while this portal's grid is loaded, and a touch left behind by
        // an unloaded grid must not mute the next real step in.
        auto touch = m_knockTouch.find(player->GetGUIDLow());
        if (touch != m_knockTouch.end() && touch->second.portalGuid == portal->GetGUIDLow())
        {
            if (WorldTimer::getMSTimeDiff(touch->second.lastSeen, nowMs) <= HOUSE_KNOCK_TOUCH_MS)
            {
                touch->second.lastSeen = nowMs;
                continue;
            }
            m_knockTouch.erase(touch);
        }

        auto itr = m_portalCooldown.find(player->GetGUIDLow());
        if (itr != m_portalCooldown.end() && itr->second > now)
            continue;

        if (leaving)
        {
            std::string error;
            if (!SendPlayerToPortal(player, error) && !error.empty())
                ChatHandler(player).SendSysMessage(error.c_str());
        }
        else
        {
            // STRICT: only your home door, or your party leader's, carries you
            // in. Once a door is a specific inside, materialising in a
            // different one is incoherent -- and with a destructive move offer
            // standing on every foreign gear, the collision-triggered path has
            // to be inert there. "Go home" on any gear keeps the old
            // convenience one click away. The hints ride the same cooldown as a
            // real trip, so standing in the ring does not print four lines a
            // second.
            uint32 const portalId = PortalIdFromGuid(portal->GetGUIDLow());
            House const* house = GetHouseByAccount(player->GetSession()->GetAccountId());

            // THE PARTY IS CHECKED FIRST, and that ordering is the whole point:
            // a party walking through one door has to end up in ONE house.
            // Deciding your own home first would split a group at the exact
            // moment it was trying to arrive together, whenever a member
            // happened to live behind the same door. Nobody is taken anywhere
            // silently -- the arrival says whose house it is, and the gear
            // still has "Go home" on it.
            HousePartyHost host;
            if (GetPartyLeaderHouse(player, portalId, host))
            {
                // A LOCKED DOOR MARKS THE STEP, whatever the knock answers:
                // the knock itself, "wait", and "nobody is home" are all said
                // once per step in, not once per cooldown.
                House const* leaderHouse = GetHouseById(host.houseId);
                if (leaderHouse && IsLocked(*leaderHouse))
                {
                    HouseKnockTouch& t = m_knockTouch[player->GetGUIDLow()];
                    t.portalGuid = portal->GetGUIDLow();
                    t.lastSeen   = nowMs;
                }

                std::string error;
                if (VisitPartyLeaderHouse(player, ObjectGuid(), portalId, error))
                    ChatHandler(player).PSendSysMessage("You follow your party into %s's home.", host.leaderName.c_str());
                else if (!error.empty())
                    ChatHandler(player).SendSysMessage(error.c_str());
            }
            else if (house && IsHomePortal(*house, portalId))
            {
                // Remember WHICH door was walked through, so the way out
                // returns them to it. The id is recoverable from the guid
                // precisely because the guid was derived from it.
                RememberEntrance(player, portalId);

                std::string error;
                if (!SendPlayerHome(player, error) && !error.empty())
                    ChatHandler(player).SendSysMessage(error.c_str());
            }
            else if (house)
                ChatHandler(player).SendSysMessage("This is not your door. Right-click the gear to go home, or to move here.");
            else
                ChatHandler(player).SendSysMessage("This home is available. Right-click the gear to claim it.");
        }

        m_portalCooldown[player->GetGUIDLow()] = now + PORTAL_COOLDOWN_SEC;
    }

    // Bounded by "players who touched a portal in the last five seconds", but it
    // is a map that only ever grows unless somebody sweeps it.
    for (auto itr = m_portalCooldown.begin(); itr != m_portalCooldown.end();)
        itr = (itr->second <= now) ? m_portalCooldown.erase(itr) : ++itr;

    // A step that ended: nobody by that guid was seen in this ring lately.
    // Only this portal's touches, since only this portal's poll sees them.
    for (auto itr = m_knockTouch.begin(); itr != m_knockTouch.end();)
        itr = (itr->second.portalGuid == portal->GetGUIDLow() &&
               WorldTimer::getMSTimeDiff(itr->second.lastSeen, nowMs) > HOUSE_KNOCK_TOUCH_MS)
            ? m_knockTouch.erase(itr) : ++itr;
}

bool HouseMgr::IsHomePortal(House const& house, HousePortal const& portal) const
{
    if (house.portalId && house.portalId == portal.id)
        return true;

    // A TEMPLATE MATCH IS A HOME MATCH. Both sides have to HAVE a template:
    // template 0 means "untemplated" on a door and "grandfathered" on a house,
    // and neither is a thing to match on -- 0 == 0 would put every old house at
    // home behind every plain door in the world.
    return house.templateId && portal.templateId == house.templateId;
}

bool HouseMgr::IsHomePortal(House const& house, uint32 portalId) const
{
    HousePortal const* p = GetPortal(portalId);
    return p && IsHomePortal(house, *p);
}

HousePortal const* HouseMgr::GetHomePortal(House const& house) const
{
    // The recorded door first: it is the one somebody actually claimed or moved
    // at, and the only one when the house has no template.
    if (HousePortal const* p = GetPortal(house.portalId))
        if (IsHomePortal(house, *p))
            return p;

    if (!house.templateId)
        return nullptr;

    for (std::map<uint32, HousePortal>::const_iterator itr = m_portals.begin();
         itr != m_portals.end(); ++itr)
        if (itr->second.templateId == house.templateId)
            return &itr->second;

    return nullptr;
}

bool HouseMgr::SendPlayerToPortal(Player* player, std::string& error)
{
    if (!player)
        return false;

    if (player->IsInCombat())
    {
        error = "You cannot leave while in combat.";
        return false;
    }

    // The door they actually walked in through comes first. Falling back to the
    // one they call home covers a relog or a restart while indoors, and covers
    // .house go, which arrives through no door at all.
    HousePortal const* p = nullptr;

    std::map<uint32, uint32>::const_iterator used = m_enteredBy.find(player->GetGUIDLow());
    if (used != m_enteredBy.end())
        p = GetPortal(used->second);

    // Any door this house is at home behind, not only the recorded id -- which
    // is nothing at all once the door it named has been removed and re-made.
    if (!p)
        if (House const* house = GetHouseByAccount(player->GetSession()->GetAccountId()))
            p = GetHomePortal(*house);

    if (!p)
    {
        // Nowhere specific to go back to. The hearthstone answer beats refusing
        // to let somebody out of their own house.
        player->TeleportToHomebind();
        m_enteredBy.erase(player->GetGUIDLow());
        return true;
    }

    // In front of the portal rather than inside it, or you arrive standing in
    // the ring and are sent straight back. HOUSE_PORTAL_PLACE_DISTANCE lands
    // you exactly where whoever placed the door was standing.
    float const x = p->x + cos(p->o) * HOUSE_PORTAL_PLACE_DISTANCE;
    float const y = p->y + sin(p->o) * HOUSE_PORTAL_PLACE_DISTANCE;

    // FACING p->o, not p->o + PI: the portal's own orientation points back down
    // the way its author approached, so this is looking away from the swirl and
    // out into the world. The + PI form put you nose to the door you had just
    // come out of, which is the one direction you have finished with.
    //
    // Same rule as GetArrivalPosition uses on the inside -- step out along the
    // portal's facing, and keep facing that way. The two ends of the trip were
    // written months apart and only the inside one said so.
    if (!player->TeleportTo(p->map, x, y, p->z, p->o))
    {
        error = "Could not send you back.";
        return false;
    }

    // ARMED HERE, NOT ONLY IN THE POLL. Walking out already lands you
    // HOUSE_PORTAL_PLACE_DISTANCE from the door, a yard outside the ring, so
    // this is belt to that braces -- but CheckPortal armed it and the gear's
    // Leave did not, and two ways out of a house that differ by a cooldown is
    // the kind of asymmetry nobody notices until one of them loops.
    m_portalCooldown[player->GetGUIDLow()] = time(nullptr) + PORTAL_COOLDOWN_SEC;

    m_enteredBy.erase(player->GetGUIDLow());
    return true;
}

// The guid carries the id, so a clicked or walked-into portal can say which one
// it is without a lookup. Zero means "not one of ours".
uint32 HouseMgr::PortalIdFromGuid(uint32 lowGuid)
{
    if (lowGuid > HOUSE_PORTAL_GUID_MIN && lowGuid <= HOUSE_PORTAL_GUID_MAX)
        return lowGuid - HOUSE_PORTAL_GUID_MIN;
    return 0;
}

void HouseMgr::RememberEntrance(Player* player, uint32 portalId)
{
    if (player && portalId)
        m_enteredBy[player->GetGUIDLow()] = portalId;
}

//== the look, live ==========================================================

// Same reasoning as SetHandleLook, and the same trap behind it:
// gameobject_template has NO reload command, so changing the portal's model in
// SQL costs a full restart every time. Picking a portal model is exactly the
// job that wants twenty tries.
//
// Deliberately not persisted. The answer it finds belongs in sql/custom/027 as
// a template change, not in memory; losing it on restart is the feature.
void HouseMgr::SetPortalLook(uint32 displayId, float scale)
{
    uint32 const entries[2] = { HOUSE_PORTAL_ENTRY, HOUSE_EXIT_ENTRY };
    for (uint32 entry : entries)
    {
        GameObjectInfo* info = const_cast<GameObjectInfo*>(sObjectMgr.GetGameObjectInfo(entry));
        if (!info)
            continue;

        // Remember the rows as loaded the first time through, so reset has
        // something to go back to. Both templates ship identical, so one pair of
        // saved values covers them.
        if (!m_portalTemplateDisplay)
        {
            m_portalTemplateDisplay = info->displayId;
            m_portalTemplateSize = info->size;
        }

        info->displayId = displayId ? displayId : m_portalTemplateDisplay;
        info->size = scale > 0.0f ? scale : m_portalTemplateSize;
    }

    m_portalDisplay = displayId;
    m_portalScale = scale;

    // A display change only reaches the client on a fresh spawn -- the 1.12
    // client builds a gameobject from its creation block and ignores a later
    // GAMEOBJECT_DISPLAYID update, the same trap SetSelectMarkLook works around. So
    // put the entrance through a despawn/respawn, and forget the exit portals so
    // the next map tick re-summons them.
    for (std::map<uint32, HousePortal>::iterator itr = m_portals.begin(); itr != m_portals.end(); ++itr)
    {
        DespawnPortalObject(itr->second);
        SpawnPortalObject(itr->second);
    }

    // The house-side ones have to be DESPAWNED, not just forgotten. Clearing the
    // map alone loses the handle on objects that are still standing, and the
    // next tick summons a second portal beside each of them.
    for (auto const& itr : m_exitPortals)
    {
        Map* map = sMapMgr.FindMap(HOUSE_MAP_ID, itr.first);
        if (!map)
            continue;
        if (GameObject* go = map->GetGameObject(itr.second))
        {
            // Not SetLootState(GO_JUST_DEACTIVATED) -- that is the goober path
            // and would drag the respawn machinery in on the way out.
            go->SetRespawnTime(0);
            go->Delete();
        }
    }
    m_exitPortals.clear();
}

//== the gear in the middle of it =============================================

// The swirl cannot be moused over. A probe on OnUse -- the earliest
// server-side point a click can reach -- logged nothing at all, while an
// interact-keybind addon driving the same object worked perfectly. The server
// side was never the problem; the art is.
//
// THE OBSERVATION STANDS AND THE OLD EXPLANATION DOES NOT. This said
// Creature_Spellportal_Purple.m2 "carries no hit volume", which was reasoning
// rather than measurement, and it is contradicted by the server's own
// extractor: display 21585 IS one of the 6,604 ids in
// vmaps/temp_gameobject_models, with a healthy 0.42 x 1.81 x 2.36 yard box --
// BIGGER than the gear that works, which is a 3.78 model at 0.15 scale.
// Checked 2026-09-03, along with the two other differences that could have
// explained it and do not: both templates are type 10 GOOBER with flags 0 and
// faction 0, both are born GO_STATE_READY, and both are set GO_STATE_ACTIVE
// afterwards (which only disables the SERVER's vmap model anyway). The only
// field left that differs is displayId.
//
// So what is untrue is "this model has nothing to hit", and what is true is
// "this model is not pickable by the client", which is a different claim and
// one no file on disk answers -- effect models generally are not. THE LEVER IS
// THE DISPLAY ID, and `.house entrance model <id> [scale]` tries one live
// without saving it. If a portal-ish model with real geometry rather than
// particles takes a click, this gear can go.
//
// So the menu hangs on a small gear standing in the portal, which is the same
// answer the edit handles already are. It is a TEMPORARY summon for the same
// reason they are: Map::SummonGameObject hardcodes Create(..., GO_STATE_READY),
// which is the state a clickable goober has to be born in, and it costs no guid
// block. The portal AI keeps it standing, so a grid unload and reload simply
// grows a new one.
// IS ANYBODY CLOSE ENOUGH TO BE OFFERED THE GEAR?
//
// BOTS DO NOT COUNT, and out in the world that is not a nicety. The entrances
// stand in Ironforge and 300 random bots roam the continent around them, so a
// bot walking past would raise the gear for everybody and drop it again a
// second later, all day. Same test CheckPortal makes and for the same reason a
// bot is not a person here -- a session with no socket, which
// WorldSession.cpp:103 addresses as "<BOT>".
static bool AnyoneNear(GameObject* at, float range)
{
    if (!at)
        return false;

    std::list<Player*> players;
    MaNGOS::AnyPlayerInObjectRangeCheck check(at, range);
    MaNGOS::PlayerListSearcher<MaNGOS::AnyPlayerInObjectRangeCheck> searcher(players, check);
    Cell::VisitWorldObjects(at, searcher, range);

    for (Player* player : players)
    {
        if (!player->IsInWorld() || player->IsTaxiFlying() || player->IsBeingTeleported())
            continue;

        std::string const& addr = player->GetSession()->GetRemoteAddress();
        if (addr == "<BOT>" || addr == "disconnected/bot")
            continue;

        return true;
    }
    return false;
}

// The same question inside a house, asked a different way ON PURPOSE. There is
// no anchor object to search around -- the exit gear is placed from a stored
// position, and the exit portal beside it may not be standing yet -- and an
// instance holds a handful of players against a continent's thousands, so
// walking the map's own list is both simpler and cheaper than a cell visit.
// Two shapes because the two situations are genuinely different, not because
// one of them was written twice.
static bool AnyoneNear(Map* map, float x, float y, float z, float range)
{
    if (!map)
        return false;

    Map::PlayerList const& list = map->GetPlayers();
    for (Map::PlayerList::const_iterator itr = list.begin(); itr != list.end(); ++itr)
    {
        Player* player = itr->getSource();
        if (!player || !player->IsInWorld() || player->IsBeingTeleported())
            continue;

        if (player->IsWithinDist3d(x, y, z, range))
            return true;
    }
    return false;
}

void HouseMgr::UpdatePortalMarker(GameObject* portal)
{
    if (!m_enabled || !portal)
        return;

    uint32 const id = PortalIdFromGuid(portal->GetGUIDLow());
    if (!id)
        return;

    Map* map = portal->GetMap();
    if (!map)
        return;

    // WHICH RANGE TO ASK ABOUT DEPENDS ON WHAT IS ALREADY THERE, and that is the
    // whole of the hysteresis: a standing gear is measured against the further
    // number, so you have to walk away from it rather than merely to the edge
    // of where it appeared. No extra state to keep -- whether it is standing is
    // already the answer, and it is already being looked up.
    ObjectGuid& known = m_portalMarkers[id];
    bool const standing = !known.IsEmpty() && map->GetGameObject(known);

    if (!AnyoneNear(portal, standing ? m_markHide : m_markShow))
    {
        // Nobody near. Take it down if it is up, and otherwise do nothing at
        // all -- which is the common case, once per portal per tick.
        if (standing)
            DespawnPortalMarker(id, portal->GetMapId());
        return;
    }

    if (standing)
        return;                                 // still standing, still wanted

    GameObject* go = map->SummonGameObject(HOUSE_PORTAL_MARK_ENTRY,
                                           portal->GetPositionX() + m_markOffX,
                                           portal->GetPositionY() + m_markOffY,
                                           portal->GetPositionZ() + m_markOffZ,
                                           portal->GetOrientation(),
                                           0.0f, 0.0f, 0.0f, 0.0f,
                                           0, WORLD_DEFAULT_OBJECT);
    if (!go)
        return;

    // Collision off so the gear does not become a doorstop in the middle of the
    // doorway. Safe AFTER the summon and only there: UpdateCollisionState needs
    // IsInWorld(). It does not cost the cursor -- the edit handles do exactly
    // this and stay clickable, because only the CREATION state matters.
    go->SetGoState(GO_STATE_ACTIVE);

    known = go->GetObjectGuid();
}

float HouseMgr::GetPortalMarkScale() const
{
    GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(HOUSE_PORTAL_MARK_ENTRY);
    return info ? info->size : 0.0f;
}

// SCALE RIDES ON THE TEMPLATE, because GameObject::Create reads goinfo->size
// into the object scale at creation. There is no resizing a standing gear: it
// has to be taken down and grown again, which the portal AI does within a tick.
void HouseMgr::SetPortalMarkScale(float scale)
{
    GameObjectInfo* info = const_cast<GameObjectInfo*>(sObjectMgr.GetGameObjectInfo(HOUSE_PORTAL_MARK_ENTRY));
    if (info)
    {
        // Remember the row as loaded the first time through, so reset has
        // something to go back to.
        if (m_markTemplateSize <= 0.0f)
            m_markTemplateSize = info->size;

        info->size = scale > 0.0f ? scale : m_markTemplateSize;
    }

    DespawnPortalMarkers();
}

// Position is a plain offset rather than a snap, for the same reason .house move
// is: there is no flight in this core, so you cannot stand where you want the
// gear and let it come to you.
void HouseMgr::SetPortalMarkOffset(float x, float y, float z)
{
    m_markOffX = x;
    m_markOffY = y;
    m_markOffZ = z;

    DespawnPortalMarkers();
}

// One gear, by portal id. The caller passes the map because the two callers
// that matter -- MovePortal and RemovePortal -- are about to make the row a
// liar or delete it outright, so it cannot be looked up from here.
void HouseMgr::DespawnPortalMarker(uint32 portalId, uint32 mapId)
{
    std::map<uint32, ObjectGuid>::iterator itr = m_portalMarkers.find(portalId);
    if (itr == m_portalMarkers.end())
        return;

    if (Map* map = sMapMgr.FindMap(mapId))
        if (GameObject* go = map->GetGameObject(itr->second))
        {
            // Not SetLootState(GO_JUST_DEACTIVATED) -- that is the goober path
            // and would drag the respawn machinery in on the way out.
            go->SetRespawnTime(0);
            go->Delete();
        }

    // Forgotten even if the map was not loaded: the guid is a temporary summon
    // that went with the grid, and holding a stale one only stops the AI ever
    // growing a replacement.
    m_portalMarkers.erase(itr);
}

// Take them all down and let the portal AI grow them again. Forgetting the guids
// alone is not enough -- that leaves the old gears standing and puts a second
// one beside each of them, which is the bug SetPortalLook had to fix first.
void HouseMgr::DespawnPortalMarkers()
{
    // Ids first: the single teardown erases as it goes, so iterating the map
    // while calling it would walk an invalidated iterator.
    std::vector<uint32> ids;
    for (std::map<uint32, ObjectGuid>::const_iterator itr = m_portalMarkers.begin();
         itr != m_portalMarkers.end(); ++itr)
        ids.push_back(itr->first);

    for (size_t i = 0; i < ids.size(); ++i)
        if (HousePortal const* p = GetPortal(ids[i]))
            DespawnPortalMarker(ids[i], p->map);

    // Belt and braces for a marker whose portal is already gone: there is no
    // map to reach it on, and keeping the entry would only stop a replacement.
    // Nothing should reach this any more -- RemovePortal takes its own gear
    // down now -- but leaking a clickable object is the worse failure.
    m_portalMarkers.clear();
}

// The gear beside the way out, carrying the house's own control panel.
//
// An exact analogue of UpdatePortalMarker, and for the same reasons: a
// temporary summon costs no guid block, comes back whenever the instance does,
// and needs no despawn bookkeeping when the house empties. The difference is
// only what it is keyed by -- one gear per INSTANCE here, because there is one
// exit per house, against one per portal id out in the world.
//
// The exit itself is left alone. Its AI still only checks for somebody walking
// into it; this rides the instance script's throttle beside UpdateExitPortal,
// which is where the exit is summoned in the first place.
// HIDE STRICTLY BEYOND SHOW, enforced here rather than trusted from the
// caller. Equal numbers, or inverted ones, make a gear that is summoned and
// despawned on alternate ticks -- a fresh creation block to every client in
// range, four times a second -- which is the exact failure the two-number
// design exists to prevent. A command that can type them in is a command that
// can type them in wrong.
void HouseMgr::SetMarkRange(float show, float hide)
{
    if (show < 0.5f)
        show = 0.5f;
    if (hide < show + 1.0f)
        hide = show + 1.0f;

    m_markShow = show;
    m_markHide = hide;
}

void HouseMgr::UpdateExitMarker(Map* map)
{
    if (!m_enabled || !map)
        return;

    House* house = GetHouseByInstance(map->GetInstanceId());
    if (!house)
        return;

    float x, y, z, o;
    GetExitPosition(*house, x, y, z, o);

    // Same come-and-go rule as the entrance gear, same hysteresis.
    //
    // MEASURED FROM THE GROUND, NOT FROM THE GEAR, and getting that wrong is
    // why this did nothing at all for one build. The gear floats
    // HOUSE_PORTAL_MARK_LIFT (2 yards) up, and the exit takes you out of the
    // house the moment you are within HOUSE_PORTAL_RANGE (2 yards) of it -- so
    // measuring to the gear costs two yards of the three straight up while the
    // portal itself refuses to let you spend the last two horizontally. What
    // is left is a band about a foot wide, sitting exactly where you get
    // teleported away: in practice, a gear that never appears.
    //
    // The entrance never had this because it searches around the PORTAL
    // object, which stands on the floor. Same rule now, written out because
    // "measure to the thing you are showing" is the obvious wrong answer.
    ObjectGuid& known = m_exitMarkers[map->GetInstanceId()];
    bool const standing = !known.IsEmpty() && map->GetGameObject(known);

    if (!AnyoneNear(map, x, y, z, standing ? m_markHide : m_markShow))
    {
        if (standing)
            DespawnExitMarker(map);
        return;
    }

    if (standing)
        return;                                 // still standing, still wanted

    GameObject* go = map->SummonGameObject(HOUSE_CONTROL_ENTRY,
                                           x + m_markOffX, y + m_markOffY, z + m_markOffZ, o,
                                           0.0f, 0.0f, 0.0f, 0.0f,
                                           0, WORLD_DEFAULT_OBJECT);
    if (!go)
        return;

    go->SetGoState(GO_STATE_ACTIVE);            // collision off -- see SpawnPortal
    known = go->GetObjectGuid();
}

// One instance's gear, taken down. The analogue of DespawnPortalMarker, and it
// forgets the guid whether or not the object was still there: it is a temporary
// summon, so a grid that unloaded took it already, and holding a stale guid
// would only stop UpdateExitMarker ever growing a replacement.
void HouseMgr::DespawnExitMarker(Map* map)
{
    if (!map)
        return;

    std::map<uint32, ObjectGuid>::iterator itr = m_exitMarkers.find(map->GetInstanceId());
    if (itr == m_exitMarkers.end())
        return;

    if (GameObject* go = map->GetGameObject(itr->second))
    {
        // Not SetLootState(GO_JUST_DEACTIVATED) -- that is the goober path and
        // would drag the respawn machinery in on the way out.
        go->SetRespawnTime(0);
        go->Delete();
    }

    m_exitMarkers.erase(itr);
}

// Same scan, same reasoning as GetPortalIdByMarker: a handful of houses are
// loaded at once, clicks are rare, and a reverse index would be a second thing
// to keep in step with the first.
House* HouseMgr::GetHouseByExitMarker(ObjectGuid marker)
{
    for (std::map<uint32, ObjectGuid>::const_iterator itr = m_exitMarkers.begin();
         itr != m_exitMarkers.end(); ++itr)
        if (itr->second == marker)
            return GetHouseByInstance(itr->first);
    return nullptr;
}

// Few portals, rare clicks, so a scan beats keeping a second index in step.
uint32 HouseMgr::GetPortalIdByMarker(ObjectGuid marker) const
{
    for (std::map<uint32, ObjectGuid>::const_iterator itr = m_portalMarkers.begin();
         itr != m_portalMarkers.end(); ++itr)
        if (itr->second == marker)
            return itr->first;
    return 0;
}

//== the scripts =============================================================

namespace
{
    // sql/custom/033 and 036 -- the unavoidable database rows, because
    // SendGossipMenu takes an npc_text id rather than a string. The OPTIONS
    // below are inline and need nothing. 6400031 greets; 6400032 is the
    // confirm page -- there are no popup boxes in this client's gossip, so a
    // confirm is a SECOND MENU with one yes and a way out.
    uint32 const HOUSE_PORTAL_TEXT         = 6400031;
    uint32 const HOUSE_PORTAL_CONFIRM_TEXT = 6400032;
    uint32 const HOUSE_PORTAL_SENDER       = 6402;

    enum PortalAction
    {
        ACT_PORTAL_ENTER     = 1,
        ACT_PORTAL_HOME      = 2,   // untemplated doors only: re-home, keep everything
        ACT_PORTAL_CLOSE     = 3,
        ACT_PORTAL_CLAIM     = 4,   // -> confirm page
        ACT_PORTAL_CLAIM_YES = 5,
        ACT_PORTAL_MOVE      = 6,   // -> confirm page
        ACT_PORTAL_MOVE_YES  = 7,
        ACT_PORTAL_PARTY     = 8,   // join the party leader inside, no guest row
        ACT_PORTAL_STORAGE   = 9,   // the furniture shelf, from your own doorstep
    };

    struct go_house_portal_ai : public GameObjectAI
    {
        go_house_portal_ai(GameObject* go, bool leaving)
            : GameObjectAI(go), m_timer(0), m_leaving(leaving), m_collisionFixed(false) {}

        // SERVER-SIDE collision off, and it has to happen HERE rather than at
        // spawn time. UpdateCollisionState begins
        // `if (!m_model || !IsInWorld()) return;` and GameObject::Create calls
        // SetGoState BEFORE the object is added to a map -- so setting go_state
        // in GameObjectData set the field and never touched the model.
        //
        // Note what this does NOT do: the client decides for itself whether a
        // character can walk through a model, from its own MPQ data. This only
        // affects vmap, line of sight and pathing. Walking INTO a portal works
        // because the bump puts you inside HOUSE_PORTAL_RANGE and the poll fires.
        void UpdateAI(uint32 diff) override
        {
            if (!m_collisionFixed)
            {
                me->SetGoState(GO_STATE_ACTIVE);
                m_collisionFixed = true;
            }

            if (m_timer > diff)
            {
                m_timer -= diff;
                return;
            }
            m_timer = HOUSE_PORTAL_INTERVAL;

            // Only the world-side entrances carry a gear. The way out is placed
            // in the template and is not the resident's to move, so there is
            // nothing for a menu on it to offer.
            if (!m_leaving)
                sHouseMgr.UpdatePortalMarker(me);

            sHouseMgr.CheckPortal(me, m_leaving);
        }

        uint32 m_timer;
        bool   m_leaving;
        bool   m_collisionFixed;
    };
}

// THE MENU LIVES ON THE GEAR, not on the portal -- see UpdatePortalMarker above
// for why. Three hooks have to line up and only one is obvious:
//
//   gameobject_template.type = 10 (GOOBER), in sql/custom/034. The client only
//   sends CMSG_GAMEOBJ_USE for types it thinks usable.
//
//   pGOHello, which is ScriptMgr::OnGameObjectUse at GameObject.cpp:1448 --
//   BEFORE the type switch. Returning true makes Use() return there, and that
//   is what stops the goober state machine (GO_FLAG_IN_USE -> GO_ACTIVATED ->
//   GO_JUST_DEACTIVATED -> delete) destroying the gear after one click.
//   pGOGossipHello is the matching-sounding hook and is WRONG: it fires inside
//   that machine.
//
//   pGOGossipSelect, the only route back from CMSG_GOSSIP_SELECT_OPTION for a
//   gameobject guid.
// THE GEAR IS THE PURCHASE UI. Four states, four menus -- and never more than
// four options, far under the 15-option cap:
//
//   no house                       Claim this home / Never mind
//   your home door                 Step through / Never mind
//   home elsewhere, templated      Go home / Move here (destructive, confirmed) / Never mind
//   home elsewhere, untemplated    Go home / Make this my home (keep everything) / Never mind
//
// The last row is the pre-template world preserved per door: an untemplated
// door re-homes without touching a single object, exactly as it always did.
//
// A FIFTH LINE RIDES ON TOP OF ALL FOUR, and only at the leader's own home
// door: "Join <leader> inside". It is additive rather than a state of its own
// because your own house does not stop existing when you join a party -- the
// question the gear answers is still "which of these two insides", and both
// answers have to stay on the page. It is drawn LAST so the options that were
// there before never move under the cursor.
static bool GossipHello_go_house_portal_mark(Player* player, GameObject* go)
{
    uint32 const id = sHouseMgr.GetPortalIdByMarker(go ? go->GetObjectGuid() : ObjectGuid());
    if (!id)
        return false;

    HousePortal const* p = sHouseMgr.GetPortal(id);
    House const* house = sHouseMgr.GetHouseByAccount(player->GetSession()->GetAccountId());
    bool const isHome = house && p && sHouseMgr.IsHomePortal(*house, *p);

    GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
    player->PlayerTalkClass->ClearMenus();

    if (!house)
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Claim this home", HOUSE_PORTAL_SENDER, ACT_PORTAL_CLAIM);
    else if (isHome)
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Step through", HOUSE_PORTAL_SENDER, ACT_PORTAL_ENTER);
    else
    {
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Go home", HOUSE_PORTAL_SENDER, ACT_PORTAL_ENTER);
        if (p && p->templateId)
            menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Move here (your furniture is packed into storage)", HOUSE_PORTAL_SENDER, ACT_PORTAL_MOVE);
        else
            menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Make this my home (keep everything)", HOUSE_PORTAL_SENDER, ACT_PORTAL_HOME);
    }

    // THE CUPBOARD IS REACHABLE FROM THE DOORSTEP, and only from your own.
    // Drawn on `isHome` rather than on merely owning a house, so a player
    // standing at somebody else's door is not offered their own shelf in a
    // place they cannot see it -- the row would work, and it would still read
    // as this door offering something it has nothing to do with.
    //
    // It is the SAME shelf as the one on the exit gear inside: house_storage is
    // keyed by account, so there is only ever one, and the doorway is simply
    // the other side of the same cupboard. That is what makes this a second
    // door onto one thing rather than a second thing.
    if (isHome)
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Furniture Storage", HOUSE_PORTAL_SENDER, ACT_PORTAL_STORAGE);

    // ONLY WHEN THE LEADER LIVES BEHIND THIS DOOR. GetPartyLeaderHouse is the
    // single place that decides so, shared with the walk-through, and it
    // already refuses when you are the leader yourself or when the leader is
    // another of your own characters -- both of which would draw an option
    // meaning "go to the house you were already being offered".
    //
    // The leader it names is REMEMBERED, not re-derived at click time: a
    // promotion between drawing this line and clicking it must not quietly
    // redirect the trip. VisitPartyLeaderHouse compares the two and refuses.
    HousePartyHost host;
    if (sHouseMgr.GetPartyLeaderHouse(player, id, host))
    {
        // A LOCKED DOOR SAYS SO BEFORE THE CLICK. Same action either way, since
        // VisitPartyLeaderHouse decides; only the promise on the row changes.
        House const* leaderHouse = sHouseMgr.GetHouseById(host.houseId);
        std::string const label = (leaderHouse && sHouseMgr.IsLocked(*leaderHouse))
                                ? "Knock on " + host.leaderName + "'s door"
                                : "Join " + host.leaderName + " inside";
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, label.c_str(), HOUSE_PORTAL_SENDER, ACT_PORTAL_PARTY);
        sHouseMgr.SetPartyOffer(player, host.leaderGuid);
    }
    else
        sHouseMgr.SetPartyOffer(player, ObjectGuid());

    menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Never mind", HOUSE_PORTAL_SENDER, ACT_PORTAL_CLOSE);

    // The greeting is a fixed npc_text, so WHICH door this is has to go to chat.
    if (p)
    {
        std::string tplNote;
        if (p->templateId)
            if (HouseTemplate const* tpl = sHouseMgr.GetTemplate(p->templateId))
                tplNote = "  (" + tpl->name + ")";
        ChatHandler(player).PSendSysMessage("%s%s%s",
                                            p->name.empty() ? "A way home." : p->name.c_str(),
                                            tplNote.c_str(),
                                            isHome ? "  This one is yours." : "");
    }

    player->PlayerTalkClass->SendGossipMenu(HOUSE_PORTAL_TEXT, go->GetObjectGuid());
    return true;
}

// The confirm pages. A second menu rather than a popup, because the popup does
// not exist: this build's gossip packet carries no box message.
static void SendPortalConfirm(Player* player, GameObject* go, bool moving)
{
    GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
    player->PlayerTalkClass->ClearMenus();

    if (moving)
    {
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Yes, move here", HOUSE_PORTAL_SENDER, ACT_PORTAL_MOVE_YES);

        // THE FORECAST GOES TO CHAT, NOT ONTO THE BUTTON. The label used to
        // carry the warning ("What I placed is lost."), which was the only
        // place to put it while the answer was the same for everybody. It is
        // counted per house now -- "9 pieces go to your furniture storage; 2
        // came from the catalogue and cannot be packed" -- and a sentence that
        // long on a gossip row eats the ~490-byte menu budget and cannot wrap.
        //
        // Sent while DRAWING the page rather than on the click, because the
        // whole point is to be read before the button is pressed.
        if (House* house = sHouseMgr.GetHouseByAccount(player->GetSession()->GetAccountId()))
        {
            std::string const forecast = sHouseMgr.StowForecast(*house);
            if (!forecast.empty())
                ChatHandler(player).SendSysMessage(forecast.c_str());
        }
    }
    else
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Yes, make my home here", HOUSE_PORTAL_SENDER, ACT_PORTAL_CLAIM_YES);
    menu.AddMenuItem(HOUSE_GOSSIP_ICON, "No, never mind", HOUSE_PORTAL_SENDER, ACT_PORTAL_CLOSE);

    player->PlayerTalkClass->SendGossipMenu(HOUSE_PORTAL_CONFIRM_TEXT, go->GetObjectGuid());
}

static bool GossipSelect_go_house_portal_mark(Player* player, GameObject* go, uint32 sender, uint32 action)
{
    if (sender != HOUSE_PORTAL_SENDER)
        return false;

    uint32 const id = sHouseMgr.GetPortalIdByMarker(go ? go->GetObjectGuid() : ObjectGuid());

    // Taken unconditionally, so one open menu can never leave a stale promise
    // behind for the next one: every option on the page passes through here,
    // "Never mind" included. The confirm pages carry no party option, so
    // clearing it on the way to one loses nothing.
    ObjectGuid const offeredLeader = sHouseMgr.TakePartyOffer(player);

    // The page actions re-open a menu, so the window only closes for the
    // terminal ones. ClaimHouseAt and MoveHouseTo re-validate everything the
    // menu believed -- a confirm page can be stale by the time it is clicked.
    std::string error;
    switch (action)
    {
        case ACT_PORTAL_CLAIM:
            SendPortalConfirm(player, go, false);
            return true;

        case ACT_PORTAL_MOVE:
            SendPortalConfirm(player, go, true);
            return true;

        case ACT_PORTAL_CLAIM_YES:
            player->PlayerTalkClass->CloseGossip();
            if (!sHouseMgr.ClaimHouseAt(player, id, error) && !error.empty())
                ChatHandler(player).SendSysMessage(error.c_str());
            return true;

        case ACT_PORTAL_MOVE_YES:
            player->PlayerTalkClass->CloseGossip();
            if (!sHouseMgr.MoveHouseTo(player, id, error) && !error.empty())
                ChatHandler(player).SendSysMessage(error.c_str());
            return true;

        // THE SAME PAIR THE EXIT GEAR SENDS, and deliberately the same two
        // calls rather than a shared "open storage" helper: the window for
        // anyone running the addon, the chat list for anyone not. The push is
        // harmless with nothing listening, and a menu row that silently does
        // nothing is the worse failure.
        //
        // No permission check here. StorageAllowed is the gate and it now
        // accepts the doorstep, so this row can only be drawn -- and can only
        // do anything -- where that gate already says yes.
        case ACT_PORTAL_STORAGE:
            player->PlayerTalkClass->CloseGossip();
            HouseOpenStorageWindow(player);
            HousePrintStorage(player);
            return true;

        case ACT_PORTAL_ENTER:
            // The same trip walking into it makes, recorded the same way, so the
            // two routes in cannot drift apart.
            player->PlayerTalkClass->CloseGossip();
            sHouseMgr.RememberEntrance(player, id);
            if (!sHouseMgr.SendPlayerHome(player, error) && !error.empty())
                ChatHandler(player).SendSysMessage(error.c_str());
            return true;

        case ACT_PORTAL_PARTY:
            // Every condition the menu drew this from is checked again inside,
            // against the leader it NAMED -- a group that disbanded, promoted
            // or dropped this player while the window sat open is refused with
            // a reason rather than honoured against whoever leads now.
            //
            // An EMPTY offer is refused outright rather than falling through to
            // "whoever leads now". Reaching here without one means the hello
            // that draws this line never ran for this click -- a restart in
            // between, or a select arriving on its own -- and the whole promise
            // of the guard is that this option goes where its label said.
            player->PlayerTalkClass->CloseGossip();
            if (offeredLeader.IsEmpty())
                ChatHandler(player).SendSysMessage("Right-click the door again.");
            else if (!sHouseMgr.VisitPartyLeaderHouse(player, offeredLeader, id, error) && !error.empty())
                ChatHandler(player).SendSysMessage(error.c_str());
            return true;

        case ACT_PORTAL_HOME:
            player->PlayerTalkClass->CloseGossip();
            if (!sHouseMgr.SetHomePortal(player, id, error))
                ChatHandler(player).SendSysMessage(error.c_str());
            else
                ChatHandler(player).SendSysMessage("This door is your home now. It is where you will be sent back to.");
            return true;

        default:
            player->PlayerTalkClass->CloseGossip();
            return true;
    }
}

GameObjectAI* GetAI_go_house_portal(GameObject* pGo) { return new go_house_portal_ai(pGo, false); }
GameObjectAI* GetAI_go_house_exit(GameObject* pGo)   { return new go_house_portal_ai(pGo, true); }

//== the house's own control panel ===========================================
//
// The gear beside the way out. Until this, the exit carried nothing, and the
// comment beside it said so: "Only the world-side entrances carry a gear. The
// way out is placed in the template and is not the resident's to move, so there
// is nothing for a menu on it to offer." That was true of MOVING the exit and
// stays true. What changed is that the house grew things worth controlling, and
// the exit is the one object every house has and every player walks up to.
//
// Built entirely in code, like the other two housing menus: no gossip_menu row
// out of the nearly-full smallint space, no option rows, no ~490-byte menu
// ceiling. The only unavoidable SQL is the greeting, because SendGossipMenu
// takes an npc_text id rather than a string.

namespace
{
    uint32 const HOUSE_CONTROL_TEXT          = 6400033;
    uint32 const HOUSE_CONTROL_CONFIRM_TEXT  = 6400034;
    uint32 const HOUSE_CONTROL_SETTINGS_TEXT = 6400038;
    uint32 const HOUSE_CONTROL_SENDER        = 6403;

    enum ControlAction
    {
        ACT_CTL_EDIT      = 1,
        ACT_CTL_LIST      = 2,
        ACT_CTL_STORAGE   = 3,
        ACT_CTL_BANK      = 4,
        ACT_CTL_EMPTY     = 5,   // -> confirm page
        ACT_CTL_EMPTY_YES = 6,
        ACT_CTL_HELP      = 7,
        ACT_CTL_CLOSE     = 8,
        ACT_CTL_SETTINGS  = 9,   // -> settings page
        ACT_CTL_BACK      = 10,  // -> main page
        ACT_CTL_LEAVE     = 11,  // out through the exit, without walking into it
        ACT_CTL_LOCK      = 12,  // the front door, locked or open

        // One action per setting, and they are numbered FROM a base rather than
        // listed, so adding a setting to the HouseSetting enum puts a row on
        // this page with no change here at all. The block is the last thing in
        // this enum for that reason.
        ACT_CTL_SETTING_0 = 20,
    };

    void SendControlMain(Player* player, GameObject* go)
    {
        GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
        player->PlayerTalkClass->ClearMenus();

        // THE LABEL CARRIES THE STATE on the edit-mode row below, so the menu
        // cannot describe a house it is not in. Same rule the furniture menu's
        // endings follow: a button that reads the same in both states is a
        // button you have to press to find out what it does.

        // FOUR GROUPS, BLANK ROWS BETWEEN THEM: the way out, then the things
        // that change how the house behaves, then the two stores, then the one
        // that undoes an afternoon. A gossip menu has no inert row, so a spacer
        // is a REAL option carrying a single space whose action redraws this
        // page -- the same trick the furniture menu uses. Clicking one costs a
        // repaint and nothing else, which is what makes it safe to put a blank
        // where a mis-click would otherwise land on something.
        //
        // Ordered so the destructive row is as far from the top as the layout
        // allows and sits alone, with Close beneath it as the thing your hand
        // is most likely to want next.

        // THE WAY OUT, AND IT IS DRAWN ALWAYS. Walking into the exit is the
        // usual way to leave and a player can switch that off, so without this
        // walk_through_portals would be a way to lock yourself in your own
        // house. Unconditional rather than "only while that setting is off": a
        // way out that appears only in the state you cannot leave from is one
        // conditional away from being no way out at all.
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Leave Home",           HOUSE_CONTROL_SENDER, ACT_CTL_LEAVE);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, " ",                    HOUSE_CONTROL_SENDER, ACT_CTL_BACK);

        menu.AddMenuItem(HOUSE_GOSSIP_ICON,
                         sHouseMgr.IsEditingAll(player) ? "Edit mode: on" : "Edit mode: off",
                         HOUSE_CONTROL_SENDER, ACT_CTL_EDIT);

        // THE LOCK IS ON THIS PAGE, not only under Settings, because it is
        // something you change while people are about rather than once. Drawn
        // only in your OWN house: the setting is per account, and a developer
        // at a template's gear would otherwise be toggling their own door from
        // somebody else's room.
        if (sHouseMgr.GetHouseByAccount(player->GetSession()->GetAccountId()) ==
            sHouseMgr.GetHouseByExitMarker(go->GetObjectGuid()))
            menu.AddMenuItem(HOUSE_GOSSIP_ICON,
                             sHouseMgr.GetSetting(player, HOUSE_SETTING_LOCKED) ? "Door: locked" : "Door: open",
                             HOUSE_CONTROL_SENDER, ACT_CTL_LOCK);

        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Settings",             HOUSE_CONTROL_SENDER, ACT_CTL_SETTINGS);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "House Manual",         HOUSE_CONTROL_SENDER, ACT_CTL_HELP);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, " ",                    HOUSE_CONTROL_SENDER, ACT_CTL_BACK);

        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Furniture Storage",    HOUSE_CONTROL_SENDER, ACT_CTL_STORAGE);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Bank",                 HOUSE_CONTROL_SENDER, ACT_CTL_BANK);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, " ",                    HOUSE_CONTROL_SENDER, ACT_CTL_BACK);

        // Paired with the row below it: the two things you do to the room's
        // furniture as a whole. Reading them is the harmless one and goes
        // first, which also puts a row between the group's blank and the
        // destructive one.
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "List Furniture in Room", HOUSE_CONTROL_SENDER, ACT_CTL_LIST);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Pick all Furniture up", HOUSE_CONTROL_SENDER, ACT_CTL_EMPTY);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Close",                HOUSE_CONTROL_SENDER, ACT_CTL_CLOSE);

        player->PlayerTalkClass->SendGossipMenu(HOUSE_CONTROL_TEXT, go->GetObjectGuid());
    }

    // THE SETTINGS PAGE. Every row carries its own state, so the page is the
    // answer to "how is this set up" as well as the way to change it -- the
    // same rule the edit-mode row above follows, and the reason clicking one
    // re-sends the page rather than closing.
    //
    // WHY THIS EXISTS WHEN `.house setting` ALREADY DOES. A command you have to
    // know the name of is not a way to find out that a setting is there at all.
    // The gear is the one thing every house has and every player walks up to.
    void SendControlSettings(Player* player, GameObject* go)
    {
        GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
        player->PlayerTalkClass->ClearMenus();

        bool const staff = player->GetSession()->GetSecurity() >= SEC_DEVELOPER;

        for (uint32 i = 0; i < HOUSE_SETTING_MAX; ++i)
        {
            HouseSetting const s = HouseSetting(i);

            // A RETIRED SLOT IS A HOLE IN THE TABLE. Ids are permanent so that
            // saved rows are never reinterpreted, which means the enum outlives
            // the settings it names and every loop over it steps past the gaps.
            if (!HouseMgr::IsSettingLive(s))
                continue;

            // A SERVER-WIDE SETTING IS NOT DRAWN FOR SOMEBODY WHO CANNOT
            // CHANGE IT. The rule this house already follows for a guest at
            // the gear: a row that can only answer "no" reads worse than no
            // row. `.house settings` still LISTS it, marked server-wide,
            // because knowing how placement behaves is worth having even when
            // changing it is not yours to do -- a list describes, a menu
            // offers, and those are different promises.
            if (HouseMgr::SettingIsGlobal(s) && !staff)
                continue;

            // The lock has its own row on the main page, and one control in
            // two places is two labels that can disagree about the state.
            if (s == HOUSE_SETTING_LOCKED)
                continue;

            // "Point-and-click placing: on" -- the TITLE rather than the word
            // you would type, because a row of underscores in a gossip menu
            // reads as a variable name rather than a choice. The command's own
            // list is where the typed form is taught.
            std::string label = std::string(HouseMgr::SettingTitle(s)) + ": " +
                                HouseMgr::SettingLabel(s, sHouseMgr.GetSetting(player, s));

            menu.AddMenuItem(HOUSE_GOSSIP_ICON, label.c_str(),
                             HOUSE_CONTROL_SENDER, ACT_CTL_SETTING_0 + i);
        }

        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Back", HOUSE_CONTROL_SENDER, ACT_CTL_BACK);
        player->PlayerTalkClass->SendGossipMenu(HOUSE_CONTROL_SETTINGS_TEXT, go->GetObjectGuid());
    }

    // A confirm is a SECOND MENU here, always: this build's SMSG_GOSSIP_MESSAGE
    // writes index, icon, coded and text per option and nothing else, so
    // AddMenuItem's BoxMessage is never transmitted and there are no popups.
    void SendControlConfirm(Player* player, GameObject* go, uint32 count)
    {
        GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
        player->PlayerTalkClass->ClearMenus();

        std::ostringstream yes;
        yes << "Yes, pick all " << count << " up";
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, yes.str().c_str(), HOUSE_CONTROL_SENDER, ACT_CTL_EMPTY_YES);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "No, leave it as it is", HOUSE_CONTROL_SENDER, ACT_CTL_CLOSE);

        player->PlayerTalkClass->SendGossipMenu(HOUSE_CONTROL_CONFIRM_TEXT, go->GetObjectGuid());
    }
}

static bool GossipHello_go_house_control(Player* player, GameObject* go)
{
    House* house = sHouseMgr.GetHouseByExitMarker(go ? go->GetObjectGuid() : ObjectGuid());
    if (!house)
        return false;

    // A GUEST GETS ONE LINE AND NO MENU. Every option here mutates somebody
    // else's house, so a menu full of refusals reads worse than no menu -- and
    // the way out itself still works for them, which is what they walked over
    // here for.
    if (!sHouseMgr.CanEditHouse(player, *house))
    {
        ChatHandler(player).SendSysMessage("This is not your house. Walk into the doorway to leave.");
        return true;
    }

    SendControlMain(player, go);
    return true;
}

static bool GossipSelect_go_house_control(Player* player, GameObject* go, uint32 sender, uint32 action)
{
    if (sender != HOUSE_CONTROL_SENDER)
        return false;

    // RE-RESOLVED ON EVERY CLICK, not trusted from the menu that drew it. A
    // gossip menu is a photograph: the player can be teleported out, or the
    // house can change hands, between the draw and the click.
    House* house = sHouseMgr.GetHouseByExitMarker(go ? go->GetObjectGuid() : ObjectGuid());
    if (!house || !sHouseMgr.CanEditHouse(player, *house))
    {
        player->PlayerTalkClass->CloseGossip();
        return true;
    }

    std::string error;

    switch (action)
    {
        // Re-sends the menu rather than closing, because the label it just
        // changed IS the answer -- closing would leave you to reopen the gear to
        // find out whether the click landed.
        case ACT_CTL_EDIT:
        {
            bool const on = !sHouseMgr.IsEditingAll(player);
            if (!sHouseMgr.SetEditMode(player, on, error) && !error.empty())
                ChatHandler(player).SendSysMessage(error.c_str());
            SendControlMain(player, go);
            return true;
        }

        // "List Furniture in Room", in the last group above the pick-up-all
        // row. `.house object list` and the addon's Control tab print the same
        // thing.
        case ACT_CTL_LIST:
            player->PlayerTalkClass->CloseGossip();
            HousePrintObjectList(player);
            return true;

        // The same call the walk-in poll makes, so there is one way out of a
        // house and not two that can drift. Closed first: the teleport pulls
        // the gossip's own object out from under it.
        case ACT_CTL_LEAVE:
        {
            player->PlayerTalkClass->CloseGossip();
            if (!sHouseMgr.SendPlayerToPortal(player, error) && !error.empty())
                ChatHandler(player).SendSysMessage(error.c_str());
            return true;
        }

        // Re-sends the page like the edit-mode row: the label is the answer.
        // Refused outside your own house for the reason the row is not drawn
        // there; the index arrives off the wire.
        case ACT_CTL_LOCK:
            if (sHouseMgr.GetHouseByAccount(player->GetSession()->GetAccountId()) == house)
                sHouseMgr.SetSetting(player, HOUSE_SETTING_LOCKED,
                                     sHouseMgr.GetSetting(player, HOUSE_SETTING_LOCKED) ? 0 : 1);
            SendControlMain(player, go);
            return true;

        case ACT_CTL_SETTINGS:
            SendControlSettings(player, go);
            return true;

        case ACT_CTL_BACK:
            SendControlMain(player, go);
            return true;

        // Both, deliberately: the window for anyone running the addon, and the
        // chat list for anyone not. The push is harmless with nothing listening,
        // and a menu option that silently does nothing is the worse failure.
        case ACT_CTL_STORAGE:
            player->PlayerTalkClass->CloseGossip();
            HouseOpenStorageWindow(player);
            HousePrintStorage(player);
            return true;

        // The player's OWN bank, opened away from any banker -- exactly what the
        // shipped .bank command does. One limit worth knowing: buying bank BAG
        // SLOTS checks GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BANKER) and
        // will refuse from here. Putting things in and taking them out go
        // through ordinary inventory swaps and work.
        case ACT_CTL_BANK:
            player->PlayerTalkClass->CloseGossip();
            player->GetSession()->SendShowBank(player->GetObjectGuid());
            return true;

        case ACT_CTL_EMPTY:
            SendControlConfirm(player, go, sHouseMgr.CountPlayerObjects(*house));
            return true;

        case ACT_CTL_EMPTY_YES:
        {
            player->PlayerTalkClass->CloseGossip();

            uint32 removed = 0, kept = 0, noRoom = 0;
            if (!sHouseMgr.ClearHouse(player, removed, kept, noRoom, error))
            {
                ChatHandler(player).SendSysMessage(error.c_str());
                return true;
            }
            ChatHandler(player).PSendSysMessage("Removed %u.", removed);
            if (noRoom)
                ChatHandler(player).PSendSysMessage("%u would not fit in your bags and are still here.", noRoom);
            return true;
        }

        case ACT_CTL_HELP:
            player->PlayerTalkClass->CloseGossip();
            HousePrintHouseHelp(player);
            return true;

        case ACT_CTL_CLOSE:
            player->PlayerTalkClass->CloseGossip();
            return true;

        // The settings block, resolved by subtraction rather than listed, so a
        // new HouseSetting needs nothing here. Re-sends the page for the same
        // reason the edit-mode row does: the label it just changed IS the
        // answer, and closing would leave you to reopen the gear to see it.
        default:
            if (action >= ACT_CTL_SETTING_0 &&
                action < ACT_CTL_SETTING_0 + HOUSE_SETTING_MAX)
            {
                HouseSetting const s = HouseSetting(action - ACT_CTL_SETTING_0);

                // A RETIRED SLOT IS NEVER DRAWN, so an honest client cannot
                // send this -- but the index arrives off the wire, and the
                // draw loop's guard does not travel with it. Without this, a
                // crafted packet writes a house_setting row for a dead id:
                // litter rather than a crash, and still a row nothing will
                // ever read or clean up. Every other loop over the enum skips
                // the holes; so does this one now.
                if (!HouseMgr::IsSettingLive(s))
                {
                    player->PlayerTalkClass->CloseGossip();
                    return true;
                }

                uint32 const value = sHouseMgr.GetSetting(player, s) ? 0 : 1;
                sHouseMgr.SetSetting(player, s, value);

                // The row's own label already reports the change, so this
                // speaks only where a state needs explaining -- which today is
                // the one that cannot stop the client drawing its circle.
                if (char const* note = HouseMgr::SettingNote(s, value))
                    if (*note)
                        ChatHandler(player).SendSysMessage(note);

                SendControlSettings(player, go);
                return true;
            }

            player->PlayerTalkClass->CloseGossip();
            return true;
    }
}

void AddSC_house_control()
{
    // pGOHello, NOT pGOGossipHello. The first fires BEFORE the goober state
    // machine and returning true stops it destroying the object after a single
    // click; the second fires inside it. pGOGossipSelect is the only route back
    // from CMSG_GOSSIP_SELECT_OPTION for a gameobject guid.
    Script* pNewScript = new Script;
    pNewScript->Name = "go_house_control";
    pNewScript->pGOHello = &GossipHello_go_house_control;
    pNewScript->pGOGossipSelect = &GossipSelect_go_house_control;
    pNewScript->RegisterSelf();
}

void AddSC_house_portal()
{
    // The portal itself carries no menu -- it cannot be clicked, and hanging one
    // on it only produced hooks that never fired.
    Script* pNewScript = new Script;
    pNewScript->Name = "go_house_portal";
    pNewScript->GOGetAI = &GetAI_go_house_portal;
    pNewScript->RegisterSelf();

    pNewScript = new Script;
    pNewScript->Name = "go_house_exit";
    pNewScript->GOGetAI = &GetAI_go_house_exit;
    pNewScript->RegisterSelf();

    pNewScript = new Script;
    pNewScript->Name = "go_house_portal_mark";
    pNewScript->pGOHello = &GossipHello_go_house_portal_mark;
    pNewScript->pGOGossipSelect = &GossipSelect_go_house_portal_mark;
    pNewScript->RegisterSelf();
}
