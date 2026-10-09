/*
 * House critters -- rabbits, cats, turtles, anything that potters about.
 *
 * THE WHOLE FEATURE IS ONE LINE DIFFERENT FROM FURNITURE, and that is not a
 * figure of speech. ObjectGridLoader::Visit loads a grid from two sets, the
 * global one and the instance's own:
 *
 *     LoadHelper(cell_guids.gameobjects, ...);
 *     LoadHelper(map->GetPersistentState()->GetCellObjectGuids(id).gameobjects, ...);
 *
 * and the creature overload three lines below it does exactly the same thing
 * to `.creatures`. So `AddCreatureToGrid` where `AddGameobjectToGrid` stood is
 * the entire trick that makes a rabbit belong to one house rather than to
 * every house on map 28.
 *
 * Map 28 was chosen partly for carrying no creature spawns of its own -- still
 * true, checked 2026-09-04, zero rows -- so nothing of the world's leaks in
 * beside them. It also has mmaps, which map 169 never did, so they can
 * actually walk.
 *
 * CRATES ARRIVED 2026-09-04 (sql/custom/058 + 059). An animal is bought as an
 * item, released by right-clicking it in your own house, and boxed back up by
 * picking it up -- exactly the furniture loop, sharing furniture's item script
 * and its on-use spell so that no new spell script name had to be bound. See
 * the ---- crates ---- block below and HouseMgr::UseCritterItem.
 *
 * CLICKING AND NAMING ARRIVED THE SAME DAY. Right-click one you own for a menu
 * (per-viewer UNIT_NPC_FLAGS, see the note above SendCritterMenu) and name it
 * (the pet name query, the only per-instance naming this client has -- see
 * DressCritter). docs/critters.md is the whole story.
 *
 * STILL NOT DONE: thumbnails for pets, and anything resembling feeding,
 * breeding or following.
 */

#include "Housing/HouseMgr.h"

#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "World.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Creature.h"
#include "Map.h"
#include "MapManager.h"
#include "MapPersistentStateMgr.h"
#include "Chat.h"
#include "ComfyCommands.h"
#include "GossipDef.h"
#include "ScriptedGossip.h"
#include "Language.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace
{
    // WHAT MAKES A RABBIT A HOUSE RABBIT. IMMUNE_TO_PLAYER and IMMUNE_TO_NPC
    // stop anything damaging it -- the player, a guest, a hunter pet, a bot's
    // pet -- and NON_ATTACKABLE_2 takes the attack cursor and the red nameplate
    // off, so it does not merely survive being attacked, it never looks like a
    // target in the first place.
    //
    // DELIBERATELY STILL SELECTABLE. NOT_SELECTABLE would make it scenery you
    // cannot click, and the whole appeal is that it is alive; being able to
    // target one is also how you tell the server which of three rabbits you
    // mean, later, when there is something to click for.
    uint32 const HOUSE_CRITTER_FLAGS = UNIT_FLAG_NON_ATTACKABLE_2
                                     | UNIT_FLAG_IMMUNE_TO_PLAYER
                                     | UNIT_FLAG_IMMUNE_TO_NPC;

    // Not every creature is a pet. type 8 is CREATURE_TYPE_CRITTER, which is
    // the rabbits and cats and turtles and nothing else -- 129 templates here.
    // A developer may place anything, because "put a cow in my house" is a
    // reasonable thing for the person building the feature to want.
    bool IsCritter(CreatureInfo const* info)
    {
        return info && info->type == CREATURE_TYPE_CRITTER;
    }
}

//== loading =================================================================

void HouseMgr::LoadCritters()
{
    m_critters.clear();
    m_nextCritterGuid = 0;

    // REFUSE RATHER THAN COLLIDE, the same bargain the furniture block makes.
    // Two ways to be wrong and both are silent months later:
    //
    //   the TEMPORARY floor below our ceiling -- a summon in some instance
    //   eventually lands on a critter's guid;
    //   the STATIC floor above our floor -- meaning tw_world.creature has
    //   grown past 8,000,000 in some future world update, and world spawns now
    //   overlap us.
    //
    // The gameobject check only tests the first because the second cannot
    // happen there; here it costs one comparison, so it is tested too.
    uint32 const firstTemp = sObjectMgr.GetFirstTemporaryCreatureLowGuid();
    uint32 const reserve   = sWorld.getConfig(CONFIG_UINT32_GUID_RESERVE_SIZE_CREATURE);
    uint32 const firstStatic = firstTemp > reserve ? firstTemp - reserve : 0;

    if (firstTemp <= HOUSE_CRITTER_GUID_MAX || firstStatic >= HOUSE_CRITTER_GUID_MIN)
    {
        sLog.outError("Housing: creature guids %u (static) / %u (temporary) do not leave the house critter "
                      "block %u-%u free. Set GuidReserveSize.Creature = %u in mangosd.conf and restart. "
                      "Critters disabled; the rest of housing is unaffected.",
                      firstStatic, firstTemp, uint32(HOUSE_CRITTER_GUID_MIN), uint32(HOUSE_CRITTER_GUID_MAX),
                      uint32(HOUSE_CRITTER_GUID_MAX) - firstStatic + 1);
        return;
    }

    m_nextCritterGuid = HOUSE_CRITTER_GUID_MIN;

    uint32 orphans = 0;
    if (QueryResult* result = CharacterDatabase.Query(
            "SELECT id, house_id, slot, entry, item_entry, custom_name, x, y, z, o, wander FROM house_critter ORDER BY id"))
    {
        do
        {
            Field* f = result->Fetch();
            HouseCritter c;
            c.guid    = f[0].GetUInt32();
            c.houseId = f[1].GetUInt32();
            c.slot    = f[2].GetUInt32();
            c.entry     = f[3].GetUInt32();
            c.itemEntry  = f[4].GetUInt32();
            c.customName = f[5].GetCppString();
            c.x = f[6].GetFloat(); c.y = f[7].GetFloat();
            c.z = f[8].GetFloat(); c.o = f[9].GetFloat();
            c.wander     = f[10].GetFloat();

            House* house = GetHouseById(c.houseId);
            if (!house)
            {
                ++orphans;
                continue;
            }
            if (!sObjectMgr.GetCreatureTemplate(c.entry))
            {
                sLog.outErrorDb("Housing: house_critter %u references creature_template %u, which does not exist. Skipped.",
                                c.guid, c.entry);
                continue;
            }

            m_critters[c.guid] = c;
            house->critters.push_back(c.guid);
            if (c.guid >= m_nextCritterGuid)
                m_nextCritterGuid = c.guid + 1;
        }
        while (result->NextRow());
        delete result;
    }

    if (orphans)
        sLog.outErrorDb("Housing: %u house_critter rows point at a house that no longer exists.", orphans);

    // Number anything unnumbered and write it back, exactly as the furniture
    // loader does -- a row with no slot is a row no command can name, so
    // healing beats warning. Covers a hand-written INSERT and any future
    // migration that forgets.
    uint32 numbered = 0;
    for (auto& itr : m_houses)
    {
        House& house = itr.second;
        for (uint32 guid : house.critters)
        {
            HouseCritter& c = m_critters[guid];
            if (c.slot)
                continue;

            c.slot = AllocateCritterSlot(house);
            CharacterDatabase.PExecute("UPDATE house_critter SET slot = %u WHERE id = %u", c.slot, c.guid);
            ++numbered;
        }
    }
    if (numbered)
        sLog.outString(">> Housing: numbered %u critter(s) that had no slot.", numbered);

    // Clear EVERY row, not just the loaded ones -- an orphan still owns its
    // primary key, and handing its id to a fresh placement makes the INSERT
    // fail, which on this server is a hard crash rather than a warning.
    if (QueryResult* result = CharacterDatabase.Query("SELECT MAX(id) FROM house_critter"))
    {
        uint32 const maxId = result->Fetch()[0].GetUInt32();
        if (maxId >= m_nextCritterGuid)
            m_nextCritterGuid = maxId + 1;
        delete result;
    }

    sLog.outString(">> Housing: %u critter(s).", uint32(m_critters.size()));
}

//== spawning ================================================================

void HouseMgr::RegisterCritterWithMap(Map* map, HouseCritter const& c)
{
    CreatureData& data = sObjectMgr.NewOrExistCreatureData(c.guid);
    data.creature_id.fill(0);
    data.creature_id[0]   = c.entry;
    data.position.mapId   = HOUSE_MAP_ID;
    data.position.x       = c.x;
    data.position.y       = c.y;
    data.position.z       = c.z;
    data.position.o       = c.o;
    data.spawntimesecsmin = 0;                  // it cannot die, so this never runs
    data.spawntimesecsmax = 0;
    data.wander_distance  = c.wander;

    // 0 yards is IDLE, not "random movement with no range". Asking for random
    // motion with a zero radius is a movement generator with nothing to do,
    // and IDLE is the state that actually means "stand there" -- the critter
    // still plays its own idle animations, so it does not read as frozen.
    data.movement_type    = c.wander > 0.0f ? RANDOM_MOTION_TYPE : IDLE_MOTION_TYPE;
    data.health_percent   = 100.0f;
    data.mana_percent     = 100.0f;
    data.spawn_flags      = 0;
    data.visibility_mod   = 0.0f;
    data.instanciatedContinentInstanceId = 0;

    // The per-instance grid set, NOT sObjectMgr's -- the one line that makes a
    // rabbit belong to one house. See the header of this file.
    if (map)
        map->GetPersistentState()->AddCreatureToGrid(c.guid, &data);
}

Creature* HouseMgr::SpawnCritterNow(Map* map, HouseCritter const& c)
{
    if (!map || !map->IsLoaded(c.x, c.y))
        return nullptr;                         // the grid will load it when it comes in

    Creature* creature = new Creature;
    if (!creature->LoadFromDB(c.guid, map))
    {
        sLog.outError("Housing: could not spawn critter %u (entry %u).", c.guid, c.entry);
        delete creature;
        return nullptr;
    }
    map->Add(creature);
    return creature;
}

// Called from Creature::LoadFromDB for anything in our guid block, which is
// the funnel BOTH spawn routes go through: SpawnCritterNow when you place one,
// and ObjectGridLoader when somebody walks into a house that already has one.
// Dressing at the placement site instead would give you a rabbit that was
// harmless until the first relog.
// IMMUNITY HERE IS THE FLAG AND NOTHING ELSE. There is no SetImmuneToPlayer
// in this core -- IsImmuneToPC() (Creature.h:580) is a straight read of
// UNIT_FLAG_IMMUNE_TO_PLAYER, so setting the bit IS setting the immunity.
void HouseMgr::DressCritter(Creature* creature, HouseCritter const& c)
{
    creature->SetFlag(UNIT_FIELD_FLAGS, HOUSE_CRITTER_FLAGS);

    // GOSSIP GOES ON HERE AND IS TAKEN OFF PER VIEWER, which is the opposite of
    // how it reads and is forced by Object::_SetCreateBits:
    //
    //     if (GetUInt32Value(index) != 0)
    //         updateMask->SetBit(index);
    //
    // A ZERO FIELD IS NEVER IN THE CREATION BLOCK. A stock critter's
    // UNIT_NPC_FLAGS is 0, so setting the flag only inside BuildValuesUpdate
    // meant the field was never sent and the per-viewer branch never ran at all
    // -- no talk cursor, no packet, nothing to debug. That is also why every
    // other rule in that function masks a flag OFF: trainer, stablemaster and
    // flightmaster are all non-zero on the object to begin with.
    //
    // So the object really carries it, and BuildValuesUpdate strips it for
    // anyone who is not the owner. A guest's client is told 0, exactly as
    // before; the difference is only that there is now a field to tell them
    // about.
    creature->SetFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);

    // ---- A PER-ANIMAL NAME (proven by probe 2026-09-04) -----------------
    //
    // A creature's name comes from creature_template and the client caches it
    // BY ENTRY, so naming one rabbit would name every rabbit. The one vanilla
    // mechanism keyed per instance is the pet name query: the client caches
    // those by PET NUMBER, and UNIT_FIELD_PET_NAME_TIMESTAMP exists precisely
    // so a renamed hunter pet invalidates that cache.
    //
    // IT ASKS FOR AN OWNERLESS UNIT, which is the fact that could not be read
    // out of the core and had to be measured:
    //
    //   [critter-probe] CMSG_PET_NAME_QUERY petnumber=8000000
    //                   guid=Creature (Entry: 1933 Guid: 8000000)
    //
    // No CREATEDBY, no SUMMONEDBY, no charmer, no pet frame -- just a non-zero
    // UNIT_FIELD_PETNUMBER. Do not "tidy" the three writes below into a
    // condition about ownership; ownerless is the tested case.
    //
    // InitCharmInfo is inert -- a passive holder, no owner, no CHARMEDBY, no
    // faction change, no following -- and the only general-purpose branch that
    // keys off it (MotionMaster::Initialize) just clears three booleans that
    // were already false. Checked before writing this.
    //
    // THE PET NUMBER IS THE CRITTER GUID, which is unique, stable across a
    // relog, and starts at 8,000,000 -- far above anything GeneratePetNumber
    // will reach, so a real hunter pet can never collide and inherit a rabbit's
    // name out of the client's cache.
    creature->InitCharmInfo(creature);
    creature->GetCharmInfo()->SetPetNumber(c.guid, true);
    creature->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(time(nullptr)));
}

// GIVING AN ANIMAL A NAME.
//
// The name reaches the client through the PET NAME query, which is the only
// per-instance naming this client has: creature names come from
// creature_template and are cached BY ENTRY, so naming one rabbit would name
// every rabbit in the world. Pet names are cached by PET NUMBER instead, and
// UNIT_FIELD_PET_NAME_TIMESTAMP is the field that invalidates that cache --
// which is exactly what renaming needs, and is a strong hint this is the
// intended road.
//
// Sanitised rather than refused. Somebody typing an apostrophe at a rabbit
// should get a named rabbit, not a lecture -- so the awkward characters are
// removed and whatever is left is the name. Only an empty result is an error.
//
// WHAT IS STRIPPED AND WHY:
//   '|'  starts a chat escape (|c colour, |H hyperlink). An unbalanced one
//        can eat the rest of a chat line.
//   ':'  is the addon sync's field separator -- ComfyHousing parses
//        slot:entry:count:name with one string.find, and CLAUDE.md's rule for
//        that channel is to SANITISE names rather than escape them.
//   control bytes, which the dashboard would repaint forever.
static std::string CleanCritterName(std::string name)
{
    std::string out;
    out.reserve(name.size());

    for (size_t i = 0; i < name.size(); ++i)
    {
        unsigned char const ch = (unsigned char)name[i];
        if (ch < 0x20 || ch == 0x7F)        // control bytes
            continue;
        if (ch == '|' || ch == ':')
            continue;
        out.push_back((char)ch);
    }

    // Trim, so " " is empty rather than a name made of spaces.
    size_t const first = out.find_first_not_of(' ');
    if (first == std::string::npos)
        return std::string();
    size_t const last = out.find_last_not_of(' ');
    out = out.substr(first, last - first + 1);

    if (out.size() > HOUSE_CRITTER_NAME_MAX)
        out.resize(HOUSE_CRITTER_NAME_MAX);

    return out;
}

bool HouseMgr::NameCritter(Player* player, uint32 guid, std::string name, std::string& error)
{
    House* house = GetHouseAt(player);
    if (!house)
    {
        error = "You are not in a house.";
        return false;
    }
    if (!CanEditHouse(player, *house))
    {
        error = "This is not your house.";
        return false;
    }

    std::map<uint32, HouseCritter>::iterator itr = m_critters.find(guid);
    if (itr == m_critters.end() || itr->second.houseId != house->id)
    {
        error = "That animal is not in this house.";
        return false;
    }

    std::string const clean = CleanCritterName(name);
    if (clean.empty())
    {
        error = "That is not a name.";
        return false;
    }

    itr->second.customName = clean;

    std::string escaped = clean;
    CharacterDatabase.escape_string(escaped);
    CharacterDatabase.PExecute("UPDATE house_critter SET custom_name = '%s' WHERE id = %u",
                               escaped.c_str(), guid);

    // THE TIMESTAMP IS THE WHOLE OF RENAMING. The client caches a pet name by
    // pet number and will not ask again while its cached copy looks current;
    // bumping this replicated field is what makes it re-query. Without it the
    // animal keeps its old name on screen until something else forces a fresh
    // creation block.
    Map* map = player->GetMap();
    if (map)
        if (Creature* creature = map->GetCreature(ObjectGuid(HIGHGUID_UNIT, itr->second.entry, guid)))
            creature->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(time(nullptr)));

    return true;
}

// Answered into WorldSession::SendPetNameQuery. False for anything that is not
// one of ours, and for an animal that has never been named -- the caller then
// keeps Creature::GetName(), the species, which the client caches per pet
// number just as happily.
// The seam Creature::LoadFromDB calls. A free function for the same reason
// HouseGameObjectUse is one: it keeps the core's include of housing to a
// declaration, and housing owns the guid test.
void HouseDressCritter(Creature* creature)
{
    if (!creature)
        return;

    if (HouseCritter const* c = sHouseMgr.GetCritter(creature->GetGUIDLow()))
        sHouseMgr.DressCritter(creature, *c);
}

bool HouseCritterPetName(uint32 creatureGuidLow, std::string& name)
{
    HouseCritter const* c = sHouseMgr.GetCritter(creatureGuidLow);
    if (!c || c->customName.empty())
        return false;

    name = c->customName;
    return true;
}

//== the manager's own bookkeeping ==========================================

uint32 HouseMgr::AllocateCritterGuid()
{
    if (!m_nextCritterGuid || m_nextCritterGuid >= HOUSE_CRITTER_GUID_MAX)
        return 0;
    return m_nextCritterGuid++;
}

uint32 HouseMgr::AllocateCritterSlot(House const& house) const
{
    uint32 highest = 0;
    for (uint32 guid : house.critters)
    {
        auto itr = m_critters.find(guid);
        if (itr != m_critters.end() && itr->second.slot > highest)
            highest = itr->second.slot;
    }
    return highest + 1;
}

uint32 HouseMgr::CountCritters(House const& house) const
{
    return uint32(house.critters.size());
}

HouseCritter const* HouseMgr::GetCritter(uint32 guid) const
{
    auto itr = m_critters.find(guid);
    return itr == m_critters.end() ? nullptr : &itr->second;
}

std::vector<HouseCritter const*> HouseMgr::GetCritters(uint32 houseId)
{
    std::vector<HouseCritter const*> out;
    House* house = GetHouseById(houseId);
    if (!house)
        return out;

    for (uint32 guid : house->critters)
    {
        auto itr = m_critters.find(guid);
        if (itr != m_critters.end())
            out.push_back(&itr->second);
    }
    return out;
}

std::string HouseMgr::CritterName(uint32 entry)
{
    CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(entry);
    if (!info)
        return "Something";
    return info->name;
}

// THE GIVEN NAME WINS, with the species dropped rather than kept beside it:
// "Bramble" is what the player calls it and what the nameplate now says, and
// repeating "Rabbit" every time would be the one word they least need.
std::string HouseMgr::CritterShortName(uint32 guid)
{
    HouseCritter const* c = sHouseMgr.GetCritter(guid);
    if (!c)
        return "Something";

    return c->customName.empty() ? CritterName(c->entry) : c->customName;
}

std::string HouseMgr::CritterLabel(uint32 guid)
{
    HouseCritter const* c = sHouseMgr.GetCritter(guid);
    if (!c)
        return "Something";

    std::ostringstream s;
    s << CritterShortName(guid) << " (" << c->slot << ')';
    return s.str();
}

// TWO ANSWERS, NOT THREE. Furniture resolves a typed id, then the selection,
// then the nearest -- because two chairs can be half a yard apart and there
// may be a hundred of them. A house holds ten critters at most and they are
// things you walk up to, so a typed number and "the one in front of me" is the
// whole vocabulary; a selection would be state to explain for no gain.
//
// A raw guid still resolves, exactly as it does for furniture: anything at or
// above HOUSE_CRITTER_GUID_MIN cannot be a slot, so one argument carries both
// with no ambiguity.
uint32 HouseMgr::FindCritter(Player* player, uint32 typed) const
{
    House* house = const_cast<HouseMgr*>(this)->GetHouseAt(player);
    if (!house)
        return 0;

    if (typed >= HOUSE_CRITTER_GUID_MIN)
    {
        auto itr = m_critters.find(typed);
        return itr != m_critters.end() && itr->second.houseId == house->id ? typed : 0;
    }

    if (typed)
    {
        for (uint32 guid : house->critters)
        {
            auto itr = m_critters.find(guid);
            if (itr != m_critters.end() && itr->second.slot == typed)
                return guid;
        }
        return 0;
    }

    uint32 best = 0;
    float  bestDist = HOUSE_CRITTER_REACH;
    for (uint32 guid : house->critters)
    {
        auto itr = m_critters.find(guid);
        if (itr == m_critters.end())
            continue;

        float const d = player->GetDistance(itr->second.x, itr->second.y, itr->second.z);
        if (d <= bestDist)
        {
            bestDist = d;
            best = guid;
        }
    }
    return best;
}

//== placing, moving, removing ==============================================

bool HouseMgr::PlaceCritter(Player* player, uint32 entry, float wander, std::string& error,
                            uint32 itemEntry)
{
    House* house = GetHouseAt(player);
    if (!house)
    {
        error = "You are not in a house.";
        return false;
    }
    if (!CanEditHouse(player, *house))
    {
        error = "This is not your house.";
        return false;
    }
    if (!m_nextCritterGuid)
    {
        error = "Critters are not available on this server.";
        return false;
    }
    if (CountCritters(*house) >= HOUSE_MAX_CRITTERS)
    {
        error = "Your house is full of animals. Send one away first.";
        return false;
    }

    CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(entry);
    if (!info)
    {
        error = "There is no creature with that id.";
        return false;
    }
    if (!IsCritter(info) && player->GetSession()->GetSecurity() < SEC_DEVELOPER)
    {
        error = "That is not a critter.";
        return false;
    }

    if (wander < 0.0f) wander = 0.0f;
    if (wander > HOUSE_CRITTER_WANDER_MAX) wander = HOUSE_CRITTER_WANDER_MAX;

    HouseCritter c;
    c.houseId   = house->id;
    c.entry     = entry;
    c.wander    = wander;
    c.itemEntry = itemEntry;                // 0 unless it came out of a crate

    // THE CHALK AIMS ANIMALS TOO, and it is consulted here rather than by the
    // caller for the reason PlaceObject gives: one rule about where a thing
    // ends up, and a mark that can only be spent by a placement that really
    // happened. Re-checked rather than trusted, so walking away from your own
    // mark falls back rather than flinging a rabbit across the room.
    float mark[3];
    bool const usedMark = GetMark(player, mark)
                       && player->IsWithinDist3d(mark[0], mark[1], mark[2], HOUSE_PLACE_DISTANCE_MAX);

    if (usedMark)
    {
        c.x = mark[0];
        c.y = mark[1];
        c.z = mark[2];
        c.o = player->GetOrientation();
    }
    else
    {
        // WHERE YOU STAND, not two yards ahead like furniture. A chair pushed
        // into your own feet is useless and an animal walks off anyway, so the
        // arm's-length offset buys nothing -- and it costs the one thing no
        // fallback can recover: standing somewhere is PROOF the spot is
        // stand-on-able, which map->GetHeight cannot tell you on map 28. It
        // reads the raw .map plane at z = 0, so every WMO floor a template
        // builds on is invisible to it.
        c.x = player->GetPositionX();
        c.y = player->GetPositionY();
        c.z = player->GetPositionZ();
        c.o = player->GetOrientation();
    }

    c.guid = AllocateCritterGuid();
    if (!c.guid)
    {
        error = "Out of critter ids.";
        return false;
    }
    c.slot = AllocateCritterSlot(*house);

    CharacterDatabase.PExecute(
        "INSERT INTO house_critter (id, house_id, slot, entry, item_entry, x, y, z, o, wander, placed_at) "
        "VALUES (%u, %u, %u, %u, %u, %f, %f, %f, %f, %f, " UI64FMTD ")",
        c.guid, c.houseId, c.slot, c.entry, c.itemEntry,
        c.x, c.y, c.z, c.o, c.wander, uint64(time(nullptr)));

    uint32 const guid = c.guid;
    m_critters[guid] = c;
    house->critters.push_back(guid);

    Map* map = player->GetMap();
    RegisterCritterWithMap(map, m_critters[guid]);
    SpawnCritterNow(map, m_critters[guid]);

    // A GUEST ALREADY IN THE ROOM WILL NOT SEE IT OTHERWISE. The create block
    // goes out correctly and their client discards it, which is the same fault
    // that keeps a player standing in a spawned building from being drawn --
    // see HouseMgr::BeatCritters. The owner is fine either way; this is for
    // whoever was already standing there.
    ArmCritterBeat(map);

    // SPENT, AND ONLY NOW. Everything above can refuse -- a house full of
    // animals, a creature entry that is not a critter, no ids left -- and every
    // one of those leaves the mark standing, so the chalk cannot be lost by
    // aiming at a house that turned out to be full.
    if (usedMark)
        ClearMark(player);

    return true;
}

// The teardown half, with no player in sight: live creature, grid entry,
// CreatureData, database row, memory. Shared by RemoveCritter (somebody
// standing there) and PurgeCritters (a reset or a move, where the map may
// legitimately be unloaded and only the persistent side needs clearing).
void HouseMgr::PurgeCritter(House& house, uint32 guid)
{
    auto itr = m_critters.find(guid);
    if (itr == m_critters.end())
        return;

    Map* map = sMapMgr.FindMap(HOUSE_MAP_ID, house.instanceId);

    if (map)
        if (Creature* creature = map->GetCreature(ObjectGuid(HIGHGUID_UNIT, itr->second.entry, guid)))
        {
            creature->CombatStop();
            creature->AddObjectToRemoveList();
        }

    if (CreatureData* data = const_cast<CreatureData*>(sObjectMgr.GetCreatureData(guid)))
        if (map)
            map->GetPersistentState()->RemoveCreatureFromGrid(guid, data);

    sObjectMgr.DeleteCreatureData(guid);
    CharacterDatabase.PExecute("DELETE FROM house_critter WHERE id = %u", guid);

    house.critters.erase(std::remove(house.critters.begin(), house.critters.end(), guid), house.critters.end());
    m_critters.erase(itr);
}

void HouseMgr::PurgeCritters(House& house)
{
    std::vector<uint32> const doomed = house.critters;
    for (uint32 guid : doomed)
        PurgeCritter(house, guid);
}

bool HouseMgr::RemoveCritter(Player* player, uint32 guid, std::string& error, uint32* returnedItem)
{
    if (returnedItem)
        *returnedItem = 0;

    House* house = GetHouseAt(player);
    if (!house)
    {
        error = "You are not in a house.";
        return false;
    }
    if (!CanEditHouse(player, *house))
    {
        error = "This is not your house.";
        return false;
    }

    auto itr = m_critters.find(guid);
    if (itr == m_critters.end() || itr->second.houseId != house->id)
    {
        error = "That animal is not in this house.";
        return false;
    }

    // GIVE THE CRATE BACK BEFORE ANYTHING IS DESTROYED, and the ORDER is the
    // whole safety of it: CanStoreNewItem first, so a refusal leaves the animal
    // standing exactly where it was. Purging first and then finding there is no
    // room would destroy something a player paid fifteen gold for.
    //
    // Rows with itemEntry 0 -- `.house critter add` -- are destroyed as they
    // always were. There is no item to give back, so there is nothing to
    // decide.
    uint32 const itemEntry = itr->second.itemEntry;
    if (itemEntry)
    {
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, 1) != EQUIP_ERR_OK)
        {
            error = "Your bags are full.";
            return false;
        }

        Item* given = player->StoreNewItem(dest, itemEntry, true);
        if (!given)
        {
            error = "Could not put that in your bags.";
            return false;
        }
        player->SendNewItem(given, 1, true, false);

        if (returnedItem)
            *returnedItem = itemEntry;
    }

    PurgeCritter(*house, guid);
    return true;
}

// Drag and wander in one function, because both rewrite the row AND have to
// re-seat the live creature -- and a second copy of the re-seat is a second
// place to forget it. Either half may be a no-op.
//
// IT RELOCATES; IT DOES NOT DESTROY AND RESPAWN. Recreating under the same guid
// looks equivalent and is not: AddObjectToRemoveList only QUEUES the old
// creature, so a fresh one added immediately shares its guid until the map
// finishes its update -- and the queued removal then takes the new one out.
// The animal would vanish until the grid next reloaded. Relocating is also
// what the furniture path does (ApplyPlacement), for the same reason.
//
// The cost of not reloading is that the three things Creature::LoadFromDB
// reads off CreatureData -- home position, wander distance, movement type --
// have to be set by hand here, because nothing re-reads that struct for a
// creature already standing in the world.
bool HouseMgr::MoveCritter(Player* player, uint32 guid, bool drag,
                           bool setWander, float wander, std::string& error)
{
    House* house = GetHouseAt(player);
    if (!house)
    {
        error = "You are not in a house.";
        return false;
    }
    if (!CanEditHouse(player, *house))
    {
        error = "This is not your house.";
        return false;
    }

    auto itr = m_critters.find(guid);
    if (itr == m_critters.end() || itr->second.houseId != house->id)
    {
        error = "That animal is not in this house.";
        return false;
    }

    HouseCritter& c = itr->second;
    Map* map = player->GetMap();

    // Out of the OLD cell first: the cell is computed from the data's position,
    // so this cannot wait until the coordinates have changed.
    if (map)
        if (CreatureData* data = const_cast<CreatureData*>(sObjectMgr.GetCreatureData(c.guid)))
            map->GetPersistentState()->RemoveCreatureFromGrid(c.guid, data);

    if (drag)
    {
        c.x = player->GetPositionX();
        c.y = player->GetPositionY();
        c.z = player->GetPositionZ();
        c.o = player->GetOrientation();
    }

    if (setWander)
    {
        if (wander < 0.0f) wander = 0.0f;
        if (wander > HOUSE_CRITTER_WANDER_MAX) wander = HOUSE_CRITTER_WANDER_MAX;
        c.wander = wander;
    }

    CharacterDatabase.PExecute(
        "UPDATE house_critter SET x = %f, y = %f, z = %f, o = %f, wander = %f WHERE id = %u",
        c.x, c.y, c.z, c.o, c.wander, c.guid);

    // Rewrites the CreatureData and puts it in the new cell, so a later grid
    // reload builds the animal where it is now.
    RegisterCritterWithMap(map, c);

    if (!map)
        return true;

    Creature* creature = map->GetCreature(ObjectGuid(HIGHGUID_UNIT, c.entry, c.guid));
    if (!creature)
    {
        // Never spawned -- placed while the grid was cold, say. Now is as good
        // a time as any.
        SpawnCritterNow(map, c);
        return true;
    }

    // CALLED OVER: it WALKS, and StartCritterWalk owns the home position, the
    // wander and the generator from here on. A `wander` change with no drag is
    // the immediate case and still re-initialises in place.
    if (drag)
    {
        StartCritterWalk(creature, c);
        return true;
    }

    creature->SetHomePosition(c.x, c.y, c.z, c.o);
    creature->SetWanderDistance(c.wander);
    creature->SetDefaultMovementType(c.wander > 0.0f ? RANDOM_MOTION_TYPE : IDLE_MOTION_TYPE);

    // Without this the animal keeps whatever generator it was born with, so a
    // rabbit told to stay put carries on roaming from its new home and one
    // told to roam stands there. The setters above are what Initialize reads.
    creature->GetMotionMaster()->Initialize();
    return true;
}

// IT WALKS OVER, IT DOES NOT BLINK. Called animals used to NearTeleportTo,
// which obeys the order and reads as nothing at all -- a rabbit is somewhere
// else between frames. Now the row and the home position are written at once
// (so the destination is the truth from this instant, and a restart mid-walk
// simply spawns it there) and the animal is sent to walk to it.
//
// A BACKSTOP RATHER THAN A PROMISE, and it is not paranoia: map 28's navmesh
// describes the flat plain and knows nothing about a spawned building, so a
// path computed across a room can run straight through a wall the animal then
// collides with. It has twenty seconds; after that it is simply put there,
// which is the old behaviour kept as the failure mode rather than the default.
//
// RE-ANCHORING NEEDS MoveRandom, NOT A POP. When the MovePoint generator
// finishes, MotionMaster pops back to the random generator that was already on
// the stack -- and RandomMovementGenerator reads its anchor ONCE, in its
// constructor, from GetRespawnCoord. So the old one keeps roaming around where
// the animal used to live. MoveRandom builds a fresh generator, which is what
// makes the new home take.
void HouseMgr::StartCritterWalk(Creature* creature, HouseCritter const& c)
{
    creature->SetHomePosition(c.x, c.y, c.z, c.o);
    creature->SetWanderDistance(c.wander);
    creature->SetDefaultMovementType(c.wander > 0.0f ? RANDOM_MOTION_TYPE : IDLE_MOTION_TYPE);

    HouseCritterWalk& w = m_critterWalks[c.guid];
    w.instanceId = creature->GetInstanceId();
    w.entry      = c.entry;
    w.x = c.x; w.y = c.y; w.z = c.z;
    w.wander     = c.wander;
    w.ticks      = HOUSE_CRITTER_WALK_TICKS;

    // The same options RandomMovementGenerator uses for its own steps, so a
    // called animal picks its way over exactly as it wanders.
    creature->GetMotionMaster()->MovePoint(HOUSE_CRITTER_WALK_ID, c.x, c.y, c.z,
                                           MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
}

void HouseMgr::RunCritterWalks(Map* map)
{
    if (!m_enabled || !map || m_critterWalks.empty())
        return;

    for (std::map<uint32, HouseCritterWalk>::iterator itr = m_critterWalks.begin(); itr != m_critterWalks.end();)
    {
        HouseCritterWalk& w = itr->second;
        if (w.instanceId != map->GetInstanceId())
        {
            ++itr;
            continue;
        }

        Creature* creature = map->GetCreature(ObjectGuid(HIGHGUID_UNIT, w.entry, itr->first));

        // Gone -- picked up, purged, or the grid unloaded under it. Nothing to
        // finish and nothing to clean up but the order itself.
        if (!creature || !creature->IsInWorld())
        {
            m_critterWalks.erase(itr++);
            continue;
        }

        bool const arrived = creature->IsWithinDist3d(w.x, w.y, w.z, HOUSE_CRITTER_WALK_ARRIVED);
        bool const gaveUp  = (w.ticks == 0);

        if (!arrived && !gaveUp)
        {
            --w.ticks;
            ++itr;
            continue;
        }

        // COULD NOT GET THERE, SO IT IS PUT THERE. The order was given and has
        // to be obeyed; a rabbit stuck behind a wall it cannot path around is
        // worse than one that blinks the last few yards.
        if (!arrived)
            creature->NearTeleportTo(w.x, w.y, w.z, creature->GetOrientation());

        // A FRESH RANDOM GENERATOR, anchored on the home position set when the
        // walk began -- see StartCritterWalk for why popping back is not
        // enough.
        if (w.wander > 0.0f)
            creature->GetMotionMaster()->MoveRandom(false, w.wander);
        else
            creature->GetMotionMaster()->MoveIdle();

        m_critterWalks.erase(itr++);
    }
}

//== crates ==================================================================

// tw_world.house_critter_item, the exact counterpart of LoadFurnitureItems.
// Both ends are checked here rather than at use time, because a bad row shows
// up in play as an item that does nothing and no error anywhere -- the same
// silent miss the collection tables have.
void HouseMgr::LoadCritterItems()
{
    m_critterItems.clear();

    QueryResult* result = WorldDatabase.Query(
        "SELECT item_entry, critter_entry, wander FROM house_critter_item");
    if (!result)
    {
        sLog.outString(">> Housing: no critter crates (house_critter_item is empty or missing).");
        return;
    }

    uint32 skipped = 0;
    do
    {
        Field* f = result->Fetch();
        uint32 const itemEntry = f[0].GetUInt32();
        CritterItem ci;
        ci.critterEntry = f[1].GetUInt32();
        ci.wander       = f[2].GetFloat();

        if (!sObjectMgr.GetItemPrototype(itemEntry))
        {
            sLog.outErrorDb("Housing: house_critter_item %u is not an item. Skipped.", itemEntry);
            ++skipped;
            continue;
        }
        if (!sObjectMgr.GetCreatureTemplate(ci.critterEntry))
        {
            sLog.outErrorDb("Housing: house_critter_item %u maps to creature_template %u, which does not exist. Skipped.",
                            itemEntry, ci.critterEntry);
            ++skipped;
            continue;
        }

        if (ci.wander < 0.0f) ci.wander = 0.0f;
        if (ci.wander > HOUSE_CRITTER_WANDER_MAX) ci.wander = HOUSE_CRITTER_WANDER_MAX;

        m_critterItems[itemEntry] = ci;
    }
    while (result->NextRow());
    delete result;

    sLog.outString(">> Housing: %u critter crate(s)%s.", uint32(m_critterItems.size()),
                   skipped ? ", some rows skipped -- see the errors above" : "");
}

// Everything PlaceCritter would refuse for, asked WITHOUT placing -- because
// releasing an animal is a one-second cast and a bar that runs and then says
// "your house is full" is a worse answer than an instant one.
//
// NOT A SECOND COPY OF THE RULES. Same ownership check, same cap, so a rule
// added to PlaceCritter is a rule this asks about. The one thing it cannot
// pre-check is the creature entry, which is fine: every entry in
// house_critter_item was validated at load.
bool HouseMgr::CanPlaceCritter(Player* player, std::string& error)
{
    House* house = GetHouseAt(player);
    if (!house)
    {
        error = "You are not in a house.";
        return false;
    }
    if (!CanEditHouse(player, *house))
    {
        error = "This is not your house.";
        return false;
    }
    if (!m_nextCritterGuid)
    {
        error = "Critters are not available on this server.";
        return false;
    }
    if (CountCritters(*house) >= HOUSE_MAX_CRITTERS)
    {
        error = "Your house is full of animals. Send one away first.";
        return false;
    }
    return true;
}

bool HouseMgr::UseCritterItem(Player* player, uint32 itemEntry, std::string& error)
{
    std::map<uint32, CritterItem>::const_iterator itr = m_critterItems.find(itemEntry);
    if (itr == m_critterItems.end())
    {
        error = "There is nothing in that crate.";
        return false;
    }

    // The crate's own radius, and the item entry so picking it up hands this
    // exact crate back. Where it lands is PlaceCritter's business: the chalk
    // mark if there is one, otherwise where the player stands.
    return PlaceCritter(player, itr->second.critterEntry, itr->second.wander, error, itemEntry);
}

//== patting ================================================================

// THE CORE ALREADY RESOLVES THE TARGET FOR US. HandleTextEmoteOpcode reads the
// emote, the variant, AND the guid the player had selected, then looks the unit
// up -- so `/pat` on a bunny arrives with everything needed and nothing to
// parse. The handler even calls Creature::AI()->ReceiveEmote two lines later,
// which is the seam this SHOULD use and cannot: ReceiveEmote needs our own AI
// class bound through creature_template.script_name, and those 129 critter rows
// belong to Turtle. Hence a free function, exactly as the gameobject click does.
void HouseCritterEmote(Player* player, uint32 creatureGuidLow, uint32 textEmote)
{
    if (!player || creatureGuidLow < HOUSE_CRITTER_GUID_MIN || creatureGuidLow > HOUSE_CRITTER_GUID_MAX)
        return;

    // FOUR EMOTES, NOT ONE. `/pat` is the one asked for, but a bunny that
    // ignores `/love` while answering `/pat` reads as broken rather than
    // specific -- these are one gesture with four spellings.
    switch (textEmote)
    {
        case TEXTEMOTE_PAT:
        case TEXTEMOTE_LOVE:
        case TEXTEMOTE_CUDDLE:
        case TEXTEMOTE_KISS:
            break;
        default:
            return;
    }

    HouseCritter const* c = sHouseMgr.GetCritter(creatureGuidLow);
    if (!c)
        return;

    Map* map = player->GetMap();
    if (!map)
        return;

    if (Creature* creature = map->GetCreature(ObjectGuid(HIGHGUID_UNIT, c->entry, creatureGuidLow)))
        sHouseMgr.PatCritter(player, creature);
}

void HouseMgr::PatCritter(Player* player, Creature* creature)
{
    if (!creature->IsInWorld())
        return;

    // Near enough to reach it. The client will happily keep a target selected
    // across the room, and hearts blooming over an animal thirty yards away is
    // somebody else's bunny reacting to you.
    if (!player->IsWithinDist(creature, INTERACTION_DISTANCE))
        return;

    time_t const now = time(nullptr);
    std::map<uint32, time_t>::iterator itr = m_critterPatted.find(creature->GetGUIDLow());
    if (itr != m_critterPatted.end() && itr->second > now)
        return;                             // silently: a refusal message on a
                                            // held-down macro is its own spam
    m_critterPatted[creature->GetGUIDLow()] = now + HOUSE_CRITTER_PAT_COOLDOWN;

    // IT LOOKS AT YOU. A reaction with no animation behind it, which matters
    // because these are 1.12 critter models -- most carry a handful of
    // animations and there is no telling from the DBC which ones a given
    // rabbit really has. Turning is geometry and always works.
    creature->SetFacingToObject(player);

    creature->SendPlaySpellVisual(HOUSE_CRITTER_HEART_VISUAL);
}

void HouseMgr::ShowCritterVisual(Player* player, uint32 guid, uint32 visualId)
{
    Map* map = player ? player->GetMap() : nullptr;
    if (!map)
        return;

    HouseCritter const* c = GetCritter(guid);
    if (!c)
        return;

    if (Creature* creature = map->GetCreature(ObjectGuid(HIGHGUID_UNIT, c->entry, guid)))
        creature->SendPlaySpellVisual(visualId);
}

//== the menu ================================================================

/*
 * RIGHT-CLICK AN ANIMAL IN YOUR OWN HOUSE AND IT OPENS A MENU.
 *
 * THE WHOLE TRICK IS ONE FLAG, SWAPPED PER VIEWER -- the same shape as edit
 * mode's GAMEOBJECT_TYPE_ID swap next door. Object::BuildValuesUpdate already
 * rewrites UNIT_NPC_FLAGS per target (that is how a trainer stops looking like
 * one to the wrong class), so a house critter is told to its OWNER that it
 * carries UNIT_NPC_FLAG_GOSSIP, and to nobody else. The client then draws the
 * talk cursor and sends CMSG_GOSSIP_HELLO.
 *
 * AND THE SERVER SIDE NEEDED NO PERSUADING AT ALL, which is the happy accident
 * that made this small. HandleGossipHelloOpcode asks
 * GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE), and CanInteractWithNPC
 * SKIPS the flag test when the mask is 0:
 *
 *     if (npcflagmask && !pCreature->HasFlag(UNIT_NPC_FLAGS, npcflagmask))
 *
 * So the server was always willing to open a gossip session with a rabbit --
 * alive, friendly, selectable, not in combat, within INTERACTION_DISTANCE, all
 * of which a house critter is. Only the client had to be convinced. Nothing
 * here fights a permission check, and the server's own copy of the flag is
 * never touched, so a guest sees an ordinary animal and every other read of
 * UNIT_NPC_FLAGS stays true.
 *
 * NO MODE, AND THEREFORE NO BLINK. Edit mode has to flash its furniture
 * because the client caches a gameobject's type from its creation block and
 * will not re-read it -- so turning the mode on has to re-send every object.
 * A critter's owner never changes while they are standing there, so the flag
 * is simply correct from the moment the creature is created for them. The
 * question "does the client re-read UNIT_NPC_FLAGS" never has to be answered.
 *
 * TWO CORE HOOKS, because a house critter can never have a script: it uses
 * stock creature_template rows we do not own, so script_name is not ours to
 * set and pGossipHello / pGossipSelect can never fire for one. The core calls
 * housing directly from the two places a creature gossip click arrives, both
 * declared in HouseMgr.h -- exactly the seam the gameobject click already uses.
 */

namespace
{
    // Distinct from HouseHandle.cpp's 700 so a stray click can never be read as
    // the other menu's. Anything else falls through to the default handling.
    uint32 const CRITTER_SENDER = 701;

    enum CritterAction
    {
        ACT_CRITTER_PAT     = 1,
        ACT_CRITTER_COME    = 2,
        ACT_CRITTER_ROAM    = 3,
        ACT_CRITTER_NAME    = 4,
        ACT_CRITTER_PICKUP  = 5,
        ACT_CRITTER_CLOSE   = 6,
    };

    // sql/custom/060. "It looks up at you."
    uint32 const CRITTER_TEXT = 6400039;

    // A blank row that reopens the page: the gossip packet has no separator, so
    // this is the only way to group anything. Same trick the furniture menu
    // uses.
    void AddSpacer(GossipMenu& menu)
    {
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, " ", CRITTER_SENDER, ACT_CRITTER_CLOSE);
    }
}

bool HouseMgr::CanOwnerTalkTo(Player* viewer, uint32 critterGuidLow) const
{
    if (!m_enabled || !viewer)
        return false;

    std::map<uint32, HouseCritter>::const_iterator itr = m_critters.find(critterGuidLow);
    if (itr == m_critters.end())
        return false;

    std::map<uint32, House>::const_iterator house = m_houses.find(itr->second.houseId);
    return house != m_houses.end() && CanEditHouse(viewer, house->second);
}

void HouseMgr::SendCritterMenu(Player* player, HouseCritter const& c, ObjectGuid guid)
{
    GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
    player->PlayerTalkClass->ClearMenus();

    // WHAT YOU CAME FOR FIRST. Patting is the reason most people click an
    // animal, and it is also the one row a player might not know exists --
    // `/pat` works but nothing advertises it.
    menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Give it a pat", CRITTER_SENDER, ACT_CRITTER_PAT);

    AddSpacer(menu);

    // ARRANGING. "Come here" rather than "Move it": you are talking to
    // something alive, and the verb the rest of housing uses for furniture
    // would read oddly on a rabbit.
    menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Come here", CRITTER_SENDER, ACT_CRITTER_COME);

    // ONE ROW, TWO STATES, and it names the state it will MOVE TO rather than
    // the one it is in -- a button that says what it does. The furniture menu's
    // "Selected -- click to clear" is the same bargain read the other way, and
    // the difference is that this one has no visible state to contradict it.
    if (c.wander > 0.0f)
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Ask it to stay put", CRITTER_SENDER, ACT_CRITTER_ROAM);
    else
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Let it wander", CRITTER_SENDER, ACT_CRITTER_ROAM);

    // TYPED INPUT, WHICH THIS CLIENT DOES HAVE. The Coded flag IS transmitted
    // -- SMSG_GOSSIP_MESSAGE writes index, icon, coded and text per option --
    // so the client opens a box and the text arrives as `code` on the select.
    // The BoxMessage argument is the part that is never sent, which is why the
    // label has to carry the whole prompt.
    menu.AddMenuItem(HOUSE_GOSSIP_ICON,
                     c.customName.empty() ? "Give it a name" : "Call it something else",
                     CRITTER_SENDER, ACT_CRITTER_NAME, "", true);

    AddSpacer(menu);

    // THE TWO ENDINGS, apart from everything above -- one keeps the animal and
    // one does not, and "Pick it up" sitting one careless row under "Come here"
    // is exactly the misclick the spacer exists to prevent.
    menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Pick it up", CRITTER_SENDER, ACT_CRITTER_PICKUP);
    menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Leave it be", CRITTER_SENDER, ACT_CRITTER_CLOSE);

    player->PlayerTalkClass->SendGossipMenu(CRITTER_TEXT, guid);
}

// The core's hook for CMSG_GOSSIP_HELLO on a creature.
bool HouseCritterGossipHello(Player* player, uint32 critterGuidLow)
{
    if (!player)
        return false;

    HouseCritter const* c = sHouseMgr.GetCritter(critterGuidLow);
    if (!c)
        return false;

    // CONSUMED FOR ANY HOUSE CRITTER, not just the owner's. The object really
    // carries UNIT_NPC_FLAG_GOSSIP now (see DressCritter), so a guest whose
    // client sends this anyway would otherwise fall through to
    // PrepareGossipMenu on a creature whose default menu is 0 -- an empty
    // window with nothing in it. Their client is told 0 and will not normally
    // ask; this is the belt to that pair of braces.
    if (!sHouseMgr.CanOwnerTalkTo(player, critterGuidLow))
    {
        player->PlayerTalkClass->CloseGossip();
        return true;
    }

    sHouseMgr.SendCritterMenu(player, *c, ObjectGuid(HIGHGUID_UNIT, c->entry, critterGuidLow));
    return true;
}

// The core's hook for CMSG_GOSSIP_SELECT_OPTION on a creature.
bool HouseCritterGossipSelect(Player* player, uint32 critterGuidLow, uint32 sender,
                              uint32 action, char const* code)
{
    if (!player || sender != CRITTER_SENDER)
        return false;

    // RE-CHECKED, NOT TRUSTED FROM WHEN THE MENU WAS DRAWN. A gossip menu is a
    // photograph: between drawing it and clicking, the player can have left the
    // house, the animal can have been picked up by the same player in another
    // window, or a reset can have purged it.
    if (!sHouseMgr.CanOwnerTalkTo(player, critterGuidLow))
    {
        player->PlayerTalkClass->CloseGossip();
        return true;
    }

    sHouseMgr.HandleCritterMenu(player, critterGuidLow, action, code);
    return true;
}

void HouseMgr::HandleCritterMenu(Player* player, uint32 guid, uint32 action, char const* code)
{
    HouseCritter const* c = GetCritter(guid);
    if (!c)
    {
        player->PlayerTalkClass->CloseGossip();
        return;
    }

    std::string const label = CritterLabel(guid);
    std::string error;

    switch (action)
    {
        case ACT_CRITTER_PAT:
        {
            Map* map = player->GetMap();
            Creature* creature = map ? map->GetCreature(ObjectGuid(HIGHGUID_UNIT, c->entry, guid)) : nullptr;
            if (creature)
                PatCritter(player, creature);

            // THE WINDOW STAYS OPEN, because patting is the one thing anybody
            // does twice. Every other row here either finishes or changes the
            // menu's own text, and both want a redraw.
            SendCritterMenu(player, *c, ObjectGuid(HIGHGUID_UNIT, c->entry, guid));
            return;
        }

        case ACT_CRITTER_COME:
            if (!MoveCritter(player, guid, true, false, 0.0f, error))
                ChatHandler(player).SendSysMessage(error.c_str());
            else
                ChatHandler(player).PSendSysMessage("%s comes over.", label.c_str());
            break;

        case ACT_CRITTER_ROAM:
        {
            // Back to the crate's own default rather than to a fixed number --
            // a penguin roams two yards and a rabbit three, and letting it
            // wander should give back what it came with.
            float const wander = c->wander > 0.0f ? 0.0f : DefaultWanderFor(*c);
            if (!MoveCritter(player, guid, false, true, wander, error))
                ChatHandler(player).SendSysMessage(error.c_str());
            else if (wander > 0.0f)
                ChatHandler(player).PSendSysMessage("%s roams %.0f yards.", label.c_str(), wander);
            else
                ChatHandler(player).PSendSysMessage("%s stays put.", label.c_str());

            // The row's own wording just changed, so redraw rather than close.
            if (HouseCritter const* now = GetCritter(guid))
            {
                SendCritterMenu(player, *now, ObjectGuid(HIGHGUID_UNIT, now->entry, guid));
                return;
            }
            break;
        }

        case ACT_CRITTER_NAME:
        {
            if (!code || !*code)
                break;                      // the box was closed empty

            // READ BEFORE, or the interesting half of the line is the half
            // that can no longer be written -- the same reason `del` reads the
            // label before removing the row.
            std::string const was = CritterShortName(guid);

            if (!NameCritter(player, guid, code, error))
                ChatHandler(player).SendSysMessage(error.c_str());
            else
                ChatHandler(player).PSendSysMessage("\"%s\" named to \"%s\".",
                                                    was.c_str(), CritterShortName(guid).c_str());

            // Redrawn, because the row's own label has just changed from
            // "Give it a name" to "Call it something else".
            if (HouseCritter const* now = GetCritter(guid))
            {
                SendCritterMenu(player, *now, ObjectGuid(HIGHGUID_UNIT, now->entry, guid));
                return;
            }
            break;
        }

        case ACT_CRITTER_PICKUP:
        {
            uint32 returned = 0;
            if (!RemoveCritter(player, guid, error, &returned))
                ChatHandler(player).SendSysMessage(error.c_str());
            else if (returned)
                ChatHandler(player).PSendSysMessage("Boxed up %s. Right-click to let it out again.", label.c_str());
            else
                ChatHandler(player).PSendSysMessage("%s wanders off.", label.c_str());
            break;
        }

        case ACT_CRITTER_CLOSE:
        default:
            break;
    }

    player->PlayerTalkClass->CloseGossip();
}

// What "let it wander" gives back: the crate's own radius if it came from one,
// otherwise housing's default. Reading the crate means a penguin goes back to
// two yards rather than to whatever number happened to be a constant.
float HouseMgr::DefaultWanderFor(HouseCritter const& c) const
{
    if (c.itemEntry)
    {
        std::map<uint32, CritterItem>::const_iterator itr = m_critterItems.find(c.itemEntry);
        if (itr != m_critterItems.end() && itr->second.wander > 0.0f)
            return itr->second.wander;
    }
    return HOUSE_CRITTER_WANDER;
}

//== commands ================================================================

bool ComfyCommands::HandleHouseCritterAddCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    uint32 entry;
    if (!handler->ExtractUint32KeyFromLink(&args, "Hcreature_entry", entry) || !entry)
    {
        handler->SendSysMessage("Usage: .house critter add <creature entry> [wander yards]");
        handler->SetSentErrorMessage(true);
        return false;
    }

    float wander = HOUSE_CRITTER_WANDER;
    handler->ExtractFloat(&args, wander);

    std::string error;
    if (!sHouseMgr.PlaceCritter(player, entry, wander, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    handler->PSendSysMessage("%s moves in.", HouseMgr::CritterName(entry).c_str());
    return true;
}

// The shared lookup for del / drag / wander.
//
// IT TAKES THE PARSED NUMBER RATHER THAN THE ARGUMENT STRING, because
// ChatHandler::ExtractOptUInt32 and SetSentErrorMessage are both PROTECTED --
// a free helper cannot reach them. So the parsing and the error flag stay in
// the handlers, where they are one line each, and only the question worth
// sharing comes here: which animal did they mean.
static uint32 CritterTarget(Player* player, uint32 typed, std::string& error)
{
    uint32 const guid = sHouseMgr.FindCritter(player, typed);
    if (!guid)
        error = typed ? "There is no animal with that number here."
                      : "No animal nearby. Stand next to one, or give its number.";
    return guid;
}

bool ComfyCommands::HandleHouseCritterDelCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    uint32 typed = 0;
    handler->ExtractOptUInt32(&args, typed, 0);

    std::string error;
    uint32 const guid = CritterTarget(player, typed, error);
    if (!guid)
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    // Read the name BEFORE the row goes, or the one message that matters --
    // WHAT you just sent away -- is the one that cannot be written.
    std::string const label = HouseMgr::CritterLabel(guid);

    uint32 returned = 0;
    if (!sHouseMgr.RemoveCritter(player, guid, error, &returned))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    // One command, two endings, and the difference is worth saying: an animal
    // that came out of a crate goes back into one, and everything else is gone
    // for good.
    if (returned)
        handler->PSendSysMessage("Boxed up %s. Right-click to let it out again.", label.c_str());
    else
        handler->PSendSysMessage("%s wanders off.", label.c_str());
    return true;
}

bool ComfyCommands::HandleHouseCritterDragCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    uint32 typed = 0;
    handler->ExtractOptUInt32(&args, typed, 0);

    std::string error;
    uint32 const guid = CritterTarget(player, typed, error);
    if (!guid)
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (!sHouseMgr.MoveCritter(player, guid, true, false, 0.0f, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    handler->PSendSysMessage("%s comes over.", HouseMgr::CritterLabel(guid).c_str());
    return true;
}

bool ComfyCommands::HandleHouseCritterWanderCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    float yards;
    if (!handler->ExtractFloat(&args, yards))
    {
        handler->PSendSysMessage("Usage: .house critter wander <0-%u yards> [number]",
                        uint32(HOUSE_CRITTER_WANDER_MAX));
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 typed = 0;
    handler->ExtractOptUInt32(&args, typed, 0);

    std::string error;
    uint32 const guid = CritterTarget(player, typed, error);
    if (!guid)
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (!sHouseMgr.MoveCritter(player, guid, false, true, yards, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (yards <= 0.0f)
        handler->PSendSysMessage("%s stays put.", HouseMgr::CritterLabel(guid).c_str());
    else
        handler->PSendSysMessage("%s roams %.0f yards.", HouseMgr::CritterLabel(guid).c_str(), yards);
    return true;
}

// DEV, live-tuned like `.house object marker` and `.house entrance model`: the
// heart search narrowed it to six SpellVisualKits and which of them looks right
// is the client's business, not the DBC's. Nothing is saved -- once the answer
// is known it belongs in HOUSE_CRITTER_HEART_VISUAL.
//
// A KIT ID, NOT A SpellVisual ID. See the note on HOUSE_CRITTER_HEART_VISUAL:
// the first build passed SpellVisual ids here and drew nothing whatsoever,
// which is the failure mode to expect from a wrong number in this packet.
// There is no error and no log line -- the client is simply handed a kit index
// that means nothing to it.
bool ComfyCommands::HandleHouseCritterVisualCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    uint32 visualId;
    if (!handler->ExtractUInt32(&args, visualId))
    {
        handler->SendSysMessage("Usage: .house critter visual <SpellVisualKit id> [number]");
        handler->SendSysMessage("Heart candidates: 6512 6517 6550 6612 (impact), 6549 6552 (state)");
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 typed = 0;
    handler->ExtractOptUInt32(&args, typed, 0);

    std::string error;
    uint32 const guid = CritterTarget(player, typed, error);
    if (!guid)
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    sHouseMgr.ShowCritterVisual(player, guid, visualId);
    handler->PSendSysMessage("Visual %u on %s.", visualId, HouseMgr::CritterLabel(guid).c_str());
    return true;
}

// DEV. Authoring a crate is rows in house_critter_item plus rows in
// item_template, and neither should cost a ~50s restart -- exactly the bargain
// `.house furniture reload` makes next door. Console-safe, so
// `.\Reload-Server.ps1 -Command 'house critter reload'` works with nobody
// logged in.
bool ComfyCommands::HandleHouseCritterReloadCommand(ChatHandler* handler, char* /*args*/)
{
    sHouseMgr.LoadCritterItems();
    handler->PSendSysMessage("Critter crates reloaded: %u.", sHouseMgr.CritterItemCount());
    return true;
}

// THE WHOLE TAIL IS THE NAME, so there is no room for a trailing id -- which
// is why this one command takes the nearest animal and nothing else. The menu
// is the targeted route: it already knows which one you clicked, and its box
// has the same job. Two ways in, each good at the half the other cannot do.
bool ComfyCommands::HandleHouseCritterNameCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    if (!args || !*args)
    {
        handler->SendSysMessage("Usage: .house critter name <name>   (the nearest animal)");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string error;
    uint32 const guid = CritterTarget(player, 0, error);
    if (!guid)
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    // Read before renaming; see the menu route for why.
    std::string const was = HouseMgr::CritterShortName(guid);

    if (!sHouseMgr.NameCritter(player, guid, args, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    handler->PSendSysMessage("\"%s\" named to \"%s\".", was.c_str(),
                    HouseMgr::CritterShortName(guid).c_str());
    return true;
}

bool ComfyCommands::HandleHouseCritterListCommand(ChatHandler* handler, char* /*args*/)
{
    Player* player = handler->m_session->GetPlayer();

    House* house = sHouseMgr.GetHouseAt(player);
    if (!house)
    {
        handler->SendSysMessage("You are not in a house.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::vector<HouseCritter const*> critters = sHouseMgr.GetCritters(house->id);
    if (critters.empty())
    {
        handler->SendSysMessage("No animals here.");
        return true;
    }

    for (HouseCritter const* c : critters)
    {
        std::ostringstream line;
        line << c->slot << ". " << HouseMgr::CritterName(c->entry)
             << " | " << uint32(player->GetDistance(c->x, c->y, c->z) + 0.5f) << " yd";

        if (c->wander > 0.0f)
            line << " | roams " << uint32(c->wander + 0.5f);
        else
            line << " | stays put";

        handler->SendSysMessage(line.str().c_str());
    }

    handler->PSendSysMessage("%u of %u.", uint32(critters.size()), uint32(HOUSE_MAX_CRITTERS));
    return true;
}
