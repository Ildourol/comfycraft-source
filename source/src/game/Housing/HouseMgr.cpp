#include "Housing/HouseMgr.h"

#include "GuidObjectScaling.h"

#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "World.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Item.h"
#include "GameObject.h"
#include "Map.h"
#include "MapManager.h"
#include "MapPersistentStateMgr.h"
#include "InstanceData.h"
#include "ScriptMgr.h"
#include "Chat.h"
#include "Group.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>
#include <utility>

HouseMgr sHouseMgr;

//== loading =================================================================

void HouseMgr::LoadFromDB()
{
    m_enabled = false;

    MapEntry const* mapEntry = sMapStorage.LookupEntry<MapEntry>(HOUSE_MAP_ID);
    if (!mapEntry || !mapEntry->IsDungeon())
    {
        sLog.outError("Housing: map %u is not an instanceable map. Run sql/custom/029_house_map_28.sql and restart. Housing disabled.", HOUSE_MAP_ID);
        return;
    }

    // Refuse to run rather than let furniture guids collide with temporary
    // summons. See the guid map in HouseMgr.h -- if the reserve was left at its
    // shipped 1000, the temporary floor sits below our block and the collision
    // would show up as furniture vanishing at random, days later.
    uint32 firstTemp = sObjectMgr.GetFirstTemporaryGameObjectLowGuid();
    if (firstTemp <= HOUSE_GO_GUID_MAX)
    {
        sLog.outError("Housing: temporary gameobject guids start at %u, inside the house block %u-%u. "
                      "Raise GuidReserveSize.GameObject in mangosd.conf to at least %u and restart. Housing disabled.",
                      firstTemp, HOUSE_GO_GUID_MIN, HOUSE_GO_GUID_MAX,
                      HOUSE_GO_GUID_MAX - firstTemp + sWorld.getConfig(CONFIG_UINT32_GUID_RESERVE_SIZE_GAMEOBJECT) + 1);
        return;
    }

    m_houses.clear();
    m_houseByAccount.clear();
    m_houseByInstance.clear();
    m_objects.clear();
    m_nextObjectGuid = HOUSE_GO_GUID_MIN;

    if (QueryResult* result = CharacterDatabase.PQuery(
            "SELECT id, account, instance_id, name, exit_set, exit_x, exit_y, exit_z, exit_o, portal_id, template_id "
            "FROM house WHERE map = %u", HOUSE_MAP_ID))
    {
        do
        {
            Field* f = result->Fetch();
            House h;
            h.id         = f[0].GetUInt32();
            h.accountId  = f[1].GetUInt32();
            h.instanceId = f[2].GetUInt32();
            h.name       = f[3].GetCppString();
            h.exitSet    = f[4].GetUInt32() != 0;
            h.exitX      = f[5].GetFloat();
            h.exitY      = f[6].GetFloat();
            h.exitZ      = f[7].GetFloat();
            h.exitO      = f[8].GetFloat();
            h.portalId   = f[9].GetUInt32();
            h.templateId = f[10].GetUInt32();

            m_houses[h.id] = h;
            m_houseByAccount[h.accountId] = h.id;
        }
        while (result->NextRow());
        delete result;
    }

    uint32 orphans = 0;
    // stowed = 0: rows stowed into the furniture chest stay in the table but
    // never load, which is what makes stowing a flip rather than a delete.
    if (QueryResult* result = CharacterDatabase.Query(
            "SELECT id, house_id, go_entry, x, y, z, o, rot0, rot1, rot2, rot3, scale, source, slot, item_entry, group_no FROM house_object WHERE stowed = 0 ORDER BY id"))
    {
        do
        {
            Field* f = result->Fetch();
            HouseObject o;
            o.guid    = f[0].GetUInt32();
            o.houseId = f[1].GetUInt32();
            o.goEntry = f[2].GetUInt32();
            o.x = f[3].GetFloat(); o.y = f[4].GetFloat();
            o.z = f[5].GetFloat(); o.o = f[6].GetFloat();
            o.rot0 = f[7].GetFloat(); o.rot1 = f[8].GetFloat();
            o.rot2 = f[9].GetFloat(); o.rot3 = f[10].GetFloat();
            o.scale = f[11].GetFloat();
            o.source = uint8(f[12].GetUInt32());
            o.slot   = f[13].GetUInt32();
            o.itemEntry = f[14].GetUInt32();
            o.groupNo   = f[15].GetUInt32();

            // house_object is the authority, so push it back into the core's
            // registry rather than trusting tw_world.object_scaling to agree.
            if (o.scale > 0.0f)
                sGuidObjectScaling.AddOrEdit(
                    ObjectGuid(HIGHGUID_GAMEOBJECT, o.goEntry, o.guid).GetRawValue(), o.scale);

            House* house = GetHouseById(o.houseId);
            if (!house)
            {
                ++orphans;
                continue;
            }
            if (!sObjectMgr.GetGameObjectInfo(o.goEntry))
            {
                sLog.outErrorDb("Housing: house_object %u references gameobject_template %u, which does not exist. Skipped.", o.guid, o.goEntry);
                continue;
            }
            // Never let a bad row reach the grid loader: Relocate asserts on an
            // invalid coordinate and that abort takes the whole server down.
            if (!MaNGOS::IsValidMapCoord(o.x, o.y, o.z, o.o))
            {
                sLog.outErrorDb("Housing: house_object %u in house %u is at (%f, %f, %f), which is off the map. Skipped; fix or delete the row.", o.guid, o.houseId, o.x, o.y, o.z);
                continue;
            }

            m_objects[o.guid] = o;
            house->objects.push_back(o.guid);
            if (o.guid >= m_nextObjectGuid)
                m_nextObjectGuid = o.guid + 1;
        }
        while (result->NextRow());
        delete result;
    }

    if (orphans)
        sLog.outErrorDb("Housing: %u house_object rows point at a house that no longer exists.", orphans);

    // The selection glow's size, into the cached template. It cannot live in
    // SQL: 2000836 is a stock Turtle row, so an edit there would be reverted by
    // the next world update AND would resize the model everywhere else it is
    // used. This is the same in-memory template patch SetSelectMarkLook makes, and
    // for the same reason -- there is no `reload gameobject_template`.
    SetSelectMarkLook(0, HOUSE_SELECT_MARK_SCALE, HOUSE_SELECT_MARK_LIFT);

    // NUMBER ANYTHING THAT IS NOT NUMBERED, and write it back. sql/custom/040
    // adds the column and deliberately does NOT backfill it, because doing it
    // here covers the migration and every later way a row can arrive without a
    // slot -- a hand-written INSERT, a restore from a dump taken before the
    // column existed, a future migration that forgets. A row with no slot is a
    // row no command can name, so healing beats warning.
    //
    // In guid order, which is creation order, so the numbers come out in the
    // order the objects were actually placed.
    {
        uint32 numbered = 0;
        for (auto& itr : m_houses)
        {
            House& house = itr.second;
            for (uint32 guid : house.objects)
            {
                HouseObject& o = m_objects[guid];
                if (o.slot)
                    continue;

                o.slot = AllocateSlot(house);
                CharacterDatabase.PExecute("UPDATE house_object SET slot = %u WHERE id = %u", o.slot, o.guid);
                ++numbered;
            }
        }
        if (numbered)
            sLog.outString(">> Housing: numbered %u object(s) that had no slot.", numbered);
    }

    // The guid counter must clear EVERY row, not just the loaded ones -- a
    // stowed row (skipped above) still owns its primary key, and handing its
    // id to a fresh placement would make the INSERT fail, which on this server
    // is a hard crash rather than a warning.
    if (QueryResult* result = CharacterDatabase.Query("SELECT MAX(id) FROM house_object"))
    {
        uint32 const maxId = result->Fetch()[0].GetUInt32();
        if (maxId >= m_nextObjectGuid)
            m_nextObjectGuid = maxId + 1;
        delete result;
    }

    // The instance ids we just read are only a cache, and PackInstances() can
    // renumber them out from under us. Drop any that no longer name a row on
    // this map; the next visit allocates a fresh one and loads the same
    // furniture, because house_object is keyed by house id.
    for (auto& itr : m_houses)
    {
        House& h = itr.second;
        if (!h.instanceId)
            continue;

        QueryResult* check = CharacterDatabase.PQuery(
                "SELECT 1 FROM instance WHERE id = %u AND map = %u", h.instanceId, HOUSE_MAP_ID);
        if (!check)
        {
            sLog.outInfo("Housing: house %u had a stale instance id %u; it will be reissued on the next visit.", h.id, h.instanceId);
            h.instanceId = 0;
            CharacterDatabase.PExecute("UPDATE house SET instance_id = 0 WHERE id = %u", h.id);
            continue;
        }
        delete check;
        m_houseByInstance[h.instanceId] = h.id;
    }


    // Before LoadPortals, which validates each door's template_id against the
    // set loaded here.
    LoadTemplates();

    m_enabled = true;

    // After m_enabled, because SpawnPortal refuses to do anything without it.
    // Safe this early: World.cpp runs us long after LoadGameobjects, and
    // SpawnPortal only registers the spawn -- FindMap returns nothing yet, so
    // the object itself appears when its grid first loads, like any other.
    LoadPortals();
    SpawnPortals();

    LoadFurnitureItems();
    LoadCritterItems();
    LoadStorage();
    LoadSettings();

    // After the houses, because every critter row has to find its house. Its
    // own guid-block check inside, which can disable critters alone and leaves
    // the rest of housing running.
    LoadCritters();

    // Claimed and template houses counted apart -- a template IS a house row
    // (synthetic account), and "4 houses" with two players logged in reads as
    // a leak until you know that.
    uint32 templateHouses = 0;
    for (auto const& itr : m_houses)
        if (itr.second.authorsTemplate)
            ++templateHouses;
    sLog.outString(">> Housing: %u houses (+%u template), %u placed objects, next guid %u",
                   uint32(m_houses.size()) - templateHouses, templateHouses, uint32(m_objects.size()), m_nextObjectGuid);
}

//== furniture items =========================================================

// tw_world.house_furniture_item: the item a player right-clicks -> the model it
// becomes. Content, not code -- adding a piece of furniture later is one row
// here and one in item_template, and `.house furniture reload` picks it up
// without a restart.
//
// A MISSING TABLE IS NOT FATAL, deliberately. Every other housing table is
// load-bearing and HouseMgr refuses to enable itself without them; this one
// only decides whether a category of item does anything, so a server that has
// not applied sql/custom/042 keeps every other housing feature and simply has
// no furniture items. The count is logged either way, because "nothing
// happens when I right-click it" needs a startup line to check against.
void HouseMgr::LoadFurnitureItems()
{
    m_furnitureItems.clear();
    m_furnitureCategories.clear();

    // ORDER BY item_entry is load-bearing, not tidiness: the shop's tabs are
    // drawn in the order their categories first appear, so the row order in
    // sql/custom/042 IS the tab order and there is no sort column to keep in
    // step with it.
    QueryResult* result = WorldDatabase.Query(
        "SELECT item_entry, go_entry, scale, category FROM house_furniture_item ORDER BY item_entry");
    if (!result)
    {
        sLog.outString(">> Housing: no furniture items (house_furniture_item is empty or missing).");
        return;
    }

    uint32 skipped = 0;
    do
    {
        Field* f = result->Fetch();
        uint32 const itemEntry = f[0].GetUInt32();
        FurnitureItem fi;
        fi.goEntry  = f[1].GetUInt32();
        fi.scale    = f[2].GetFloat();
        fi.category = f[3].GetCppString();

        // Both ends checked at load, because the failure at USE time is a
        // player holding an item that does nothing and no error anywhere --
        // the same silent miss the collection tables have (see CLAUDE.md).
        if (!sObjectMgr.GetItemPrototype(itemEntry))
        {
            sLog.outErrorDb("Housing: house_furniture_item %u is not an item. Skipped.", itemEntry);
            ++skipped;
            continue;
        }
        if (!sObjectMgr.GetGameObjectInfo(fi.goEntry))
        {
            sLog.outErrorDb("Housing: house_furniture_item %u maps to gameobject_template %u, which does not exist. Skipped.",
                            itemEntry, fi.goEntry);
            ++skipped;
            continue;
        }

        // First appearance decides the tab order. A linear scan over a handful
        // of strings, once per row at load -- a set would order them
        // alphabetically, which is exactly what this is avoiding.
        if (!fi.category.empty()
            && std::find(m_furnitureCategories.begin(), m_furnitureCategories.end(),
                         fi.category) == m_furnitureCategories.end())
            m_furnitureCategories.push_back(fi.category);

        m_furnitureItems[itemEntry] = fi;
    }
    while (result->NextRow());
    delete result;

    sLog.outString(">> Housing: %u furniture item(s) in %u categor%s%s.",
                   uint32(m_furnitureItems.size()), uint32(m_furnitureCategories.size()),
                   m_furnitureCategories.size() == 1 ? "y" : "ies",
                   skipped ? ", some rows skipped -- see the errors above" : "");
}

// The whitelist SendListInventory takes for one page of the shop.
void HouseMgr::FurnitureItemsIn(std::string const& category, std::set<uint32>& out) const
{
    for (std::map<uint32, FurnitureItem>::const_iterator it = m_furnitureItems.begin();
         it != m_furnitureItems.end(); ++it)
        if (it->second.category == category)
            out.insert(it->first);
}

// Right-clicking a furniture item. Every rule this has to enforce is already
// enforced by PlaceObject: CheckOwner refuses outside your own house and while
// visiting somebody else's, and the hundred-object cap refuses a full one. So
// this resolves the model and gets out of the way -- and because the caller
// only consumes the item when this returns true, a refusal never costs
// anything.
bool HouseMgr::UseFurnitureItem(Player* player, uint32 itemEntry, std::string& error)
{
    auto itr = m_furnitureItems.find(itemEntry);
    if (itr == m_furnitureItems.end())
    {
        error = "That is not furniture.";
        return false;
    }

    // NO SPOT, EVER, AND THAT IS THE WHOLE OF CHALK-ONLY MODE. Passing null
    // sends PlaceObject to the chalk mark, and only to two yards ahead when
    // there is no mark -- so a crate is aimed by exactly the same thing that
    // aims `.house object add` and the addon, instead of being the only route
    // with an aiming mechanism of its own.
    //
    // A client on a stale WDB cache can still draw the old ground circle and
    // send a point. There is nothing here to read it, which is a better
    // guarantee than checking a setting: the two can no longer disagree,
    // because only one of them has an opinion.
    return PlaceObject(player, itr->second.goEntry, HOUSE_PLACE_DISTANCE, error,
                       itemEntry, itr->second.scale, nullptr);
}

//== per-account settings ====================================================
//
// Two choices with no right answer, held by account so an alt walks into the
// same room set up the same way. See the HouseSetting enum for what each one
// does; this end is deliberately generic, so adding a third is one row in the
// table below and one line of SQL comment -- no schema change, no backfill.

namespace
{
    // The whole definition of a setting. ONE PLACE, because the chat command
    // and the panel on the exit gear both describe the same state and a second
    // copy of these words would drift within a week.
    //
    // TWO NAMES, AND THEY ARE NOT THE SAME JOB. `name` is what you TYPE, so it
    // is lowercase, has no spaces, and -- most of all -- SAYS WHAT ON MEANS.
    // `point_and_click_placing on` needs nothing else to be understood, where
    // `placing on` (what it was called for one build) leaves the reader to
    // guess what the other state even is. `title` is the same setting written
    // for a menu row, where the underscores would just be noise.
    //
    // ORDER MATTERS -- indexed by HouseSetting.
    struct HouseSettingInfo
    {
        char const* name;                               // what you type
        char const* title;                              // what a menu row calls it
        uint32      defValue;
        bool        global;                             // the server's, not the player's

        // A SETTING IS BOOLEAN WHEN ITS RANGE IS 0..1, and numeric otherwise --
        // one field rather than a `numeric` flag that could disagree with the
        // bounds beside it. Everything player-facing is 0..1 and should stay
        // that way; the range exists for the server's own knobs, where the
        // value carries a real quantity and clamping is the only guard between
        // a typo and a feature that quietly stops working.
        uint32      minValue;
        uint32      maxValue;
    };

    // SCOPE IS A PROPERTY OF THE SETTING, not of the system: the glow is
    // genuinely per-player and costs nothing to vary, where a setting that
    // rewrote a shared item_template column could only ever be the server's.
    // Nothing global is left, but the column stays -- the rule was the useful
    // part, not the one setting that needed it.
    //
    // A NULL NAME MEANS RETIRED, AND THE ROW STAYS. Slot 0 was
    // `point_and_click_placing`; it is a hole rather than a deletion because
    // house_setting rows are keyed by this index and shuffling the table would
    // silently re-point everybody's saved preferences at their neighbours.
    // IsSettingLive is the only thing that knows about the hole.
    HouseSettingInfo const HOUSE_SETTINGS[HOUSE_SETTING_MAX] =
    {
        { nullptr,                   nullptr,                   1, false, 0,   1     },
        { "glow",                    "Selection glow",          1, false, 0,   1     },
        { "walk_through_portals",    "Walk through portals",    1, false, 0,   1     },
        { "arrival_ping_ms",         "Arrival ping delay",   3000, true,  500, 30000 },
        { "locked",                  "Door locked",             0, false, 0,   1     },
    };

    bool SettingIsBoolean(HouseSetting setting)
    {
        return setting >= HOUSE_SETTING_MAX || HOUSE_SETTINGS[setting].maxValue <= 1;
    }

    bool WordIsPrefixOf(std::string const& word, char const* of)
    {
        return !word.empty() && strlen(of) >= word.size() &&
               !strncmp(word.c_str(), of, word.size());
    }
}

// A retired slot has a null name, so every accessor answers "" for it rather
// than handing a null to something that will call strlen on it.
bool HouseMgr::IsSettingLive(HouseSetting setting)
{
    return setting < HOUSE_SETTING_MAX && HOUSE_SETTINGS[setting].name != nullptr;
}

char const* HouseMgr::SettingName(HouseSetting setting)
{
    return IsSettingLive(setting) ? HOUSE_SETTINGS[setting].name : "";
}

char const* HouseMgr::SettingTitle(HouseSetting setting)
{
    return IsSettingLive(setting) ? HOUSE_SETTINGS[setting].title : "";
}

// EVERY SETTING IS ON OR OFF AND SAYS SO IN THOSE WORDS. It used to answer with
// a phrase per state -- "point and click" against "in front of you" -- which
// described the behaviour beautifully and left the player with no idea what to
// type. The name carries the meaning now, so the value only has to carry the
// state.
// ...UNLESS IT CARRIES A QUANTITY, in which case the number IS the state and
// "on" would be throwing the only interesting part away. The rule above is
// unchanged for everything a player can reach.
//
// The ping's label says what it ROUNDED TO as well as what was typed, because
// it is quantised to the instance throttle and somebody who sets 2900 and is
// told "2900" will not understand why it behaves like 3000.
std::string HouseMgr::SettingLabel(HouseSetting setting, uint32 value)
{
    if (SettingIsBoolean(setting))
        return value ? "on" : "off";

    if (setting == HOUSE_SETTING_ARRIVE_PING)
    {
        uint32 const real = ArrivePingTicks(value) * HOUSE_EDIT_INTERVAL;
        if (real != value)
            return std::to_string(value) + "ms (" + std::to_string(real) + "ms in practice)";
    }

    return std::to_string(value) + "ms";
}

// What turning it OFF gets you, in one line. Only the off state needs this:
// the name already says what on does, which is the whole point of naming them
// this way.
std::string HouseMgr::SettingHint(HouseSetting setting)
{
    switch (setting)
    {
        case HOUSE_SETTING_GLOW:
            return "off: no shimmer on the object you have selected";

        case HOUSE_SETTING_WALK_IN:
            return "off: use the gear's Go home or Leave instead";

        case HOUSE_SETTING_LOCKED:
            return "off: your party walks straight in";

        // Not an "off:" line, because there is no off. What this one needs to
        // say is which way is safe to be wrong: too low and people standing in
        // a building are invisible to whoever walks in, intermittently, and it
        // presents as the fix never having worked.
        case HOUSE_SETTING_ARRIVE_PING:
            return "raise it if arrivals cannot see who is already indoors";

        default:
            return "";
    }
}

// Nothing left needs one. It existed for `point_and_click_placing`, whose
// server half took effect at once while its client half waited on a WDB cache
// clear -- so the page had to say so in both directions. Kept rather than
// deleted because the NEXT setting with a delay in it will want exactly this,
// and an empty string is what every setting without one already returned.
char const* HouseMgr::SettingNote(HouseSetting /*setting*/, uint32 /*value*/)
{
    return "";
}

bool HouseMgr::SettingIsGlobal(HouseSetting setting)
{
    return setting < HOUSE_SETTING_MAX && HOUSE_SETTINGS[setting].global;
}

bool HouseMgr::SettingFromWord(std::string const& word, HouseSetting& out)
{
    for (uint32 i = 0; i < HOUSE_SETTING_MAX; ++i)
    {
        // A retired slot has no name to match, and WordIsPrefixOf would call
        // strlen on a null pointer trying.
        if (!IsSettingLive(HouseSetting(i)))
            continue;

        if (WordIsPrefixOf(word, HOUSE_SETTINGS[i].name))
        {
            out = HouseSetting(i);
            return true;
        }
    }
    return false;
}

bool HouseMgr::SettingValueFromWord(HouseSetting setting, std::string const& word, uint32& out)
{
    if (setting >= HOUSE_SETTING_MAX)
        return false;

    // ON AND OFF AND NOTHING ELSE, for every setting there will ever be here.
    // There used to be a friendlier pair per setting -- `point` and `ahead` for
    // the placing one -- and they were a kindness that cost more than it gave:
    // two vocabularies to learn instead of one, for a word you would have to
    // read the source to discover. yes/no and 1/0 stay because nobody has to be
    // told about them.
    //
    // A NUMERIC SETTING TAKES A NUMBER AND NOTHING ELSE -- not "on", which
    // would have to invent a quantity. It is REFUSED rather than clamped when
    // it is out of range: silently turning 50 into 500 is how somebody comes
    // away believing they set a value they did not.
    if (!SettingIsBoolean(setting))
    {
        if (word.empty() || word.find_first_not_of("0123456789") != std::string::npos)
            return false;

        uint32 const n = uint32(atoi(word.c_str()));
        if (n < HOUSE_SETTINGS[setting].minValue || n > HOUSE_SETTINGS[setting].maxValue)
            return false;

        out = n;
        return true;
    }

    if (word == "on" || word == "yes" || word == "1")
    {
        out = 1;
        return true;
    }
    if (word == "off" || word == "no" || word == "0")
    {
        out = 0;
        return true;
    }
    return false;
}

// Milliseconds in, instance ticks out, ROUNDED UP and never zero: the ping
// rides the instance script's throttle, so any value between two ticks can only
// be served by the later one -- and rounding down would quietly make the
// smallest settings mean "immediately", which is the one value measured not to
// work at all.
uint32 HouseMgr::ArrivePingTicks(uint32 ms)
{
    uint32 const ticks = (ms + HOUSE_EDIT_INTERVAL - 1) / HOUSE_EDIT_INTERVAL;
    return ticks ? ticks : 1;
}

// Loaded whole at startup: a row is three small integers, most accounts have
// none, and the alternative is a query on a path that runs on every placement.
void HouseMgr::LoadSettings()
{
    m_settings.clear();

    QueryResult* result = CharacterDatabase.Query(
        "SELECT account, setting, value FROM house_setting");
    if (!result)
    {
        sLog.outString(">> Housing: no settings changed from their defaults.");
        return;
    }

    uint32 rows = 0, dropped = 0;
    do
    {
        Field* f = result->Fetch();
        uint32 const setting = f[1].GetUInt32();

        // A SETTING THIS BINARY DOES NOT KNOW IS IGNORED, NOT AN ERROR. Rolling
        // a binary back past a setting that has been used is otherwise a
        // startup failure over a preference, which is the wrong price.
        if (setting >= HOUSE_SETTING_MAX)
        {
            ++dropped;
            continue;
        }

        m_settings[f[0].GetUInt32()][setting] = f[2].GetUInt32();
        ++rows;
    }
    while (result->NextRow());
    delete result;

    if (dropped)
        sLog.outString("Housing: %u house_setting row(s) this build has no setting for were ignored.",
                       dropped);

    sLog.outString(">> Housing: %u setting(s) across %u account(s).",
                   rows, uint32(m_settings.size()));

}

uint32 HouseMgr::GetSetting(uint32 accountId, HouseSetting setting) const
{
    if (setting >= HOUSE_SETTING_MAX)
        return 0;

    auto acc = m_settings.find(accountId);
    if (acc != m_settings.end())
    {
        auto row = acc->second.find(uint32(setting));
        if (row != acc->second.end())
            return row->second;
    }

    return HOUSE_SETTINGS[setting].defValue;
}

uint32 HouseMgr::GetSetting(Player* player, HouseSetting setting) const
{
    // A GLOBAL SETTING IGNORES WHO IS ASKING, which is what makes it safe to
    // read from paths that have no player at all -- and what stops a caller
    // accidentally giving one player a different answer from another.
    if (SettingIsGlobal(setting))
        return GetSetting(uint32(HOUSE_SETTING_GLOBAL_ACCOUNT), setting);

    if (!player || !player->GetSession())
        return setting < HOUSE_SETTING_MAX ? HOUSE_SETTINGS[setting].defValue : 0;

    return GetSetting(player->GetSession()->GetAccountId(), setting);
}

void HouseMgr::SetSetting(Player* player, HouseSetting setting, uint32 value)
{
    if (!player || !player->GetSession() || setting >= HOUSE_SETTING_MAX)
        return;

    uint32 const account = SettingIsGlobal(setting)
                         ? uint32(HOUSE_SETTING_GLOBAL_ACCOUNT)
                         : player->GetSession()->GetAccountId();

    m_settings[account][uint32(setting)] = value;
    CharacterDatabase.PExecute(
        "REPLACE INTO house_setting (account, setting, value) VALUES (%u, %u, %u)",
        account, uint32(setting), value);

    // APPLY IT NOW, not at the next opportunity. The glow is the one that shows
    // this up: switching it off has to take the shimmer down while the player is
    // looking at it, or the setting reads as having failed. Refresh is
    // idempotent and cheap, so it is fine to call for a setting it knows
    // nothing about.
    RefreshSelectionMarker(player);
}

//== furniture storage =======================================================

// Defined further down, beside the placement code it was written for. Declared
// here so storage can ask the same question in the same words: a rule copied is
// a rule that drifts.
static bool CheckOwner(HouseMgr& mgr, Player* player, House*& house, std::string& error);


// One account's crates. Loaded whole at startup -- a row is three integers and
// there will never be many -- and written through on every change, the way the
// houses themselves are.
void HouseMgr::LoadStorage()
{
    m_storage.clear();

    QueryResult* result = CharacterDatabase.Query(
        "SELECT account, slot, item_entry FROM house_storage ORDER BY account, slot");
    if (!result)
    {
        sLog.outString(">> Housing: furniture storage is empty.");
        return;
    }

    uint32 rows = 0, dropped = 0;
    do
    {
        Field* f = result->Fetch();
        uint32 const slot = f[1].GetUInt32();

        // Slot 0 is the one square that can never exist: `take 0` cannot be
        // typed distinctly from "no argument", so a crate there would be a
        // crate nothing could reach. Above the backstop is refused for the same
        // reason it exists at all. Both are only reachable by hand-editing.
        if (!slot || slot > HOUSE_STORAGE_MAX)
        {
            ++dropped;
            continue;
        }

        m_storage[f[0].GetUInt32()][slot] = f[2].GetUInt32();
        ++rows;
    }
    while (result->NextRow());
    delete result;

    if (dropped)
        sLog.outErrorDb("Housing: %u house_storage row(s) outside squares 1-%u were ignored.",
                        dropped, uint32(HOUSE_STORAGE_MAX));

    sLog.outString(">> Housing: %u stored crate(s) across %u account(s).",
                   rows, uint32(m_storage.size()));
}

// One row per kind, in square order. See the header for why the grouping lives
// here rather than in each of the three callers that wanted it.
std::vector<HouseStorageStack> HouseMgr::GetStorageStacks(Player* player) const
{
    std::vector<HouseStorageStack> out;
    if (!player || !player->GetSession())
        return out;

    auto itr = m_storage.find(player->GetSession()->GetAccountId());
    if (itr == m_storage.end())
        return out;

    // m_storage's inner map is keyed by slot and therefore already in square
    // order, so the FIRST time a kind is seen is its lowest square and the
    // list comes out ordered by where each kind starts. No sort needed, and
    // the order is stable across calls -- which matters, because the window
    // redraws from this on every change.
    for (auto const& row : itr->second)
    {
        if (!row.second)
            continue;

        bool merged = false;
        for (size_t i = 0; i < out.size() && !merged; ++i)
            if (out[i].itemEntry == row.second)
            {
                ++out[i].count;
                merged = true;
            }

        if (!merged)
        {
            HouseStorageStack s;
            s.slot = row.first;
            s.itemEntry = row.second;
            s.count = 1;
            out.push_back(s);
        }
    }
    return out;
}

uint32 HouseMgr::GetStorageCount(Player* player) const
{
    if (!player || !player->GetSession())
        return 0;

    auto itr = m_storage.find(player->GetSession()->GetAccountId());
    if (itr == m_storage.end())
        return 0;

    uint32 n = 0;
    for (auto const& row : itr->second)
        if (row.second)
            ++n;
    return n;
}

uint32 HouseMgr::GetStorageAt(Player* player, uint32 slot) const
{
    if (!player || !player->GetSession() || !slot)
        return 0;

    auto itr = m_storage.find(player->GetSession()->GetAccountId());
    if (itr == m_storage.end())
        return 0;

    auto row = itr->second.find(slot);
    return row == itr->second.end() ? 0 : row->second;
}

// Standing at your own front door. See the header for why any matching door
// counts and why the hide range is the one used.
bool HouseMgr::AtOwnHomeDoor(Player* player)
{
    if (!m_enabled || !player || !player->GetSession())
        return false;

    House const* house = GetHouseByAccount(player->GetSession()->GetAccountId());
    if (!house)
        return false;

    float const reach = m_markHide;

    // A LINEAR SCAN, AND IT SHOULD STAY ONE. There is no index from a position
    // to a portal, there are a handful of doors in the world, and this is asked
    // once per storage action rather than per tick -- so the map that would
    // make it faster would cost more to keep true than the scan costs to run.
    for (auto const& itr : m_portals)
    {
        HousePortal const& door = itr.second;
        if (door.map != player->GetMapId())
            continue;
        if (!IsHomePortal(*house, door))
            continue;
        if (player->IsWithinDist3d(door.x, door.y, door.z, reach))
            return true;
    }
    return false;
}

// The one gate every storage entry point goes through, and the reason there is
// no second rule to drift.
bool HouseMgr::StorageAllowed(Player* player, std::string& error)
{
    // The door first, because it is the cheap answer for somebody who is NOT in
    // a house -- and because CheckOwner's own error ("You are not in a house.")
    // is the wrong thing to say to a player standing at their own front door.
    if (AtOwnHomeDoor(player))
        return true;

    House* house = nullptr;
    if (CheckOwner(*this, player, house, error))
        return true;

    // STANDING IN SOMEBODY ELSE'S HOUSE keeps CheckOwner's own answer. "This is
    // not your house." is both true and more useful than anything below, and a
    // guest being told where their storage is instead of why this one is shut
    // would read as the server having misunderstood the question.
    if (house)
        return false;

    // Everywhere else gets an error that names BOTH places. Falling through
    // with "You are not in a house." would send a player standing at their own
    // front door -- which now works -- confidently indoors to look for it.
    error = "Your furniture storage is inside your house, or at your front door.";
    return false;
}

bool HouseMgr::StorageDeposit(Player* player, uint32 itemEntry, uint32 slot, std::string& error)
{
    if (!StorageAllowed(player, error))
        return false;

    // Furniture AND critter crates -- see IsStorableItem for why it cannot be
    // only the former. Refused by name rather than silently ignored: the addon
    // only ever offers things that fit, so anybody seeing this typed the
    // command and deserves to know which half of it was wrong.
    if (!IsStorableItem(itemEntry))
    {
        error = "Only furniture and pets go in there.";
        return false;
    }

    if (!player->GetItemCount(itemEntry, false))
    {
        error = "You are not carrying that.";
        return false;
    }

    uint32 const accountId = player->GetSession()->GetAccountId();
    std::map<uint32, uint32>& shelf = m_storage[accountId];

    if (slot > HOUSE_STORAGE_MAX)
    {
        error = "There is no such square.";
        return false;
    }

    if (!slot)
    {
        // THE LOWEST FREE SQUARE, and it is a scan over the ROWS rather than
        // over a fixed range: the shelf has no size any more, so "1 to forty"
        // has nothing to count to. Reusing the lowest gap rather than always
        // appending is what stops square numbers climbing forever as crates go
        // in and out -- they are the identity `take` uses, and an identity that
        // grows without bound for no reason is one that eventually needs
        // explaining.
        slot = 1;
        while (slot <= HOUSE_STORAGE_MAX && shelf.find(slot) != shelf.end())
            ++slot;

        // The fuse, not a shelf size. Reaching this means either a very
        // determined collector or a bug, and the message says the true thing
        // rather than inventing a limit the player was never shown.
        if (slot > HOUSE_STORAGE_MAX)
        {
            error = "Your furniture storage cannot hold any more.";
            return false;
        }
    }
    else if (shelf.find(slot) != shelf.end())
    {
        // REFUSED, NOT SWAPPED. A swap would have to hand the displaced crate
        // back, and there is no way to put an item on the cursor from here --
        // so it would have to go to the bags, which may be full, in the middle
        // of an action the player thinks is a rearrangement.
        error = "There is already something in that square.";
        return false;
    }

    uint32 count = 1;
    player->DestroyItemCount(itemEntry, count, true);

    shelf[slot] = itemEntry;
    CharacterDatabase.PExecute(
        "REPLACE INTO house_storage (account, slot, item_entry) VALUES (%u, %u, %u)",
        accountId, slot, itemEntry);
    return true;
}

// Every crate in the bags, in one go. It asks the FURNITURE TABLE what to look
// for rather than walking the bags: there are a dozen kinds and forty squares,
// GetItemCount is the core's own answer to "how many of these do I have", and a
// bag walk would have to know about bank slots, equipped items and bags inside
// bags to be right.
bool HouseMgr::StorageDepositAll(Player* player, uint32& moved, std::string& error)
{
    moved = 0;
    if (!StorageAllowed(player, error))
        return false;

    // BOTH CATALOGUES, gathered first so the deposit loop has one list to walk.
    // "Put it all away" meaning "all the chairs, and you can go back for the
    // rabbits" is the kind of half-answer that gets reported as a bug.
    std::vector<uint32> entries;
    entries.reserve(m_furnitureItems.size() + m_critterItems.size());
    for (auto const& fi : m_furnitureItems)
        entries.push_back(fi.first);
    for (auto const& ci : m_critterItems)
        entries.push_back(ci.first);

    bool full = false;
    for (uint32 entry : entries)
    {
        uint32 have = player->GetItemCount(entry, false);
        while (have)
        {
            std::string why;
            if (!StorageDeposit(player, entry, 0, why))
            {
                full = true;
                break;
            }
            ++moved;
            --have;
        }
        if (full)
            break;
    }

    if (!moved)
    {
        error = full ? "Your furniture storage is full."
                     : "You are not carrying anything that goes in there.";
        return false;
    }
    return true;
}

// What packing this house up would come to. Pure -- see the header for why the
// three answers are kept apart rather than summed.
void HouseMgr::CountStowable(House const& house, uint32& savable, uint32& noRoom, uint32& noItem) const
{
    savable = noRoom = noItem = 0;

    // How many squares are free RIGHT NOW. Counting against the live shelf
    // rather than against forty is what makes the preview honest for somebody
    // who already has thirty crates put away.
    // Room against the BACKSTOP, not against a shelf size -- so in ordinary
    // play `noRoom` now comes back 0 and the forecast is simply "this many go,
    // this many were never crates". The branch stays because the fuse is real:
    // a house at the hundred-object cap plus a shelf near the thousand-crate
    // ceiling can still overflow, and silently losing furniture there would be
    // the worst version of this feature.
    auto shelf = m_storage.find(house.accountId);
    size_t const used = (shelf == m_storage.end()) ? 0 : shelf->second.size();
    uint32 free = (used >= HOUSE_STORAGE_MAX) ? 0 : HOUSE_STORAGE_MAX - uint32(used);

    for (uint32 guid : house.objects)
    {
        HouseObject const* o = GetObject(guid);
        if (!o || IsHouseFabric(*o))
            continue;

        if (!o->itemEntry)
        {
            ++noItem;
            continue;
        }
        if (free)
        {
            --free;
            ++savable;
        }
        else
            ++noRoom;
    }

    // THE ANIMALS COUNT TOO, because a crate is a crate. Adding critter crates
    // without this line would have made `.house reset` quietly destroy a
    // fifteen-gold whelpling while the confirm text promised only furniture
    // would be packed -- the forecast has to describe what the purge will
    // actually do, or it is worse than no forecast at all.
    for (uint32 guid : house.critters)
    {
        std::map<uint32, HouseCritter>::const_iterator itr = m_critters.find(guid);
        if (itr == m_critters.end())
            continue;

        if (!itr->second.itemEntry)
        {
            ++noItem;
            continue;
        }
        if (free)
        {
            --free;
            ++savable;
        }
        else
            ++noRoom;
    }
}

// The forecast, for a confirm page. See the header for why both routes share it.
std::string HouseMgr::StowForecast(House const& house) const
{
    uint32 savable = 0, noRoom = 0, noItem = 0;
    CountStowable(house, savable, noRoom, noItem);

    if (!savable && !noRoom && !noItem)
        return std::string();

    std::ostringstream out;

    if (savable)
        out << savable << (savable == 1 ? " piece goes" : " pieces go") << " to your furniture storage";
    else
        out << "Nothing can go to your furniture storage";

    // THE TWO LOSSES ARE NAMED SEPARATELY because only one of them has a fix.
    // "The shelf is full" is something a player can go and do something about;
    // "these were never items" is a property of how they were placed and no
    // amount of tidying changes it. Rolling them into one number would hide the
    // actionable half behind the permanent half.
    if (noRoom)
        out << "; " << noRoom << " will not fit and " << (noRoom == 1 ? "is" : "are") << " lost";
    if (noItem)
        out << "; " << noItem << " came from the catalogue rather than a crate and cannot be packed";

    out << '.';
    return out.str();
}

// The rescue, run just before a house is torn down.
//
// NOT StorageDeposit, THOUGH IT LOOKS LIKE THE SAME JOB. That one starts by
// checking the item is in the player's bags and destroying it, which is exactly
// right for a crate being put away by hand and exactly wrong here: this
// furniture is standing in the room, not carried, and the player may not even
// be online. So the shelf is written directly, and the object is left for the
// caller's purge to remove.
void HouseMgr::StowHouseFurniture(House const& house, uint32& stowed, uint32& lost)
{
    stowed = lost = 0;

    std::map<uint32, uint32>& shelf = m_storage[house.accountId];

    // The next free square, walked forward across the whole run rather than
    // rediscovered per object: forty squares and a hundred objects is a
    // thousand map lookups otherwise, and the answer only ever moves one way.
    uint32 next = 1;

    for (uint32 guid : house.objects)
    {
        HouseObject const* o = GetObject(guid);
        if (!o || IsHouseFabric(*o))
            continue;

        if (!o->itemEntry)
        {
            ++lost;
            continue;
        }

        while (next <= HOUSE_STORAGE_MAX && shelf.find(next) != shelf.end())
            ++next;

        if (next > HOUSE_STORAGE_MAX)
        {
            ++lost;
            continue;
        }

        shelf[next] = o->itemEntry;
        CharacterDatabase.PExecute(
            "REPLACE INTO house_storage (account, slot, item_entry) VALUES (%u, %u, %u)",
            house.accountId, next, o->itemEntry);
        ++next;
        ++stowed;
    }

    // The animals, on the same shelf and by the same rules -- an entry with a
    // crate goes into a square, one placed with `.house critter add` is lost.
    // The shelf holds item entries and a critter crate is an ordinary item, so
    // this needed no schema and no second store.
    for (uint32 guid : house.critters)
    {
        std::map<uint32, HouseCritter>::const_iterator itr = m_critters.find(guid);
        if (itr == m_critters.end())
            continue;

        if (!itr->second.itemEntry)
        {
            ++lost;
            continue;
        }

        while (next <= HOUSE_STORAGE_MAX && shelf.find(next) != shelf.end())
            ++next;

        if (next > HOUSE_STORAGE_MAX)
        {
            ++lost;
            continue;
        }

        shelf[next] = itr->second.itemEntry;
        CharacterDatabase.PExecute(
            "REPLACE INTO house_storage (account, slot, item_entry) VALUES (%u, %u, %u)",
            house.accountId, next, itr->second.itemEntry);
        ++next;
        ++stowed;
    }
}

// What to say afterwards. Shared so the two callers that tear a house down
// cannot describe the same outcome two ways.
//
// SILENT WHEN NOTHING HAPPENED. A house with no player furniture in it -- a
// freshly stamped one, or a template -- would otherwise get "0 pieces went to
// storage" as the answer to a question nobody asked.
static void ReportStowResult(Player* player, uint32 stowed, uint32 lost)
{
    if (!player || (!stowed && !lost))
        return;

    ChatHandler handler(player);

    if (stowed && lost)
        handler.PSendSysMessage("%u pieces went to your furniture storage; %u could not be saved.", stowed, lost);
    else if (stowed)
        handler.PSendSysMessage("%u pieces went to your furniture storage.", stowed);
    else
        handler.PSendSysMessage("%u pieces could not be saved.", lost);
}

bool HouseMgr::StorageWithdraw(Player* player, uint32 slot, std::string& error)
{
    if (!StorageAllowed(player, error))
        return false;

    uint32 const accountId = player->GetSession()->GetAccountId();
    auto acc = m_storage.find(accountId);
    if (acc == m_storage.end())
    {
        error = "There is nothing in your storage.";
        return false;
    }

    auto row = acc->second.find(slot);
    if (row == acc->second.end())
    {
        error = "That square is empty.";
        return false;
    }

    uint32 const itemEntry = row->second;

    // BAG SPACE FIRST, THE SQUARE SECOND -- the same ordering that makes
    // picking an object up safe. Emptying the square and then discovering there
    // is nowhere to put the crate would lose it outright, and storage is where
    // people put the furniture they care about.
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

    acc->second.erase(row);
    CharacterDatabase.PExecute("DELETE FROM house_storage WHERE account = %u AND slot = %u",
                               accountId, slot);
    return true;
}

bool HouseMgr::StorageMove(Player* player, uint32 from, uint32 to, std::string& error)
{
    if (!StorageAllowed(player, error))
        return false;

    if (!from || from > HOUSE_STORAGE_MAX || !to || to > HOUSE_STORAGE_MAX)
    {
        error = "There is no such square.";
        return false;
    }
    if (from == to)
        return true;

    uint32 const accountId = player->GetSession()->GetAccountId();
    auto acc = m_storage.find(accountId);
    if (acc == m_storage.end())
    {
        error = "There is nothing in your storage.";
        return false;
    }

    auto src = acc->second.find(from);
    if (src == acc->second.end())
    {
        error = "That square is empty.";
        return false;
    }

    uint32 const moving = src->second;
    auto dst = acc->second.find(to);
    bool const swapping = dst != acc->second.end();
    uint32 const displaced = swapping ? dst->second : 0;

    if (swapping)
        acc->second[from] = displaced;
    else
        acc->second.erase(src);
    acc->second[to] = moving;

    // ONE TRANSACTION, because the ORDER of these matters and PExecute does not
    // guarantee any: CharacterDatabase.WorkerThreads = 4, each with its own
    // connection, so a queued DELETE can land after the REPLACE that was meant
    // to follow it. BeginTransaction rides a single worker, which is the same
    // reason the Player save path uses one.
    CharacterDatabase.BeginTransaction();
    CharacterDatabase.PExecute("DELETE FROM house_storage WHERE account = %u AND slot IN (%u, %u)",
                               accountId, from, to);
    CharacterDatabase.PExecute("INSERT INTO house_storage (account, slot, item_entry) VALUES (%u, %u, %u)",
                               accountId, to, moving);
    if (swapping)
        CharacterDatabase.PExecute("INSERT INTO house_storage (account, slot, item_entry) VALUES (%u, %u, %u)",
                                   accountId, from, displaced);
    CharacterDatabase.CommitTransaction();
    return true;
}

// The seam the item script comes in through. src/scripts cannot include
// HouseMgr.h -- src/game/Housing is the one game subfolder missing from the
// scripts project's include path -- so this is declared `extern` at the call
// site and defined here, exactly the way SCRIPT_COMMAND_RECRUIT_BOT reaches
// the playerbot module. Player and Item are all the caller needs to name.
//
// IT TAKES NO POINT ANY MORE. It carried three floats for the crate's own
// ground reticle; the chalk owns aiming now, so a crate has nothing to send and
// there is nothing here to receive it.
bool HouseUseFurnitureItem(Player* player, Item* item)
{
    if (!player || !item)
        return false;

    // The same fork as HouseFurnitureShouldCast, and it has to be the same
    // order: an entry in both tables would place furniture there and an animal
    // here, which is the one way these two can disagree.
    std::string error;
    bool const ok = sHouseMgr.IsCritterItem(item->GetEntry())
                  ? sHouseMgr.UseCritterItem(player, item->GetEntry(), error)
                  : sHouseMgr.UseFurnitureItem(player, item->GetEntry(), error);

    if (!ok)
    {
        ChatHandler(player).SendSysMessage(error.c_str());
        return false;
    }
    return true;
}

bool HouseMgr::CanPlaceFurniture(Player* player, std::string& error)
{
    House* house = nullptr;
    if (!CheckOwner(*this, player, house, error))
        return false;

    if (CountPlayerObjects(*house) >= HOUSE_MAX_OBJECTS)
    {
        error = "Your house is full. Remove something first.";
        return false;
    }

    return true;
}

// SHOULD USING THIS CRATE START A CAST? Answered for the script that has the
// item in its hand, across the same extern seam as everything else here.
//
// TRUE means let the item's spell cast; the furniture goes down when the bar
// finishes. FALSE means it was refused and the player has already been told --
// there is no third answer any more. There used to be, because a setting chose
// between casting and placing instantly; the setting is gone and a crate always
// casts, so the question collapsed from three states to two.
//
// THE REFUSAL IS DECIDED HERE, BEFORE THE CAST, not when it finishes. A bar
// that runs for a second and then says "your house is full" is a worse answer
// than an instant one -- the same ordering that makes picking an object up
// safe: ask whether it can work before anything visible happens.
bool HouseFurnitureShouldCast(Player* player, Item* item)
{
    if (!player || !item)
        return false;

    // TWO CATALOGUES, ONE SCRIPT. A critter crate carries the same
    // script_name and the same on-use spell as a chair -- see sql/custom/059
    // for why that is worth more than a tidier second script: a new spell
    // script name is read ONCE AT STARTUP, so it would drag back the
    // SQL-before-restart ordering trap. The item entry is what tells them
    // apart, here and in HouseUseFurnitureItem, and nowhere else.
    std::string error;
    if (sHouseMgr.IsCritterItem(item->GetEntry()))
    {
        if (!sHouseMgr.CanPlaceCritter(player, error))
        {
            ChatHandler(player).SendSysMessage(error.c_str());
            return false;
        }
        return true;
    }

    // Not one of ours at all. The script is only ever bound to furniture and
    // crates, so this is a guard rather than a path anybody reaches.
    if (!sHouseMgr.IsFurnitureItem(item->GetEntry()))
    {
        ChatHandler(player).SendSysMessage("That is not furniture.");
        return false;
    }

    if (!sHouseMgr.CanPlaceFurniture(player, error))
    {
        ChatHandler(player).SendSysMessage(error.c_str());
        return false;
    }

    return true;
}

//== lookups =================================================================

House* HouseMgr::GetHouseById(uint32 id)
{
    auto itr = m_houses.find(id);
    return itr != m_houses.end() ? &itr->second : nullptr;
}

House* HouseMgr::GetHouseByAccount(uint32 accountId)
{
    auto itr = m_houseByAccount.find(accountId);
    return itr != m_houseByAccount.end() ? GetHouseById(itr->second) : nullptr;
}

House* HouseMgr::GetHouseByInstance(uint32 instanceId)
{
    auto itr = m_houseByInstance.find(instanceId);
    return itr != m_houseByInstance.end() ? GetHouseById(itr->second) : nullptr;
}

House* HouseMgr::GetHouseAt(Player* player)
{
    if (!player || !player->IsInWorld() || player->GetMapId() != HOUSE_MAP_ID)
        return nullptr;
    return GetHouseByInstance(player->GetInstanceId());
}

std::vector<HouseObject const*> HouseMgr::GetObjects(uint32 houseId)
{
    std::vector<HouseObject const*> out;
    if (House* h = GetHouseById(houseId))
        for (uint32 guid : h->objects)
        {
            auto itr = m_objects.find(guid);
            if (itr != m_objects.end())
                out.push_back(&itr->second);
        }
    return out;
}

uint32 HouseMgr::AllocateObjectGuid()
{
    if (m_nextObjectGuid >= HOUSE_GO_GUID_MAX)
        return 0;
    return m_nextObjectGuid++;
}

//== houses and their instances ==============================================

House* HouseMgr::CreateHouse(uint32 accountId)
{
    // DirectPExecute, not PExecute. PExecute queues onto the database worker
    // thread, and the SELECT below runs synchronously -- so the read raced the
    // write and the first .house go of a new account failed with "failed to
    // create a house", leaving the player to run it a second time. Observed
    // 2026-08-29 21:43:34; the retry then tripped the unique key and logged a
    // duplicate INSERT. The read-back has to see the row it just wrote.
    CharacterDatabase.DirectPExecute("INSERT INTO house (account, map, instance_id, name, created_at) VALUES (%u, %u, 0, '', " UI64FMTD ")",
                                     accountId, HOUSE_MAP_ID, uint64(time(nullptr)));

    QueryResult* result = CharacterDatabase.PQuery("SELECT id FROM house WHERE account = %u AND map = %u", accountId, HOUSE_MAP_ID);
    if (!result)
    {
        sLog.outError("Housing: failed to create a house for account %u.", accountId);
        return nullptr;
    }

    House h;
    h.id        = result->Fetch()[0].GetUInt32();
    h.accountId = accountId;
    delete result;

    m_houses[h.id] = h;
    m_houseByAccount[accountId] = h.id;
    return GetHouseById(h.id);
}

bool HouseMgr::PrepareInstance(House& house)
{
    MapEntry const* mapEntry = sMapStorage.LookupEntry<MapEntry>(HOUSE_MAP_ID);
    if (!mapEntry)
        return false;

    if (!house.instanceId)
    {
        house.instanceId = sMapMgr.GenerateInstanceId();

        // Written by hand rather than through DungeonPersistentState::SaveToDB,
        // because that stamps GetResetTimeForDB() and we want the far-future
        // value that keeps the house out of every reset path.
        CharacterDatabase.PExecute("REPLACE INTO instance (id, map, resettime, data) VALUES (%u, %u, " UI64FMTD ", '')",
                                   house.instanceId, HOUSE_MAP_ID, uint64(HOUSE_RESET_TIME));
        CharacterDatabase.PExecute("UPDATE house SET instance_id = %u WHERE id = %u", house.instanceId, house.id);
        m_houseByInstance[house.instanceId] = house.id;

        sLog.outInfo("Housing: house %u (account %u) assigned instance %u.", house.id, house.accountId, house.instanceId);
    }

    // load = true so this neither schedules a two hour reset nor writes the
    // instance row a second time; canReset = false because somebody is about to
    // be permanently bound to it.
    MapPersistentState* state = sMapPersistentStateMgr.AddPersistentState(
            mapEntry, house.instanceId, HOUSE_RESET_TIME, false, true);

    return state != nullptr;
}

// SOMEBODY STANDING STILL INSIDE A SPAWNED BUILDING IS NOT DRAWN TO SOMEBODY
// WHO HAS JUST WALKED IN, and they arrive hanging two yards over the floor.
// One late packet answers both, because both are the same fault wearing two
// hats: the client has not finished placing anybody when the arrival lands.
//
// Measured 2026-09-03 with LogFilter_VisibilityChanges off -- three controlled
// arrivals whose SERVER logs are line-for-line identical:
//
//   Ragefire Chasm, real dungeon geometry ... 7.73yd ... drawn
//   House, owner INSIDE the Goldshire Inn ... 1.44yd ... NOT drawn
//   House, owner outside the inn ............ 9.13yd ... drawn
//
// So the create block goes out correctly and the client throws one away. A
// spawned building has collision but NOT the client's portal system -- already
// why sky and terrain show through indoors -- and it turns out to cost units
// too, because the client places what it draws by which WMO group it is in and
// cannot resolve one.
//
// THREE THINGS WERE WRONG AT ONCE and each fix alone read as a flat failure,
// which is the part worth keeping: a fix that changes nothing is not proof the
// theory is wrong when the theory has three independent preconditions.
//
//   DELIVERY. SendHeartBeat goes through WorldObject::SendMovementMessageToSet,
//   which for a Player uses the PlayerBroadcaster -- or, with
//   Network.PacketBroadcast.Threads = 0 which is what this server runs, a
//   fallback branch reading `SendObjectMessageToSet(&data, true, except)`. The
//   self argument is HARDCODED true there, so `includingSelf = false` is
//   silently ignored. Nothing is broadcast here: every occupant is described
//   straight to every occupant's session.
//
//   POSITION. A plain heartbeat carries the position the unit is ALREADY at, so
//   the client reads no change and does nothing with it. Real movement, which
//   does fix it, carries a different one. Hence the hop -- and hence a second
//   truthful packet, so nothing is left displaced.
//
//   TIMING. A beat sent from DungeonMap::Add lands in the same instant as the
//   create block and does nothing at all. HOUSE_NUDGE_DELAY is the whole point
//   of this being armed rather than sent.
//
// AND IT FIXED A BUG NOBODY WAS LOOKING FOR, found because the ignored self
// flag was curing it by accident: THE ARRIVING PLAYER'S OWN MODEL is invisible
// to them until they move. That half is kept and is now asked for -- it would
// have been deleted silently the moment the self flag was "corrected".
//
// The precedent is three lines from the code this was found in:
// MovementHandler.cpp:278 already beats once after a teleport to un-stick the
// MacOS client's camera, a different client-side rendering fault of the same
// shape.
#define HOUSE_NUDGE_LIFT    0.05f       // the hop: a real delta, invisible
// THE DELAY IS `arrival_ping_ms`, A SERVER-WIDE SETTING, defaulting to 3000.
//
// MEASURED, NOT CHOSEN, because it is pure client behaviour and no amount of
// reading settles it. The sweep that found the fix beat at every tick from 1 to
// 6, then each end was separated by hand: **3000ms works, 2000ms does not,
// 1500ms does not.** So the boundary is between 2 and 3 seconds.
//
// 2500 WAS DELIBERATELY NOT TRIED. It might well work, and it would sit
// directly on a load race -- which does not fail cleanly. It fails SOMETIMES,
// on a busier house or a slower machine, and presents as the whole feature
// never having worked. The asymmetry decides it: too low costs the feature,
// silently and intermittently; too high costs a couple of seconds of an
// empty-looking room. Half a second of margin is not worth that class of bug.
//
// AND IT IS A SETTING RATHER THAN A CONSTANT FOR THAT SAME REASON. A load race
// is a property of the machine and the client, not of this code, so 3000 is the
// right answer HERE and cannot be the right answer everywhere. An operator on
// slower hardware must be able to raise it once and have it stick -- which is
// what separates this from `.house object marker` and `.house entrance model`,
// the other live-tuned knobs, where the answer is found once and then belongs
// in the source.
//
// If it is ever retuned, search DOWNWARD from a value confirmed to work; going
// up from one that fails is a test per guess, which is what the sweep existed
// to avoid.

// ONE PING, LATE -- not a sweep. The sweep was how the delay was FOUND: one
// delay per build is one bit per build, and two builds had already gone on
// single guesses, so beating across the whole plausible range settled it in
// one. Keeping the sweep afterwards would be keeping the scaffolding.
void HouseMgr::NudgeOccupants(Player* player)
{
    Map* map = player->GetMap();
    if (!map)
        return;

    HouseArrival& a = m_arrivals[player->GetGUIDLow()];
    a.instanceId = map->GetInstanceId();
    a.ticks      = ArrivePingTicks(GetSetting(uint32(HOUSE_SETTING_GLOBAL_ACCOUNT),
                                              HOUSE_SETTING_ARRIVE_PING));
    a.x          = player->GetPositionX();
    a.y          = player->GetPositionY();
    a.floorZ     = player->GetPositionZ() - HOUSE_ARRIVE_LIFT;
    a.watchTicks = ArrivePingTicks(HOUSE_FALL_WATCH);
}

// The ping itself, from the instance script's own throttle.
void HouseMgr::RunPendingNudges(Map* map)
{
    if (!m_enabled || !map)
        return;

    for (std::map<uint32, HouseArrival>::iterator itr = m_arrivals.begin(); itr != m_arrivals.end();)
    {
        HouseArrival& a = itr->second;
        if (a.instanceId != map->GetInstanceId())
        {
            ++itr;
            continue;
        }

        Player* p = map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, itr->first));

        if (a.ticks)                    // waiting for the ping
        {
            if (--a.ticks)
            {
                ++itr;
                continue;
            }

            if (p)
            {
                SettleArrival(p, a);
                BeatOccupants(map);
                BeatCritters(map);
            }
        }
        else if (p)                     // after it: see HOUSE_FALL_WATCH
            RescueFall(p, a);

        if (p && a.watchTicks)
        {
            --a.watchTicks;
            ++itr;
            continue;
        }

        m_arrivals.erase(itr++);        // SPENT, so an arrival nobody is around
    }                                   // for leaves no row behind

    // A beat with no arrival behind it: a critter placed into a room somebody
    // was already standing in. Same packet, same delay, different trigger.
    std::map<uint32, uint32>::iterator beat = m_critterBeats.find(map->GetInstanceId());
    if (beat != m_critterBeats.end() && !--beat->second)
    {
        BeatCritters(map);
        m_critterBeats.erase(beat);
    }
}

// PUT THEM DOWN. HOUSE_ARRIVE_LIFT drops the player in two yards high because a
// teleport lands them before the destination has loaded and arriving at exactly
// floor height falls THROUGH it (sql-free fix, source 8f11867, 2026-08-31).
// The lift assumed they would then fall. They do not -- the client considers
// itself placed where it was put and applies no gravity until the player moves,
// so the trade left everybody hovering.
//
// This is the moment the assumption can be retired instead of tuned: the room
// has finished loading, so the floor is known-good and they can simply be
// stood on it. That is why the lift is not just made smaller -- a smaller lift
// is a smaller float AND less protection, worse at both jobs.
//
// THE FLOOR IS REMEMBERED, NOT QUERIED. map->GetHeight reads the raw .map
// surface and map 28 is a single plane at z = 0, so it answers 0 from any
// height and every WMO floor a template builds on is invisible to it -- the
// same trap the placement reticle documents. GetArrivalPosition knew the real
// floor; NudgeOccupants writes it down.
//
// Nothing happens if they have already moved: walking is what makes the client
// apply gravity in the first place, so a player who has taken a step has
// solved this themselves and must not be yanked back to where they came in.
void HouseMgr::SettleArrival(Player* player, HouseArrival const& a)
{
    if (player->GetPositionZ() <= a.floorZ + 0.1f)
        return;                         // already down

    if (!player->IsWithinDist3d(a.x, a.y, player->GetPositionZ(), 1.0f))
        return;                         // moved off the arrival point

    player->NearTeleportTo(a.x, a.y, a.floorZ, player->GetOrientation());
}

// Fell through the building onto the ground plane. See HOUSE_FALL_WATCH.
void HouseMgr::RescueFall(Player* player, HouseArrival const& a)
{
    if (a.floorZ < HOUSE_FALL_MIN_FLOOR)
        return;                         // built on the plane

    if (player->GetPositionZ() > HOUSE_FALL_PLANE_Z)
        return;                         // not on the plane

    if (!player->IsWithinDist2d(a.x, a.y, HOUSE_FALL_RADIUS))
        return;                         // walked out of the building

    player->NearTeleportTo(a.x, a.y, a.floorZ, player->GetOrientation());
}

void HouseMgr::BeatOccupants(Map* map)
{
    Map::PlayerList const& players = map->GetPlayers();

    for (Map::PlayerList::const_iterator sub = players.begin(); sub != players.end(); ++sub)
    {
        Player* s = sub->getSource();
        if (!s || !s->IsInWorld())
            continue;

        float const x = s->GetPositionX();
        float const y = s->GetPositionY();
        float const z = s->GetPositionZ();
        float const o = s->GetOrientation();

        // Up, then back: a real delta in both directions, ending truthful. Only
        // m_movementInfo is touched, so the server's own idea of where anybody
        // stands never moves and the second packet leaves every client agreeing
        // with it -- even if no further movement ever arrives.
        for (int pass = 0; pass < 2; ++pass)
        {
            s->m_movementInfo.ChangePosition(x, y, pass ? z : z + HOUSE_NUDGE_LIFT, o);
            s->m_movementInfo.SetAsServerSide();

            WorldPacket data(MSG_MOVE_HEARTBEAT);
            data << s->GetPackGUID();
            data << s->m_movementInfo;

            for (Map::PlayerList::const_iterator vw = players.begin(); vw != players.end(); ++vw)
                if (Player* v = vw->getSource())
                    if (v->GetSession())
                        v->GetSession()->SendPacket(&data);
        }
    }
}

// THE ANIMALS NEED THE SAME NUDGE AS THE PEOPLE, and it took two goes to send
// the right thing.
//
// THE SERVER WAS NEVER THE PROBLEM. With LogFilter_VisibilityChanges off and
// LogFileLevel 3, a guest walking in logs:
//
//   Creature (Entry: 721 Guid: 8000000) is visible now for Player Awd. Distance = 6.43
//
// So the create block goes out correctly at six yards and the client declines
// to DRAW it -- the same fault as a player standing in a spawned building,
// on a different object type. The client cannot resolve which WMO group the
// thing is in, so it never places it anywhere.
//
// AND IT IS A DRAWING FAULT, NOT A DELIVERY ONE. The owner settled that in one
// move: `.house critter drag` makes the animal appear for everybody, and drag
// only ever MOVES it. A client cannot move something it has thrown away, so
// the object was there on every client the whole time. That also kills the
// obvious "re-send the create block" fix before it costs a build.
//
// SO CALL THE WHOLE OF NearTeleportTo RATHER THAN HAND-ROLLING THE PACKET.
// The first attempt sent MSG_MOVE_TELEPORT to every occupant's session by hand
// -- the shape of the player beat -- and changed nothing. Two things it left
// out, and drag has both:
//
//   DisableSpline(). A wandering critter is mid-spline half the time, and a
//   teleport aimed at a unit the client is already interpolating is one the
//   client can carry straight on over.
//
//   A REAL DISTANCE. The player beat's 0.05yd lift is enough for somebody
//   standing still. There is no reason to assume it is enough for something
//   already moving.
//
// So this is drag, minus the part that changes where the animal lives: lift it
// a hair, put it back, both through the function that provably works. It
// interrupts and resets the movement generator on the way, which is what stops
// the spline fighting it, and ending on the truthful call leaves the animal
// exactly where it was standing.
void HouseMgr::BeatCritters(Map* map)
{
    House* house = GetHouseByInstance(map->GetInstanceId());
    if (!house || house->critters.empty())
        return;

    if (map->GetPlayers().isEmpty())
        return;

    for (uint32 guid : house->critters)
    {
        std::map<uint32, HouseCritter>::const_iterator itr = m_critters.find(guid);
        if (itr == m_critters.end())
            continue;

        Creature* c = map->GetCreature(ObjectGuid(HIGHGUID_UNIT, itr->second.entry, guid));
        if (!c || !c->IsInWorld())
            continue;

        float const x = c->GetPositionX();
        float const y = c->GetPositionY();
        float const z = c->GetPositionZ();
        float const o = c->GetOrientation();

        // Up, then back. NearTeleportTo broadcasts to observers itself, and the
        // visibility log above confirms the guest IS an observer -- that was
        // the thing worth checking before trusting a function that routes by
        // the visible set.
        c->NearTeleportTo(x, y, z + HOUSE_NUDGE_LIFT, o);
        c->NearTeleportTo(x, y, z, o);
    }
}

void HouseMgr::ArmCritterBeat(Map* map)
{
    if (!map)
        return;

    // Never zero: RunPendingNudges pre-decrements, so a 0 here would wrap and
    // the beat would be armed for roughly forever.
    uint32 const ticks = ArrivePingTicks(GetSetting(uint32(HOUSE_SETTING_GLOBAL_ACCOUNT),
                                                    HOUSE_SETTING_ARRIVE_PING));
    m_critterBeats[map->GetInstanceId()] = ticks ? ticks : 1;
}

// The line you get for walking in. See the header for why it hangs off the map
// rather than off the teleport.
void HouseMgr::OnPlayerArrive(Player* player)
{
    if (!m_enabled || !player || player->GetMapId() != HOUSE_MAP_ID)
        return;

    NudgeOccupants(player);

    House* house = GetHouseAt(player);
    if (!house)
        return;

    ChatHandler handler(player);

    // A DEVELOPER IN A TEMPLATE IS NOT HOME, and greeting them as though they
    // were is the kind of wrong that teaches somebody the message is noise.
    // The authoring space IS a house row owned by a synthetic account, so it
    // passes every ownership check in here and would otherwise get the warmest
    // line housing sends while they are standing in shared fabric.
    if (house->authorsTemplate)
    {
        if (HouseTemplate const* tpl = GetTemplate(house->authorsTemplate))
            handler.PSendSysMessage("Editing template \"%s\". .house template save publishes it.", tpl->name.c_str());
        return;
    }

    // A GUEST GETS NOTHING HERE, deliberately. Every route into somebody
    // else's house is an explicit act -- .house visit, a gear button, walking
    // into a leader's door -- and each already says what it did. A second line
    // on arrival would be the same news twice, and the one thing a guest needs
    // to know (that the furniture is not theirs) is better said by the refusal
    // at the moment they try, where it is an answer rather than a warning about
    // something they may not have been about to do.
    if (!CanEditHouse(player, *house))
        return;

    handler.PSendSysMessage("Welcome home, %s.", player->GetName());

    // THE HINT IS SELF-CLEARING, WITH NO STATE TO STORE. Two lines on every
    // arrival is exactly the noise the rest of this pass is removing, and a
    // "have they seen it" flag would be a column, a load, a save and a thing to
    // reset. An empty house answers the same question for free: nobody who has
    // placed a single piece of furniture still needs telling that the commands
    // exist, and somebody who has picked everything up again seeing it a second
    // time costs one line.
    if (!CountPlayerObjects(*house))
        handler.SendSysMessage("Type .house to see what you can do in here, or /house to browse furniture.");
}

bool HouseMgr::SendPlayerHome(Player* player, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }
    if (player->IsInCombat())
    {
        error = "You cannot go home while in combat.";
        return false;
    }

    uint32 accountId = player->GetSession()->GetAccountId();
    House* house = GetHouseByAccount(accountId);

    // TELEPORT ONLY. This used to create the house, which made every door in
    // the world -- and .house go -- a free claim. Since templates, a house is
    // claimed at an entrance (ClaimHouseAt), where it is stamped with that
    // door's inside; this one check retires auto-create for all four callers
    // at once. Every existing house keeps working: it exists, so it teleports.
    if (!house)
    {
        error = "You do not have a home yet. Find an entrance portal and claim one.";
        return false;
    }

    if (!PrepareInstance(*house))
    {
        error = "Could not prepare your house.";
        return false;
    }

    DungeonPersistentState* state = (DungeonPersistentState*)sMapPersistentStateMgr.GetPersistentState(HOUSE_MAP_ID, house->instanceId);
    if (!state)
    {
        error = "Could not prepare your house.";
        return false;
    }

    // Going home ends any visit. Without this the override would still be set
    // and would route us straight back into somebody else's house.
    player->ClearForcedInstance();

    // The permanent bind is the whole trick: GetBoundInstanceSaveForSelfOrGroup
    // consults it before any group bind, so every entry path -- this teleport, a
    // relog, a corpse run -- lands in the same copy of the map.
    player->BindToInstance(state, true);

    // Beside your own way out, not at a fixed corner of the map. Once somebody
    // builds, the doorway they built is where coming home means arriving.
    //
    // FORCE THE MAP CHANGE when already on the house map in a DIFFERENT
    // instance: TeleportTo to the same map id takes the near-teleport path,
    // which relocates within the CURRENT instance and never re-resolves the
    // binds -- so .house go from inside a template just moved you across the
    // template. Same-instance stays near, which is the cheap reposition.
    float ax, ay, az, ao;
    GetArrivalPosition(*house, ax, ay, az, ao);
    uint32 const teleFlags = (player->GetMapId() == HOUSE_MAP_ID && player->GetInstanceId() != house->instanceId)
                             ? TELE_TO_FORCE_MAP_CHANGE : 0;
    return player->TeleportTo(HOUSE_MAP_ID, ax, ay, az, ao, teleFlags);
}

//== visiting ================================================================

// A name is turned into an ACCOUNT, so every alt of a friend is the same person
// and renaming a character changes nothing.
static bool LookUpAccount(std::string const& name, uint32& accountId, std::string& error)
{
    accountId = sObjectMgr.GetPlayerAccountIdByPlayerName(name);
    if (!accountId)
    {
        error = "No character by that name.";
        return false;
    }
    return true;
}

bool HouseMgr::VisitHouse(Player* player, std::string const& ownerName, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }
    if (player->IsInCombat())
    {
        error = "You cannot travel while in combat.";
        return false;
    }

    uint32 ownerAccount = 0;
    if (!LookUpAccount(ownerName, ownerAccount, error))
        return false;

    // Your own house by any of your own characters is just going home.
    if (ownerAccount == player->GetSession()->GetAccountId())
        return SendPlayerHome(player, error);

    House* house = GetHouseByAccount(ownerAccount);
    if (!house)
    {
        error = "They do not have a house.";
        return false;
    }
    // A GROUP IS THE INVITATION here too. Refusing the typed command while the
    // leader's own front door lets the same player walk straight in would be a
    // difference with no reason behind it -- and this is the same rule, minus
    // the door, because a typed name has no door to be coherent with.
    HousePartyHost host;
    bool const following = GetPartyLeader(player, host) && host.leaderAccount == ownerAccount;

    // A DEVELOPER GETS IN WITHOUT A GUEST ROW, and this exists to keep them off
    // `.appear`, which is genuinely destructive here rather than merely
    // impolite.
    //
    // `.appear <resident>` walks the core's own instance path
    // (HandleGonameCommand): it UNBINDS the GM from map 28 -- throwing away the
    // permanent bind to their own house -- and then binds them to the
    // resident's instance, permanently, because a house is created with
    // canReset = false and the bind takes `!save->CanReset()`. `.summon` is the
    // mirror image and simply does not work: the target teleports to map 28 and
    // resolves it against their OWN bind, so they arrive alone in their own
    // house while the GM waits in somebody else's.
    //
    // Visiting has neither problem, because a visit is not a bind at all --
    // EnterHouseAsVisitor travels on the forced instance, which
    // BindPlayerOrGroupOnEnter checks first and returns on. So the fix is not
    // to patch the GM commands but to make the housing route reachable without
    // asking a player to let staff in.
    bool const staff = player->GetSession()->GetSecurity() >= SEC_DEVELOPER;

    // THE PARTY IS THE WHOLE RULE, and the error says what to do rather than
    // what went wrong. There is no list to be on any more, so "you are not on
    // their guest list" would name a thing that no longer exists -- and the
    // action it implies (ask them to add you) is not the action that works.
    if (!following && !staff)
    {
        error = "Join their party and they can take you home with them.";
        return false;
    }

    // THE LOCK GATES THE PARTY TICKET, and only that. Staff with no party are
    // on their own bypass, which the lock is not about; staff who ARE in the
    // party knock like anybody else, which is also what makes this testable
    // from a GM account.
    if (following && IsLocked(*house))
        return Knock(player, *house, host, 0, error);

    return EnterHouseAsVisitor(player, *house, error);
}

// THE ARRIVAL, shared by every way into somebody else's house -- the typed
// command and walking through the door. Permission is the caller's
// job and deliberately so; what lives here is the part that must never differ
// between them, because a second copy of it is a second place for the forced
// instance and the teleport flag to fall out of step.
bool HouseMgr::EnterHouseAsVisitor(Player* player, House& house, std::string& error)
{
    if (!PrepareInstance(house))
    {
        error = "Could not open their house.";
        return false;
    }

    // An override rather than a bind: binds are one per (character, map), so a
    // visitor who owns a house cannot be bound to somebody else's. Cleared when
    // they leave the map, in DungeonMap::Remove.
    player->SetForcedInstance(HOUSE_MAP_ID, house.instanceId);

    // A guest lands where the OWNER arrives -- beside the way out, which is the
    // one spot in a house a visitor definitely needs to be able to find again.
    // Forced map change for the same reason SendPlayerHome forces it: visiting
    // from inside a house is a same-map, different-instance trip, and the
    // near-teleport path would go nowhere. DungeonMap::Remove deliberately
    // keeps the forced id on a same-map teleport, so it survives the hop.
    float ax, ay, az, ao;
    GetArrivalPosition(house, ax, ay, az, ao);
    uint32 const teleFlags = (player->GetMapId() == HOUSE_MAP_ID && player->GetInstanceId() != house.instanceId)
                             ? TELE_TO_FORCE_MAP_CHANGE : 0;

    if (!player->TeleportTo(HOUSE_MAP_ID, ax, ay, az, ao, teleFlags))
    {
        player->ClearForcedInstance();
        error = "Could not travel there.";
        return false;
    }
    return true;
}

//== travelling with a party =================================================
//
// A GROUP IS THE INVITATION. Adding every party member to a guest list would
// be the obvious implementation and is the wrong one twice over: it writes
// rows for something that lasts minutes, and it outlives the party -- somebody
// you grouped with once would keep a key to your house forever. Resolving the
// leader live has neither problem, and it revokes itself.
//
// It is narrow on purpose. The offer only exists at the leader's OWN home
// door, so standing at any other entrance behaves exactly as it did before,
// and a party is never pulled through a door none of them chose to stand in.

// THE PERMISSION ITSELF, with no door in it: who leads the party this player is
// in, and do they own a house. Split out from the door-shaped question below
// because `.house visit` asks it too, and a permission written twice is a
// permission that eventually disagrees with itself.
bool HouseMgr::GetPartyLeader(Player* player, HousePartyHost& out)
{
    if (!m_enabled || !player)
        return false;

    Group* group = player->GetGroup();
    if (!group)
        return false;

    ObjectGuid const leaderGuid = group->GetLeaderGuid();
    if (leaderGuid.IsEmpty() || leaderGuid == player->GetObjectGuid())
        return false;                                   // you lead: this is your own house question

    // Cached, not a query -- ObjectMgr::GetPlayerAccountIdByGUID reads the live
    // player first and the player cache second, so this is cheap enough for the
    // portal poll to ask it.
    uint32 const leaderAccount = sObjectMgr.GetPlayerAccountIdByGUID(leaderGuid);
    if (!leaderAccount)
        return false;

    // The leader is another of your own characters. Their house IS your house,
    // so the ordinary "go home" answer is the right one and offering to visit
    // yourself would be nonsense.
    if (leaderAccount == player->GetSession()->GetAccountId())
        return false;

    House* house = GetHouseByAccount(leaderAccount);
    if (!house)
        return false;

    out.leaderGuid    = leaderGuid;
    out.leaderAccount = leaderAccount;
    out.houseId       = house->id;
    out.leaderName    = group->GetLeaderName();
    return true;
}

// The same question with a door attached, which is the only form the entrance
// ever asks. A zero portal id is refused rather than treated as "any door":
// PortalIdFromGuid returns 0 for a guid that is not one of ours, and a
// wildcard here would turn that into a free teleport out of any gameobject
// that ever got the portal script bound to it by mistake.
bool HouseMgr::GetPartyLeaderHouse(Player* player, uint32 portalId, HousePartyHost& out)
{
    if (!portalId)
        return false;

    HousePartyHost host;
    if (!GetPartyLeader(player, host))
        return false;

    House* house = GetHouseById(host.houseId);
    if (!house || !IsHomePortal(*house, portalId))
        return false;

    out = host;
    return true;
}

void HouseMgr::SetPartyOffer(Player* player, ObjectGuid leaderGuid)
{
    if (!player)
        return;

    if (leaderGuid.IsEmpty())
        m_partyOffer.erase(player->GetGUIDLow());
    else
        m_partyOffer[player->GetGUIDLow()] = leaderGuid;
}

ObjectGuid HouseMgr::TakePartyOffer(Player* player)
{
    if (!player)
        return ObjectGuid();

    std::map<uint32, ObjectGuid>::iterator itr = m_partyOffer.find(player->GetGUIDLow());
    if (itr == m_partyOffer.end())
        return ObjectGuid();

    ObjectGuid const guid = itr->second;
    m_partyOffer.erase(itr);
    return guid;
}

//== the lock and the knock ==================================================
//
// A LOCKED DOOR TURNS THE PARTY TICKET INTO A QUESTION. The party is still the
// whole permission; the lock adds the owner's yes on top, asked through a
// ComfyUI popup (`CHOUSE K <name>`) and answered by the popup typing `.house
// admit` or `.house refuse`. Nothing is written: a knock lives a minute in
// memory, and a guest who is let in rides the same forced instance as any
// other, so EvictLapsedVisitors still shows them out when the party ends.

bool HouseMgr::IsLocked(House const& house) const
{
    return GetSetting(house.accountId, HOUSE_SETTING_LOCKED) != 0;
}

bool HouseMgr::Knock(Player* visitor, House const& house, HousePartyHost const& host, uint32 portalId, std::string& error)
{
    time_t const now = time(nullptr);

    // Swept here rather than on a timer: the map holds a handful of rows at
    // most, and this is the only place that adds to it.
    for (auto itr = m_knocks.begin(); itr != m_knocks.end();)
        itr = (itr->second.expires <= now) ? m_knocks.erase(itr) : ++itr;

    // ONE POPUP PER VISITOR EVERY HOUSE_KNOCK_SECONDS, counted from the knock
    // and not from the answer. CheckPortal already holds the walk-through to
    // one knock per step into the ring; this holds every route, the gear and
    // `.house visit` included, to one popup per half minute, answered or not.
    auto pending = m_knocks.find(visitor->GetGUIDLow());
    if (pending != m_knocks.end() && pending->second.houseId == house.id)
    {
        error = pending->second.answered
              ? "Wait a moment before you knock again."
              : "You have knocked. Wait for " + host.leaderName + ".";
        return false;
    }

    // THE LEADER IS THE OWNER, because GetPartyLeader resolved the house from
    // the leader's account, and an account has one character online at most.
    // So the leader being offline is the owner being offline.
    Player* owner = ObjectAccessor::FindPlayer(host.leaderGuid);
    if (!owner || !owner->IsInWorld() || !owner->GetSession())
    {
        error = "Nobody is home.";
        return false;
    }

    HouseKnock& knock = m_knocks[visitor->GetGUIDLow()];
    knock.houseId  = house.id;
    knock.portalId = portalId;
    knock.expires  = now + HOUSE_KNOCK_SECONDS;

    // The popup for ComfyUI, and one chat line so the knock is on record in
    // the chat frame after the popup has gone.
    owner->SendAddonMessage("CHOUSE", std::string("K ") + visitor->GetName());
    ChatHandler(owner).PSendSysMessage("%s knocks at your door.", visitor->GetName());
    ChatHandler(visitor).PSendSysMessage("You knock on %s's door.", host.leaderName.c_str());

    error.clear();
    return false;
}

bool HouseMgr::AnswerKnock(Player* owner, std::string const& visitorName, bool admit, std::string& error)
{
    if (!m_enabled || !owner || !owner->GetSession())
        return false;

    House* house = GetHouseByAccount(owner->GetSession()->GetAccountId());
    if (!house)
    {
        error = "You do not have a house.";
        return false;
    }

    std::string name = visitorName;
    Player* visitor = normalizePlayerName(name) ? ObjectAccessor::FindPlayerByName(name.c_str()) : nullptr;

    auto itr = visitor ? m_knocks.find(visitor->GetGUIDLow()) : m_knocks.end();
    if (itr == m_knocks.end() || itr->second.houseId != house->id ||
        itr->second.answered || itr->second.expires <= time(nullptr))
    {
        error = "Nobody by that name is at your door.";
        return false;
    }

    HouseKnock const knock = itr->second;

    // A DENY KEEPS THE ROW until the half minute is up, marked answered, so a
    // visitor who steps straight back into the ring does not raise a second
    // popup. Every Open ending, success or refusal, drops it: somebody the
    // owner said yes to should not have to wait out a cooldown to try again.
    if (!admit)
    {
        itr->second.answered = true;
        ChatHandler(visitor).PSendSysMessage("%s does not let you in.", owner->GetName());
        ChatHandler(owner).PSendSysMessage("You turn %s away.", visitor->GetName());
        return true;
    }

    m_knocks.erase(itr);

    // THE KNOCK WAS A PHOTOGRAPH. Half a minute is long enough for the party to
    // break up, and the lock only ever added a yes to the party ticket; it
    // never replaced it. Letting in somebody who has left the party would be
    // undone by EvictLapsedVisitors half a second after they landed.
    HousePartyHost host;
    if (!GetPartyLeader(visitor, host) || host.leaderAccount != house->accountId)
    {
        error = std::string(visitor->GetName()) + " is not in your party any more.";
        return false;
    }
    if (visitor->IsInCombat() || visitor->IsTaxiFlying() || visitor->IsBeingTeleported())
    {
        error = std::string(visitor->GetName()) + " cannot travel right now.";
        return false;
    }

    std::string travelError;
    if (!EnterHouseAsVisitor(visitor, *house, travelError))
    {
        error = std::string(visitor->GetName()) + " could not come in.";
        return false;
    }

    // Leave takes them back out through the door they knocked at, the same
    // bookkeeping VisitPartyLeaderHouse does. `.house visit` had no door, so
    // Leave falls back down its ladder as it always has.
    if (knock.portalId)
        RememberEntrance(visitor, knock.portalId);

    ChatHandler(visitor).PSendSysMessage("%s lets you in.", owner->GetName());
    return true;
}

// EVERYTHING IS RE-CHECKED HERE. A gossip menu is a photograph: by the time an
// option is clicked the group may have disbanded, promoted somebody else,
// dropped this player, or the leader may have moved house. So the menu decides
// only what to DRAW, and this decides what actually happens.
//
// `expectedLeader` is the difference between the two callers. The gear passes
// the leader whose NAME it printed, and a promotion in between is refused
// rather than silently honoured against a different house -- being sent
// somewhere other than the door you read is the surprise worth preventing.
// Walking through the portal promised nothing, so it passes an empty guid and
// takes whoever leads at the moment of the step.
bool HouseMgr::VisitPartyLeaderHouse(Player* player, ObjectGuid expectedLeader, uint32 portalId, std::string& error)
{
    if (!player)
        return false;

    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }
    if (player->IsInCombat())
    {
        error = "You cannot travel while in combat.";
        return false;
    }

    HousePartyHost host;
    if (!GetPartyLeaderHouse(player, portalId, host))
    {
        // One message for every way it can evaporate -- disbanded, kicked,
        // promoted, the leader moved house. Naming which one would mean
        // re-deriving the reason the resolver just discarded, and the answer
        // the player needs is the same in all of them.
        error = "Your party leader's home is not here any more.";
        return false;
    }

    if (!expectedLeader.IsEmpty() && expectedLeader != host.leaderGuid)
    {
        error = "Your party has a different leader now. Right-click the door again.";
        return false;
    }

    House* house = GetHouseById(host.houseId);
    if (!house)
    {
        error = "Your party leader's home is not here any more.";
        return false;
    }

    // Both routes through a door land here, the walk-through and the gear's
    // "Join X inside", so this one line locks both.
    if (IsLocked(*house))
        return Knock(player, *house, host, portalId, error);

    if (!EnterHouseAsVisitor(player, *house, error))
        return false;

    // The door they came in by, so the way out returns them to it rather than
    // to whichever one they call home -- the same bookkeeping every other trip
    // through an entrance does.
    RememberEntrance(player, portalId);
    return true;
}

//== templates ===============================================================
//
// A template's authoring space IS a house, owned by a synthetic account
// (HOUSE_TEMPLATE_ACCOUNT_MAX - id), so PrepareInstance, every furniture
// command, the exit and the edit gears all work in it unchanged. What
// stamping copies is NOT the live contents but the FROZEN snapshot written by
// SaveTemplate -- half-finished edits never reach a purchase.

namespace
{
    // Names are typed as command tokens, so case must not matter. ASCII only:
    // the create command restricts names harder than this anyway.
    std::string NormalizeTemplateName(std::string name)
    {
        for (char& c : name)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return name;
    }
}

HouseTemplate* HouseMgr::GetTemplate(uint32 id)
{
    auto itr = m_houseTemplates.find(id);
    return itr == m_houseTemplates.end() ? nullptr : &itr->second;
}

HouseTemplate* HouseMgr::GetTemplateByName(std::string const& name)
{
    auto itr = m_templateByName.find(NormalizeTemplateName(name));
    return itr == m_templateByName.end() ? nullptr : GetTemplate(itr->second);
}

std::vector<HouseTemplate const*> HouseMgr::GetTemplates() const
{
    std::vector<HouseTemplate const*> out;
    for (auto const& itr : m_houseTemplates)
        out.push_back(&itr.second);
    return out;
}

void HouseMgr::LoadTemplates()
{
    m_houseTemplates.clear();
    m_templateByName.clear();

    if (QueryResult* result = CharacterDatabase.Query(
            "SELECT id, name, house_id, exit_set, exit_x, exit_y, exit_z, exit_o, saved_at "
            "FROM house_template ORDER BY id"))
    {
        do
        {
            Field* f = result->Fetch();
            HouseTemplate t;
            t.id      = f[0].GetUInt32();
            t.name    = NormalizeTemplateName(f[1].GetCppString());
            t.houseId = f[2].GetUInt32();
            t.exitSet = f[3].GetUInt32() != 0;
            t.exitX   = f[4].GetFloat();
            t.exitY   = f[5].GetFloat();
            t.exitZ   = f[6].GetFloat();
            t.exitO   = f[7].GetFloat();
            t.savedAt = time_t(f[8].GetUInt64());

            // The authoring house learns it is one. A template whose house is
            // gone still stamps -- the frozen rows are all stamping reads --
            // it just cannot be visited or edited any more.
            if (House* authoring = GetHouseById(t.houseId))
                authoring->authorsTemplate = t.id;
            else
                sLog.outErrorDb("Housing: template %u (%s) lost its authoring house %u; it still stamps, but cannot be edited.",
                                t.id, t.name.c_str(), t.houseId);

            m_templateByName[t.name] = t.id;
            m_houseTemplates[t.id] = t;
        }
        while (result->NextRow());
        delete result;
    }

    if (QueryResult* result = CharacterDatabase.Query(
            "SELECT template_id, go_entry, x, y, z, o, rot0, rot1, rot2, rot3, scale "
            "FROM house_template_object ORDER BY id"))
    {
        do
        {
            Field* f = result->Fetch();
            auto itr = m_houseTemplates.find(f[0].GetUInt32());
            if (itr == m_houseTemplates.end())
                continue;

            HouseObject o;
            o.guid    = 0;                      // frozen rows carry no guid
            o.houseId = 0;
            o.goEntry = f[1].GetUInt32();
            o.x = f[2].GetFloat(); o.y = f[3].GetFloat();
            o.z = f[4].GetFloat(); o.o = f[5].GetFloat();
            o.rot0 = f[6].GetFloat(); o.rot1 = f[7].GetFloat();
            o.rot2 = f[8].GetFloat(); o.rot3 = f[9].GetFloat();
            o.scale = f[10].GetFloat();
            o.source = HOUSE_SOURCE_TEMPLATE;

            itr->second.objects.push_back(o);
        }
        while (result->NextRow());
        delete result;
    }

    sLog.outString(">> Housing: %u template(s).", uint32(m_houseTemplates.size()));
}

bool HouseMgr::CreateTemplate(Player* dev, std::string const& rawName, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }

    std::string const name = NormalizeTemplateName(rawName);
    if (name.empty() || name.size() > 48)
    {
        error = "Template names are one word, up to 48 characters.";
        return false;
    }
    if (m_templateByName.count(name))
    {
        error = "There is already a template called that.";
        return false;
    }

    std::string safeName = name;
    CharacterDatabase.escape_string(safeName);

    // DirectPExecute + read-back, the CreateHouse pattern; the UNIQUE name is
    // the read-back key.
    CharacterDatabase.DirectPExecute(
        "INSERT INTO house_template (name, house_id, created_at) VALUES ('%s', 0, " UI64FMTD ")",
        safeName.c_str(), uint64(time(nullptr)));

    uint32 id = 0;
    if (QueryResult* result = CharacterDatabase.PQuery(
            "SELECT id FROM house_template WHERE name = '%s'", safeName.c_str()))
    {
        id = result->Fetch()[0].GetUInt32();
        delete result;
    }
    if (!id)
    {
        error = "Could not save the template.";
        return false;
    }

    uint32 const account = HOUSE_TEMPLATE_ACCOUNT_MAX - id;
    if (account < HOUSE_TEMPLATE_ACCOUNT_MIN)   // sixty-five thousand templates deep
    {
        CharacterDatabase.PExecute("DELETE FROM house_template WHERE id = %u", id);
        error = "There is no room for another template.";
        return false;
    }

    House* house = CreateHouse(account);
    if (!house)
    {
        CharacterDatabase.PExecute("DELETE FROM house_template WHERE id = %u", id);
        error = "Could not create the template's house.";
        return false;
    }

    CharacterDatabase.PExecute("UPDATE house_template SET house_id = %u WHERE id = %u", house->id, id);

    HouseTemplate t;
    t.id      = id;
    t.name    = name;
    t.houseId = house->id;
    m_houseTemplates[id] = t;
    m_templateByName[name] = id;
    house->authorsTemplate = id;

    sLog.outInfo("Housing: template %u (%s) created as house %u.", id, name.c_str(), house->id);
    return GotoTemplate(dev, id, error);
}

bool HouseMgr::SaveTemplate(Player* dev, uint32& savedObjects, bool& savedExit, std::string& error)
{
    House* house = GetHouseAt(dev);
    if (!house || !house->authorsTemplate)
    {
        error = "You are not standing in a template.";
        return false;
    }

    HouseTemplate* tpl = GetTemplate(house->authorsTemplate);
    if (!tpl)
    {
        error = "This template no longer exists.";
        return false;
    }

    // Rewritten wholesale, INSIDE A TRANSACTION. There is no FIFO to lean on:
    // CharacterDatabase runs four async workers, so a bare DELETE racing the
    // INSERTs behind it could eat freshly saved rows. A transaction rides one
    // worker as a unit, which restores the ordering -- same pattern Player
    // save uses.
    CharacterDatabase.BeginTransaction();
    CharacterDatabase.PExecute("DELETE FROM house_template_object WHERE template_id = %u", tpl->id);

    tpl->objects.clear();
    for (uint32 guid : house->objects)
    {
        auto itr = m_objects.find(guid);
        if (itr == m_objects.end())
            continue;

        HouseObject o = itr->second;
        CharacterDatabase.PExecute(
            "INSERT INTO house_template_object (template_id, go_entry, x, y, z, o, rot0, rot1, rot2, rot3, scale) "
            "VALUES (%u, %u, %f, %f, %f, %f, %f, %f, %f, %f, %f)",
            tpl->id, o.goEntry, o.x, o.y, o.z, o.o, o.rot0, o.rot1, o.rot2, o.rot3, o.scale);

        o.guid = 0;
        o.houseId = 0;
        o.source = HOUSE_SOURCE_TEMPLATE;
        tpl->objects.push_back(o);
    }

    tpl->exitSet = house->exitSet;
    tpl->exitX = house->exitX;
    tpl->exitY = house->exitY;
    tpl->exitZ = house->exitZ;
    tpl->exitO = house->exitO;
    tpl->savedAt = time(nullptr);

    CharacterDatabase.PExecute(
        "UPDATE house_template SET exit_set = %u, exit_x = %f, exit_y = %f, exit_z = %f, exit_o = %f, saved_at = " UI64FMTD " WHERE id = %u",
        tpl->exitSet ? 1 : 0, tpl->exitX, tpl->exitY, tpl->exitZ, tpl->exitO, uint64(tpl->savedAt), tpl->id);
    CharacterDatabase.CommitTransaction();

    savedObjects = uint32(tpl->objects.size());
    savedExit = tpl->exitSet;
    sLog.outInfo("Housing: template %u (%s) saved with %u objects.", tpl->id, tpl->name.c_str(), savedObjects);
    return true;
}

bool HouseMgr::GotoTemplate(Player* dev, uint32 id, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }

    HouseTemplate* tpl = GetTemplate(id);
    if (!tpl)
    {
        error = "There is no template with that id.";
        return false;
    }
    House* house = GetHouseById(tpl->houseId);
    if (!house)
    {
        error = "This template lost its authoring house; it can stamp, but not be visited.";
        return false;
    }
    if (dev->IsInCombat())
    {
        error = "You cannot travel while in combat.";
        return false;
    }
    if (!PrepareInstance(*house))
    {
        error = "Could not open the template.";
        return false;
    }

    // A forced instance, exactly as a guest visit rides: a permanent bind is
    // one per (character, map) and the developer's own house holds theirs.
    // Template instances are never bound, which also means CleanupInstances
    // collects their instance rows every restart -- harmless, because the
    // stale-id check nulls the cache and this reissues one.
    dev->SetForcedInstance(HOUSE_MAP_ID, house->instanceId);

    // Forced map change when hopping template-to-template or house-to-template:
    // same map id, different instance, and the near path would just slide the
    // developer across the room they are already in.
    float ax, ay, az, ao;
    GetArrivalPosition(*house, ax, ay, az, ao);
    uint32 const teleFlags = (dev->GetMapId() == HOUSE_MAP_ID && dev->GetInstanceId() != house->instanceId)
                             ? TELE_TO_FORCE_MAP_CHANGE : 0;
    if (!dev->TeleportTo(HOUSE_MAP_ID, ax, ay, az, ao, teleFlags))
    {
        dev->ClearForcedInstance();
        error = "Could not travel there.";
        return false;
    }
    return true;
}

bool HouseMgr::DeleteTemplate(Player* dev, uint32 id, std::string& error)
{
    HouseTemplate* tpl = GetTemplate(id);
    if (!tpl)
    {
        error = "There is no template with that id.";
        return false;
    }

    // Doors first: an entrance pointing at a deleted template would be a door
    // to nowhere, and refusing is cheaper than orphan handling. Houses already
    // stamped from it keep their copies -- stamping never references back.
    std::string doors;
    for (auto const& itr : m_portals)
        if (itr.second.templateId == id)
        {
            if (!doors.empty())
                doors += ", ";
            doors += std::to_string(itr.second.id);
        }
    if (!doors.empty())
    {
        error = "Entrances " + doors + " still lead here. Retarget or remove them first.";
        return false;
    }

    House* house = GetHouseById(tpl->houseId);

    if (house && dev && GetHouseAt(dev) == house)
    {
        error = "Step outside the template first.";
        return false;
    }

    if (house)
    {
        // Anybody else forced into the authoring instance -- a second
        // developer -- is swept out before the floor goes. Same loop as
        // EvictLapsedVisitors, for the same reason: the forced instance is per
        // player and in memory, so only who is online can be reached, and only
        // who is online could have noticed.
        for (auto const& itr : sObjectAccessor.GetPlayers())
        {
            Player* p = itr.second;
            if (p && house->instanceId &&
                p->GetForcedInstanceId(HOUSE_MAP_ID) == house->instanceId)
            {
                ChatHandler(p).SendSysMessage("This template is being deleted.");
                p->ClearForcedInstance();
                p->TeleportToHomebind();
            }
        }

        std::vector<uint32> const doomed = house->objects;
        for (uint32 guid : doomed)
            PurgeObject(*house, guid);

        if (Map* map = sMapMgr.FindMap(HOUSE_MAP_ID, house->instanceId))
            DespawnExitPortal(map);

        if (house->instanceId)
        {
            CharacterDatabase.PExecute("DELETE FROM instance WHERE id = %u", house->instanceId);
            m_houseByInstance.erase(house->instanceId);
            m_exitPortals.erase(house->instanceId);
        }
        CharacterDatabase.PExecute("DELETE FROM house WHERE id = %u", house->id);
        m_houseByAccount.erase(house->accountId);
        m_houses.erase(house->id);
        house = nullptr;
    }

    CharacterDatabase.PExecute("DELETE FROM house_template_object WHERE template_id = %u", id);
    CharacterDatabase.PExecute("DELETE FROM house_template WHERE id = %u", id);

    m_templateByName.erase(tpl->name);
    m_houseTemplates.erase(id);

    sLog.outInfo("Housing: template %u deleted.", id);
    return true;
}

//== claiming and moving =====================================================

// Copy the frozen rows in: fresh guids, source = TEMPLATE, the template's exit,
// the house's template_id. On a fresh claim the instance does not exist yet, so
// only the GOData is created and OnHouseMapCreated grids the rows when the map
// spins up; on a move with a guest standing inside, the map IS loaded and the
// new furniture appears around them live.
void HouseMgr::StampTemplate(House& house, HouseTemplate const& tpl)
{
    Map* map = house.instanceId ? sMapMgr.FindMap(HOUSE_MAP_ID, house.instanceId) : nullptr;

    uint32 stamped = 0;
    for (HouseObject const& frozen : tpl.objects)
    {
        uint32 const guid = AllocateObjectGuid();
        if (!guid)
        {
            sLog.outError("Housing: out of furniture ids while stamping template %u into house %u.", tpl.id, house.id);
            break;
        }

        HouseObject o = frozen;
        o.guid    = guid;
        o.houseId = house.id;
        o.source  = HOUSE_SOURCE_TEMPLATE;

        // Numbered as it goes in, and AllocateSlot reads the list this loop is
        // filling -- so the stamped furniture comes out 1, 2, 3 in the order
        // the template author placed it.
        o.slot = AllocateSlot(house);

        CharacterDatabase.PExecute(
            "INSERT INTO house_object (id, house_id, slot, go_entry, x, y, z, o, rot0, rot1, rot2, rot3, scale, placed_at, source) "
            "VALUES (%u, %u, %u, %u, %f, %f, %f, %f, %f, %f, %f, %f, %f, " UI64FMTD ", %u)",
            o.guid, o.houseId, o.slot, o.goEntry, o.x, o.y, o.z, o.o,
            o.rot0, o.rot1, o.rot2, o.rot3, o.scale,
            uint64(time(nullptr)), uint32(o.source));

        if (o.scale > 0.0f)
            sGuidObjectScaling.AddOrEdit(
                ObjectGuid(HIGHGUID_GAMEOBJECT, o.goEntry, o.guid).GetRawValue(), o.scale);

        m_objects[guid] = o;
        house.objects.push_back(guid);

        RegisterObjectWithMap(map, m_objects[guid]);
        if (map)
            SpawnObjectNow(map, m_objects[guid]);
        ++stamped;
    }

    house.exitSet = tpl.exitSet;
    house.exitX = tpl.exitX;
    house.exitY = tpl.exitY;
    house.exitZ = tpl.exitZ;
    house.exitO = tpl.exitO;
    house.templateId = tpl.id;

    CharacterDatabase.PExecute(
        "UPDATE house SET exit_set = %u, exit_x = %f, exit_y = %f, exit_z = %f, exit_o = %f, template_id = %u WHERE id = %u",
        house.exitSet ? 1 : 0, house.exitX, house.exitY, house.exitZ, house.exitO, house.templateId, house.id);

    sLog.outInfo("Housing: stamped template %u (%s) into house %u -- %u objects.",
                 tpl.id, tpl.name.c_str(), house.id, stamped);
}

bool HouseMgr::ClaimHouseAt(Player* player, uint32 portalId, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }
    HousePortal const* door = GetPortal(portalId);
    if (!door)
    {
        error = "That entrance is gone.";
        return false;
    }
    // BEFORE anything mutates. SendPlayerHome checks combat too, but by then
    // the house would already exist -- created and never entered.
    if (player->IsInCombat())
    {
        error = "Not while you are in combat.";
        return false;
    }
    // Re-validated here, not only in the menu: the confirm page may be stale
    // by the time it is clicked.
    if (GetHouseByAccount(player->GetSession()->GetAccountId()))
    {
        error = "You already have a home. Use the move option instead.";
        return false;
    }

    House* house = CreateHouse(player->GetSession()->GetAccountId());
    if (!house)
    {
        error = "Could not create your house.";
        return false;
    }

    if (door->templateId)
    {
        if (HouseTemplate const* tpl = GetTemplate(door->templateId))
            StampTemplate(*house, *tpl);
        else
            sLog.outError("Housing: door %u names template %u, which is gone; claimed empty.", portalId, door->templateId);
    }

    // The door you claimed at is your home -- purchase portal and home portal
    // are one concept, which is also what the strict walk-in keys off.
    house->portalId = portalId;
    CharacterDatabase.PExecute("UPDATE house SET portal_id = %u WHERE id = %u", portalId, house->id);

    // NOT "the Verdant Fields". That is the area name on map 169, which houses
    // have not used since they moved to map 28 -- so the one line a player
    // reads at the single most memorable moment in the feature named a place
    // they were not standing in. "Azeroth" is true wherever a door is put, and
    // stays true if houses ever move map again.
    ChatHandler(player).SendSysMessage("A comfy corner of Azeroth is now yours.");

    RememberEntrance(player, portalId);
    return SendPlayerHome(player, error);
}

// Everything goes: the old template's rows are dropped, and what the player
// placed is DELETED -- the furniture chest, when it exists, turns that delete
// into a stow (the stowed column is already waiting). A guest standing inside
// just sees the furniture vanish around them.
//
// ONE house UPDATE comes out of this, not two. CharacterDatabase runs FOUR
// async workers (CharacterDatabase.WorkerThreads in mangosd.conf), so two
// separately queued statements have NO ordering guarantee -- the original
// zero-then-stamp pair raced, the zero sometimes landed second, and the first
// live test produced houses with a stamped shell and template_id = 0. The
// final state is now written once: by StampTemplate when a template applies,
// by the zero row when none does.
void HouseMgr::RebuildHouse(House& house, uint32 templateId)
{
    std::vector<uint32> const doomed = house.objects;
    for (uint32 guid : doomed)
        PurgeObject(house, guid);

    // THE ANIMALS GO WITH THE FURNITURE. A reset that emptied the room and left
    // three rabbits standing in it would be a fine bug to explain and a poor
    // one to ship. They are not stowed the way furniture is -- nothing puts a
    // critter in a crate yet -- so this destroys, and `.house reset` says so.
    PurgeCritters(house);

    if (Map* map = sMapMgr.FindMap(HOUSE_MAP_ID, house.instanceId))
        DespawnExitPortal(map);

    house.exitSet = false;
    house.exitX = house.exitY = house.exitZ = house.exitO = 0.0f;
    house.templateId = 0;

    if (templateId)
        if (HouseTemplate const* tpl = GetTemplate(templateId))
        {
            StampTemplate(house, *tpl);         // writes the one UPDATE
            return;
        }

    CharacterDatabase.PExecute(
        "UPDATE house SET exit_set = 0, exit_x = 0, exit_y = 0, exit_z = 0, exit_o = 0, template_id = 0 WHERE id = %u",
        house.id);
}

// .house reset -- back to the beginning, wherever you stand. A claimed home
// re-stamps from ITS OWN DOOR as it is bound today (retargeting the door and
// resetting is how a shipped template update reaches an existing house); a
// template empties but keeps its exit, because save is what publishes and
// the arrival spot is the one thing worth keeping when starting over.
bool HouseMgr::ResetHouse(Player* player, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }

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

    // Gears down first: purging under a live edit session would leave them
    // marking nothing for a reconcile pass. Immediate beats eventual.
    ClearEdit(player);

    if (house->authorsTemplate)
    {
        std::vector<uint32> const doomed = house->objects;
        for (uint32 guid : doomed)
            PurgeObject(*house, guid);
        // The exit deliberately stays, and the frozen snapshot is untouched
        // until the next save.
        return true;
    }

    // Packed up first, exactly as a move is. A reset destroys the same
    // furniture for the same reason, and the two commands landing differently
    // on the same loss would be the difference no player could predict --
    // "moving keeps my chairs but resetting burns them" is not a rule anybody
    // would guess, and it is not one worth teaching.
    //
    // DELIBERATELY NOT IN RebuildHouse, though both callers reach it. The
    // template branch above returns before this point and must keep destroying
    // outright: emptying a template is an authoring act, its contents belong to
    // the template rather than to the developer standing in it, and filling a
    // dev's own shelf with forty copies of somebody's fabric would be a bug.
    uint32 stowed = 0, lost = 0;
    StowHouseFurniture(*house, stowed, lost);

    HousePortal const* door = GetPortal(house->portalId);
    RebuildHouse(*house, door ? door->templateId : 0);

    ReportStowResult(player, stowed, lost);
    return true;
}

bool HouseMgr::MoveHouseTo(Player* player, uint32 portalId, std::string& error)
{
    if (!m_enabled)
    {
        error = "Housing is not available on this server.";
        return false;
    }
    HousePortal const* door = GetPortal(portalId);
    if (!door)
    {
        error = "That entrance is gone.";
        return false;
    }
    if (player->IsInCombat())
    {
        error = "Not while you are in combat.";
        return false;
    }

    House* house = GetHouseByAccount(player->GetSession()->GetAccountId());
    if (!house)
    {
        error = "You do not have a home yet. Claim this one instead.";
        return false;
    }
    // ALREADY HOME COVERS A TEMPLATE MATCH TOO, which is what stops "Move
    // here" offering to rebuild a house into the fabric it already has -- a
    // purge and a re-stamp that would cost the player everything they placed
    // and leave the room looking identical.
    if (IsHomePortal(*house, portalId))
    {
        error = "This door is already your home.";
        return false;
    }

    // PACK BEFORE THE FLOOR GOES. Moving house used to destroy every piece the
    // player had placed, which is a real cost to charge for a decision whose
    // whole appeal is "I like that door better" -- and the shelf that could
    // hold the furniture was sitting right there, empty, being used for
    // nothing but crates carried in from a vendor.
    //
    // What cannot be saved is still lost, and there are two ways to be in that
    // bucket: the shelf filling up, and a piece that never came from an item at
    // all. The player was told which before they confirmed; this is where it
    // actually happens.
    uint32 stowed = 0, lost = 0;
    StowHouseFurniture(*house, stowed, lost);

    RebuildHouse(*house, door->templateId);

    house->portalId = portalId;
    CharacterDatabase.PExecute("UPDATE house SET portal_id = %u WHERE id = %u", portalId, house->id);

    ChatHandler(player).SendSysMessage("You have moved. The new rooms are yours.");
    ReportStowResult(player, stowed, lost);

    RememberEntrance(player, portalId);
    return SendPlayerHome(player, error);
}

//== furniture ===============================================================

void HouseMgr::RegisterObjectWithMap(Map* map, HouseObject const& obj)
{
    GameObjectData& data = sObjectMgr.NewGOData(obj.guid);
    data.id                = obj.goEntry;
    data.position.mapId    = HOUSE_MAP_ID;
    data.position.x        = obj.x;
    data.position.y        = obj.y;
    data.position.z        = obj.z;
    data.position.o        = obj.o;
    data.rotation0         = obj.rot0;
    data.rotation1         = obj.rot1;
    data.rotation2         = obj.rot2;
    data.rotation3         = obj.rot3;
    data.spawntimesecsmin  = 0;                 // furniture never despawns
    data.spawntimesecsmax  = 0;
    data.animprogress      = GO_ANIMPROGRESS_DEFAULT;
    data.go_state          = GO_STATE_READY;
    data.spawn_flags       = 0;
    data.visibility_mod    = 0.0f;
    data.instanciatedContinentInstanceId = 0;

    // The per-instance grid set, NOT sObjectMgr's. ObjectGridLoader::Visit reads
    // both when a grid loads, so this is the one line that makes a chair belong
    // to one house instead of every house.
    if (map)
        map->GetPersistentState()->AddGameobjectToGrid(obj.guid, &data);
}

GameObject* HouseMgr::SpawnObjectNow(Map* map, HouseObject const& obj)
{
    if (!map || !map->IsLoaded(obj.x, obj.y))
        return nullptr;                         // the grid will load it when it comes in

    GameObject* go = new GameObject;
    if (!go->LoadFromDB(obj.guid, map))
    {
        sLog.outError("Housing: could not spawn object %u (entry %u).", obj.guid, obj.goEntry);
        delete go;
        return nullptr;
    }
    map->Add(go);
    return go;
}

void HouseMgr::OnHouseMapCreated(Map* map)
{
    if (!m_enabled || !map)
        return;

    House* house = GetHouseByInstance(map->GetInstanceId());
    if (!house)
    {
        // Normal for the brief window between GenerateInstanceId and the first
        // entry, and for any instance of this map we did not create.
        return;
    }

    uint32 count = 0;
    for (uint32 guid : house->objects)
    {
        auto itr = m_objects.find(guid);
        if (itr == m_objects.end())
            continue;
        RegisterObjectWithMap(map, itr->second);
        ++count;
    }

    // The animals, on exactly the same footing. Registered, not spawned: the
    // grid loader brings them in when the cell first loads, which is the same
    // deal the furniture gets and the reason neither needs the map to be ready
    // here.
    uint32 critters = 0;
    for (uint32 guid : house->critters)
    {
        auto itr = m_critters.find(guid);
        if (itr == m_critters.end())
            continue;
        RegisterCritterWithMap(map, itr->second);
        ++critters;
    }

    sLog.outInfo("Housing: house %u opened as instance %u with %u objects and %u critters.",
                 house->id, map->GetInstanceId(), count, critters);
}

//== placing, moving and removing ============================================

// "May this player rearrange this house?" -- the ONE answer both ownership
// checks give. The owner may. A developer may inside a template's authoring
// house, because that is what authoring is; nobody owns those synthetic
// accounts, so without the bypass a template could never be furnished at all.
bool HouseMgr::CanEditHouse(Player* player, House const& house) const
{
    if (!player)
        return false;
    if (house.accountId == player->GetSession()->GetAccountId())
        return true;
    return house.authorsTemplate && player->GetSession()->GetSecurity() >= SEC_DEVELOPER;
}

uint32 HouseMgr::CountPlayerObjects(House const& house) const
{
    uint32 count = 0;
    for (uint32 guid : house.objects)
    {
        auto itr = m_objects.find(guid);
        if (itr != m_objects.end() && itr->second.source == HOUSE_SOURCE_PLAYER)
            ++count;
    }
    return count;
}

// Every one of these requires the player to be standing in a house they may
// edit -- their own, or a template they are authoring.
static bool CheckOwner(HouseMgr& mgr, Player* player, House*& house, std::string& error)
{
    house = mgr.GetHouseAt(player);
    if (!house)
    {
        error = "You are not in a house.";
        return false;
    }
    if (!mgr.CanEditHouse(player, *house))
    {
        error = "This is not your house.";
        return false;
    }
    return true;
}

// The one public door to that check, and a deliberately narrow one: the way
// out belongs to the TEMPLATE, not to whoever lives behind it. A stamped house
// gets its exit copied at claim time and nobody moves it afterwards -- not the
// resident, not a developer standing in their front room. So this is CheckOwner
// plus "and you are authoring this one".
//
// A bespoke house (template_id 0 -- the grandfathered ones, and anything a
// door with no template claims) therefore has no way to set an exit at all,
// and keeps the computed fallback GetExitPosition has always returned for a
// house whose owner never chose. That is the intended shape, not a gap.
House* HouseMgr::GetAuthoredTemplateAt(Player* player, std::string& error)
{
    House* house = nullptr;
    if (!CheckOwner(*this, player, house, error))
        return nullptr;

    if (!house->authorsTemplate)
    {
        error = "The way out is part of the house, not the furniture. It is placed in the template.";
        return nullptr;
    }
    return house;
}

//== naming an object ========================================================

// One past the highest slot this house is using. Deliberately computed from
// the rows rather than carried as a counter: a counter is a second source of
// truth for something the rows already say, and it would drift the first time
// a purge emptied a house behind its back.
//
// The consequence is that a deleted object's number comes back on the next
// placement. That is the trade, and it is the right way round: what must NEVER
// happen is an existing object changing number, because that is how "move 3"
// silently means something different than it did a minute ago. A number reused
// after a deletion can only mislead about a thing that no longer exists.
uint32 HouseMgr::AllocateSlot(House const& house) const
{
    uint32 highest = 0;
    for (uint32 guid : house.objects)
    {
        std::map<uint32, HouseObject>::const_iterator itr = m_objects.find(guid);
        if (itr != m_objects.end() && itr->second.slot > highest)
            highest = itr->second.slot;
    }
    return highest + 1;
}

// What a player typed, as a guid. The two namespaces cannot overlap -- guids
// start at HOUSE_GO_GUID_MIN and a house never holds anything like that many
// objects -- so one argument carries both with no prefix and no ambiguity.
// Raw guids are accepted purely so that anything already holding one keeps
// working; nothing prints them any more.
uint32 HouseMgr::ResolveObjectRef(House const& house, uint32 typed) const
{
    if (!typed)
        return 0;

    if (typed >= HOUSE_GO_GUID_MIN)
        return typed;

    for (uint32 guid : house.objects)
    {
        std::map<uint32, HouseObject>::const_iterator itr = m_objects.find(guid);
        if (itr != m_objects.end() && itr->second.slot == typed)
            return guid;
    }
    return 0;
}

namespace
{
    bool IsUpperAscii(char c) { return c >= 'A' && c <= 'Z'; }
    bool IsLowerAscii(char c) { return c >= 'a' && c <= 'z'; }
    bool IsDigitAscii(char c) { return c >= '0' && c <= '9'; }
    bool IsAlphaAscii(char c) { return IsUpperAscii(c) || IsLowerAscii(c); }

    std::string LowerAscii(std::string v)
    {
        for (char& c : v)
            if (IsUpperAscii(c))
                c = char(c - 'A' + 'a');
        return v;
    }
}

// See the header for why this exists at all. The rule, step for step, is the
// addon catalogue's: take the leaf, drop the extension, break the run-together
// words apart, and title-case only the words that carry no case of their own.
// "Wooden Chair (3)". See the header for why every route says it this way.
//
// "that object" rather than an error for a guid that resolves to nothing: this
// is called while composing a line that is ALREADY being sent, and several
// callers read the label BEFORE the row goes away so they can still name what
// they deleted. A caller that needs to know the object exists asks first.
std::string HouseMgr::ObjectLabel(uint32 objectGuid)
{
    HouseObject const* o = sHouseMgr.GetObject(objectGuid);
    if (!o)
        return "that object";

    std::ostringstream out;
    out << PieceName(*o) << " (" << o->slot << ")";
    return out.str();
}

// The item's name when the piece came out of a bag, so the painting from item
// 100109 reads "Framed Painting" and not "Communitypainting 5". Anything with
// no item behind it (.house object add, template stamps) keeps the model name.
std::string HouseMgr::PieceName(HouseObject const& o)
{
    if (o.itemEntry)
        if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(o.itemEntry))
            if (!proto->Name1.empty())
                return proto->Name1;
    return ObjectDisplayName(o.goEntry);
}

std::string HouseMgr::ObjectDisplayName(uint32 goEntry)
{
    GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(goEntry);
    if (!info || info->name.empty())
        return "Something";

    std::string const& raw = info->name;

    // Is this a path at all? Two tests, because neither alone is enough: most
    // rows end in an extension, and a few carry a trailing space that would
    // hide it -- and a path whose extension was stripped upstream still starts
    // with the archive root.
    bool isPath = false;
    {
        std::string tail = raw;
        while (!tail.empty() && (tail[tail.size() - 1] == ' ' || tail[tail.size() - 1] == '\t'))
            tail.erase(tail.size() - 1);

        size_t const dot = tail.find_last_of('.');
        if (dot != std::string::npos)
        {
            std::string const ext = LowerAscii(tail.substr(dot + 1));
            isPath = (ext == "mdx" || ext == "wmo" || ext == "m2");
        }
        if (!isPath && LowerAscii(tail.substr(0, 6)) == "world ")
            isPath = true;
    }

    // A real name, authored by somebody. Leave it exactly as it is: "Wooden
    // Chair" needs nothing done to it, and half this table is like that.
    if (!isPath)
        return raw;

    size_t const cut = raw.find_last_of(" \t\\/");
    std::string leaf = (cut == std::string::npos) ? raw : raw.substr(cut + 1);

    size_t const dot = leaf.find_last_of('.');
    if (dot != std::string::npos)
        leaf.erase(dot);

    // Underscores and dashes are spaces; so are the seams inside a run-together
    // word. Three seams, and they are the three the catalogue generator uses:
    // lower-or-digit before a capital, a letter before a digit, a digit before
    // a letter. "BE_Table_Large01" -> "BE Table Large 01".
    std::string spaced;
    for (size_t i = 0; i < leaf.size(); ++i)
    {
        char const c = leaf[i];
        if (c == '_' || c == '-')
        {
            if (!spaced.empty() && spaced[spaced.size() - 1] != ' ')
                spaced += ' ';
            continue;
        }

        if (i > 0 && !spaced.empty() && spaced[spaced.size() - 1] != ' ')
        {
            char const prev = leaf[i - 1];
            bool const seam = ((IsLowerAscii(prev) || IsDigitAscii(prev)) && IsUpperAscii(c))
                           || (IsAlphaAscii(prev) && IsDigitAscii(c))
                           || (IsDigitAscii(prev) && IsAlphaAscii(c));
            if (seam)
                spaced += ' ';
        }
        spaced += c;
    }

    while (!spaced.empty() && spaced[spaced.size() - 1] == ' ')
        spaced.erase(spaced.size() - 1);
    if (spaced.empty())
        return raw;

    // Title-case ONLY the words that have no case of their own -- all-caps or
    // all-lower. A word that is already mixed was capitalised by the artist who
    // named it, and "GoldshireInn" is more readable left as "Goldshire Inn"
    // than flattened. "BE" becomes "Be", "elfbed" becomes "Elfbed".
    std::string out;
    size_t pos = 0;
    while (pos < spaced.size())
    {
        size_t const end = spaced.find(' ', pos);
        std::string word = spaced.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? spaced.size() : end + 1;

        bool hasUpper = false, hasLower = false;
        for (char const c : word)
        {
            if (IsUpperAscii(c)) hasUpper = true;
            else if (IsLowerAscii(c)) hasLower = true;
        }

        if (!hasUpper || !hasLower)
        {
            word = LowerAscii(word);
            if (!word.empty() && IsLowerAscii(word[0]))
                word[0] = char(word[0] - 'a' + 'A');
        }

        if (!out.empty())
            out += ' ';
        out += word;
    }
    return out;
}

//== the selected object =====================================================
//
// See the header for why this exists. What matters in here is that NOTHING
// cleans up after it: `GetSelectedObject` re-derives validity on every read
// from the house the player is standing in right now, which is the one fact
// that makes every way of going stale -- deletion, clear, reset, moving house,
// visiting somebody else's -- collapse into a single check.

uint32 HouseMgr::GetSelectedObject(Player* player)
{
    if (!player)
        return 0;

    std::map<uint32, uint32>::iterator itr = m_selected.find(player->GetGUIDLow());
    if (itr == m_selected.end())
        return 0;

    House const* here = GetHouseAt(player);
    HouseObject const* o = GetObject(itr->second);

    // The fabric check is here as well as at selection time because a house
    // can be re-stamped underneath a selection: `.house reset` deletes the
    // player rows and lays down template ones, and a guid can come back as
    // something nobody is allowed to move.
    if (!here || !o || o->houseId != here->id || IsHouseFabric(*o))
    {
        m_selected.erase(itr);
        return 0;
    }
    return itr->second;
}

// THE BLINK, and it is now a cue and nothing more.
//
// Remove(go, false) leaves the object alive and merely un-adds it, so the client
// gets a despawn and then a fresh creation block: the object visibly flashes. It
// works on every furniture type, which is why it is here at all. Same dance as
// ScaleObject, and .gobject scale before it -- the 1.12 client builds a
// gameobject from its creation block and does not re-read it, so taking it out
// and putting it back is the only way to make a changed field visible.
//
// IT USED TO DO A SECOND JOB AND NO LONGER DOES. The glow was once
// GO_DYNFLAG_LO_ACTIVATE written per viewer in Object::BuildValuesUpdate, and
// the blink is what DELIVERED it -- a new creation block being the only way the
// client would read the flag. That flag never drew on GENERIC, CHAIR or
// MAP_OBJECT, so the glow is a summoned marker now (HOUSE_SELECT_MARK_ENTRY)
// and the check came out of BuildValuesUpdate entirely.
//
// So calling this no longer makes anything appear. Call it where a flash is the
// message you want, and nowhere else.
void HouseMgr::FlashObject(Player* player, uint32 objectGuid)
{
    if (!player || !objectGuid)
        return;

    HouseObject const* o = GetObject(objectGuid);
    if (!o)
        return;

    Map* map = player->GetMap();
    if (!map)
        return;

    ObjectGuid const fullGuid(HIGHGUID_GAMEOBJECT, o->goEntry, o->guid);
    if (GameObject* go = map->GetGameObject(fullGuid))
    {
        map->Remove(go, false);
        map->Add(go);
        go->Refresh();
    }
}

//== the glow on the selected object =========================================
//
// A summoned gameobject standing where the chosen furniture is, and the honest
// answer to a question the update fields could not settle: GO_DYNFLAG_LO_ACTIVATE
// is written per viewer and reaches the client correctly, and the client draws
// it for some object types and not the ones housing mostly uses.
//
// Unlike the sparkle this is a real object, so it is visible to guests as well
// as to the owner. That is a fair trade for working at all, and arguably right:
// two people decorating a room together both want to see which piece is being
// talked about.

void HouseMgr::DespawnSelectionMarker(Player* player)
{
    if (!player)
        return;

    std::map<uint32, ObjectGuid>::iterator itr = m_selectMarkers.find(player->GetGUIDLow());
    if (itr == m_selectMarkers.end())
        return;

    if (Map* map = player->GetMap())
        if (GameObject* go = map->GetGameObject(itr->second))
            go->Delete();

    m_selectMarkers.erase(itr);
}

// Idempotent, and cheap to over-call: every mutator can invoke it without
// knowing whether the object it just moved was the selected one. It takes the
// old glow down and puts a new one wherever the selection is now -- which is
// also how the glow follows a nudge, since a summon cannot be relocated the way
// a handle is without the client keeping the old position.
void HouseMgr::RefreshSelectionMarker(Player* player)
{
    if (!m_enabled || !player)
        return;

    DespawnSelectionMarker(player);

    // AFTER THE DESPAWN, NEVER BEFORE IT. This is also the path that turns the
    // glow off, so bailing first would leave the last shimmer standing until
    // something else happened to take it down.
    //
    // The selection itself is untouched by the setting: what a bare command
    // acts on is unchanged, it simply stops being drawn. Selecting still blinks
    // the object and every command still names what it acted on, which is what
    // makes this a fair thing to switch off.
    if (!GetSetting(player, HOUSE_SETTING_GLOW))
        return;

    uint32 const selected = GetSelectedObject(player);
    if (!selected)
        return;

    HouseObject const* o = GetObject(selected);
    Map* map = player->GetMap();
    if (!o || !map || !map->IsLoaded(o->x, o->y))
        return;

    GameObject* go = map->SummonGameObject(HOUSE_SELECT_MARK_ENTRY,
                                           o->x, o->y, o->z + m_selectLift, o->o,
                                           0.0f, 0.0f, 0.0f, 0.0f, HOUSE_SELECT_MARK_LIFETIME_SEC, 0);
    if (!go)
        return;

    // COLLISION OFF, the same switch the edit gear needs and for the same
    // reason: this stands exactly where the furniture is, and a marker you can
    // walk into is worse than no marker. SetGoState is the whole of it --
    // UpdateCollisionState only enables the model while the state is READY.
    go->SetGoState(GO_STATE_ACTIVE);

    m_selectMarkers[player->GetGUIDLow()] = go->GetObjectGuid();
}

// Patch the cached template, not the spawned object -- exactly as SetHandleLook
// has to. GameObject::Create reads size and displayId out of goinfo and the
// 1.12 client builds the object from its creation block, so setting either on a
// live object writes a value nobody will ever see. The next summon picks it up,
// which is why this ends by re-making the glow.
void HouseMgr::SetSelectMarkLook(uint32 displayId, float scale, float lift)
{
    GameObjectInfo* info = const_cast<GameObjectInfo*>(sObjectMgr.GetGameObjectInfo(HOUSE_SELECT_MARK_ENTRY));
    if (info)
    {
        if (displayId)
            info->displayId = displayId;
        if (scale > 0.0f)
            info->size = scale;
    }
    m_selectLift = lift;
}

bool HouseMgr::SelectObject(Player* player, uint32 objectGuid, std::string& error)
{
    if (!player)
        return false;

    House const* here = GetHouseAt(player);
    if (!here)
    {
        error = "You are not in a house.";
        return false;
    }

    // Scoped to the house you are standing in, not merely to an id that exists.
    // Selecting is not a permission -- the mutating commands still check who
    // owns the place -- but a selection that can name furniture in a house you
    // are not in is a selection that will surprise somebody later.
    HouseObject const* o = GetObject(objectGuid);
    if (!o || o->houseId != here->id)
    {
        error = "Nothing in this house has that id. .house object list shows them.";
        return false;
    }
    if (IsHouseFabric(*o))
    {
        error = "That came with the house.";
        return false;
    }

    // BOTH blink: the one being let go and the one being taken up. Two flashes
    // read as a handover, which is what it is -- and the one that matters most
    // is the old one, because the object you are no longer aiming at is the one
    // you are about to forget you had.
    uint32 const previous = GetSelectedObject(player);
    m_selected[player->GetGUIDLow()] = objectGuid;

    if (previous && previous != objectGuid)
        FlashObject(player, previous);

    // A grouped piece blinks with its whole group: what a command on it will
    // move is the group, so that is what "this one" means. The glow stays on
    // the one piece.
    std::vector<HouseObject*> members;
    GroupMembers(*o, members);
    for (HouseObject* m : members)
        FlashObject(player, m->guid);
    RefreshSelectionMarker(player);
    return true;
}

// LETTING GO IS NOT AN EVENT WORTH FLASHING. This used to blink the object on
// the way out, which made sense when the blink was also what delivered the glow
// -- back then it was the only way to take one away. It is not any more: the
// marker despawning IS the message, it is unmissable, and it happens on the same
// frame.
//
// What is left over reads as a fault rather than a signal. "Done editing" is the
// common route here, and a chair that flickers as you close its menu looks like
// something went wrong with the chair -- exactly the wrong note to end on for a
// button whose whole job is to say "finished, nothing changed".
//
// Selecting still blinks, and should: there the flash IS the answer to "which
// one", and it arrives with the glow rather than after it.
void HouseMgr::ClearSelection(Player* player)
{
    if (!player)
        return;

    m_selected.erase(player->GetGUIDLow());
    DespawnSelectionMarker(player);
}

//== the chalk mark ==========================================================
//
// A spot on the floor that the next object placed lands on. Set by
// right-clicking the Decorator's Chalk (sql/custom/054) and clicking the
// ground; spent by the placement that uses it.
//
// WHY THIS IS A STORED POINT AND NOT A ONE-STEP GESTURE. Ground targeting is
// raised entirely client-side. Every spell-related SMSG in this build reports a
// cast that is already under way -- SPELL_START, SPELL_GO, CAST_RESULT and the
// rest -- and there is no opcode that induces the client to begin one. So no
// dot command and no gossip option can ever put the cursor into targeting mode;
// using an item can, and an item that places nothing itself has nowhere to put
// the point except here. The two-step is forced by the protocol.
//
// IT IS VISIBLE, AND THAT IS THE WHOLE REASON IT IS ACCEPTABLE. A remembered
// point that silently changes where the next thing lands is exactly the kind of
// invisible state housing keeps arriving at and rejecting. The glow is what
// makes it ordinary instead: the mark is a thing you can see standing on the
// floor, and it goes out when it is spent.

// WHERE A CHALK MARK MAY BE. The same two guards PlaceAtPoint applies, checked
// here as well as there, so a bad point is refused at the moment it is aimed
// rather than remembered and refused later by a command that did not aim it.
static bool CheckMarkPoint(Player* player, float const* at, std::string& error)
{
    if (!player->IsWithinDist3d(at[0], at[1], at[2], HOUSE_PLACE_DISTANCE_MAX))
    {
        error = "That is too far away to chalk.";
        return false;
    }

    if (fabs(at[2] - player->GetPositionZ()) > HOUSE_PLACE_HEIGHT_MAX)
    {
        error = "Aim closer to the ground.";
        return false;
    }
    return true;
}

bool HouseMgr::SetMark(Player* player, float const* at, std::string& error)
{
    if (!m_enabled || !player)
    {
        error = "Housing is not available.";
        return false;
    }

    // The same ownership rule as everything else that changes a house. Chalking
    // is not a mutation, but a mark you could set in somebody else's house
    // would be a mark that fires the moment you got home.
    House* house = nullptr;
    if (!CheckOwner(*this, player, house, error))
        return false;

    // NO POINT MEANS THE CLIENT NEVER RAISED ITS RETICLE, which is a cache
    // problem and not a mistake the player made -- so say which one it is. The
    // 1.12 client holds an item prototype for the life of the process and
    // writes it to WDB\itemcache.wdb; a relog does not clear it.
    if (!at)
    {
        error = "Your client did not send a spot. Quit the game, delete WDB\\itemcache.wdb, and start it again.";
        return false;
    }

    if (!CheckMarkPoint(player, at, error))
        return false;

    ChalkMark m;
    m.houseId = house->id;
    m.x = at[0];
    m.y = at[1];
    m.z = at[2];
    m_marks[player->GetGUIDLow()] = m;

    RefreshMarkMarker(player);
    return true;
}

// Validated on read, exactly like GetSelectedObject and for the same reasons.
// One check here collapses logging out, moving house, visiting somebody else's,
// a reset and a restart -- none of which needs a hook of its own.
//
// THE HOUSE ID IS WHAT MAKES THIS WORK. Map 28 is a single plane and every
// house is stamped at the same coordinates, so a point alone cannot tell one
// house from another: without the id, a mark chalked at home would read as live
// the moment you walked into a friend's.
bool HouseMgr::GetMark(Player* player, float* out)
{
    if (!player)
        return false;

    std::map<uint32, ChalkMark>::iterator itr = m_marks.find(player->GetGUIDLow());
    if (itr == m_marks.end())
        return false;

    House const* here = GetHouseAt(player);
    if (!here || here->id != itr->second.houseId)
    {
        m_marks.erase(itr);
        DespawnMarkMarker(player);
        return false;
    }

    if (out)
    {
        out[0] = itr->second.x;
        out[1] = itr->second.y;
        out[2] = itr->second.z;
    }
    return true;
}

void HouseMgr::ClearMark(Player* player)
{
    if (!player)
        return;

    m_marks.erase(player->GetGUIDLow());
    DespawnMarkMarker(player);
}

void HouseMgr::DespawnMarkMarker(Player* player)
{
    if (!player)
        return;

    std::map<uint32, ObjectGuid>::iterator itr = m_markMarkers.find(player->GetGUIDLow());
    if (itr == m_markMarkers.end())
        return;

    if (Map* map = player->GetMap())
        if (GameObject* go = map->GetGameObject(itr->second))
            go->Delete();

    m_markMarkers.erase(itr);
}

// Idempotent, like RefreshSelectionMarker, and a near-copy of it on purpose:
// the two are the same idea pointed at different things, and a shared routine
// taking "an object or a point" would be one function with two unrelated halves.
//
// NOT GATED ON HOUSE_SETTING_GLOW, and that is the one real difference. Turning
// the selection shimmer off is fair because selecting still blinks the object
// and every command still names what it acted on -- the glow is a convenience
// on top of feedback that survives without it. A chalk mark has none of that:
// there is no object to blink and nothing to name, so with the glow off the
// mark would be genuinely invisible state. The setting governs decoration, not
// the only way to see something.
void HouseMgr::RefreshMarkMarker(Player* player)
{
    if (!m_enabled || !player)
        return;

    DespawnMarkMarker(player);

    float at[3];
    if (!GetMark(player, at))
        return;

    Map* map = player->GetMap();
    if (!map || !map->IsLoaded(at[0], at[1]))
        return;

    GameObject* go = map->SummonGameObject(HOUSE_SELECT_MARK_ENTRY,
                                           at[0], at[1], at[2] + HOUSE_CHALK_MARK_LIFT, 0.0f,
                                           0.0f, 0.0f, 0.0f, 0.0f, HOUSE_SELECT_MARK_LIFETIME_SEC, 0);
    if (!go)
        return;

    // Collision off, the same one-line reason the selection glow needs it: this
    // stands exactly where you are about to put furniture, and a mark you can
    // walk into is worse than no mark.
    go->SetGoState(GO_STATE_ACTIVE);

    m_markMarkers[player->GetGUIDLow()] = go->GetObjectGuid();
}

// The seam the chalk's item script comes in through, the same shape as
// HouseUseFurnitureItem next door: src/game/Housing is not on the scripts
// project's include path, so the point crosses as three bare floats and the
// entry point is declared `extern` at the call site.
//
// The chalk is never consumed, so nothing is riding on the return value today.
// It is a bool anyway because the script that calls it is one line away from
// wanting to know, and a void here would be the thing to change first.
bool HouseUseChalk(Player* player, bool haveSpot, float x, float y, float z)
{
    if (!player)
        return false;

    float spot[3] = { x, y, z };

    std::string error;
    if (!sHouseMgr.SetMark(player, haveSpot ? spot : nullptr, error))
    {
        ChatHandler(player).SendSysMessage(error.c_str());
        return false;
    }

    ChatHandler(player).SendSysMessage("Chalk mark set. The next thing you place lands on it.");
    return true;
}

// WHERE THE OBJECT ENDS UP -- two routines rather than one branch inside
// PlaceObject, because the reasoning behind each is long and they share nothing
// but the row they fill in.

// The old way, and still the one `.house object add` and a client that sent no
// clicked point both use: `distance` yards along your facing.
//
// YOUR OWN HEIGHT, ALWAYS. THE GROUND IS NEVER ASKED.
//
// There was a snap here until 2026-09-04 -- measure how far the player was off
// the queryable surface, and if they were standing on it, drop the object to
// that surface at the target x/y so furniture followed a slope. Every part of
// that reasoning is about terrain, and housing has none: map 28 is a single
// plane at z = 0 and `map->GetHeight` answers 0 from any height, WMO floors
// being invisible to it. So the snap could only ever do two things here, and
// neither is wanted:
//
//   out on the plain, where the player is at z = 0 too, nothing -- it moved
//   the object to the height it was already at;
//
//   and on anything under a yard tall that the player climbed onto -- a step,
//   a low platform, the lip of a placed building -- it read them as "standing
//   on the ground", found 0 under the target point and SANK the object into
//   the plain, a foot below their feet, for no reason they could see.
//
// The rule is now the one a person would state: it goes where you are standing.
// Nothing about a real-terrain house map survives this; if one ever exists,
// follow the ground there rather than restoring a check that was inert.
static void PlaceAheadOfPlayer(Player* player, float distance, HouseObject& o)
{
    float const face = player->GetOrientation();

    o.x = player->GetPositionX() + cos(face) * distance;
    o.y = player->GetPositionY() + sin(face) * distance;
    o.z = player->GetPositionZ();
}

// Where you pointed. The client raises its ground reticle for any item whose
// spell carries TARGET_FLAG_DEST_LOCATION and sends back the point it drew the
// circle on; see item_house_furniture.cpp for the whole route.
//
// TAKE THAT Z AS SENT AND DO NOT SNAP IT. This is the opposite of the rule
// above and it is not an inconsistency -- it is the same goal with better
// information. The snap exists because the server has to guess what you were
// standing over. Here it does not have to guess: the client drew that circle
// on the geometry it is rendering, WMO floors included, which is exactly what
// map->GetHeight cannot see. On map 28 re-deriving the height would answer 0
// for every point on the map and drop the chair through the inn floor onto the
// plain below.
//
// The two guards are not aim assistance. The spell's own 25-yard range means an
// honest client can never trip either; they are here so a crafted packet cannot
// furnish the far side of the map, or hang a bookshelf in the sky.
static bool PlaceAtPoint(Player* player, float const* at, HouseObject& o, std::string& error)
{
    if (!player->IsWithinDist3d(at[0], at[1], at[2], HOUSE_PLACE_DISTANCE_MAX))
    {
        error = "That is too far away.";
        return false;
    }

    if (fabs(at[2] - player->GetPositionZ()) > HOUSE_PLACE_HEIGHT_MAX)
    {
        error = "Aim closer to the ground.";
        return false;
    }

    o.x = at[0];
    o.y = at[1];
    o.z = at[2];
    return true;
}

bool HouseMgr::PlaceObject(Player* player, uint32 goEntry, float distance, std::string& error, uint32 itemEntry, float scale, float const* at)
{
    House* house = nullptr;
    if (!CheckOwner(*this, player, house, error))
        return false;

    // Player-placed rows only: what a template stamped in is the house's
    // fabric and rides for free, or a rich template would eat the whole
    // decorating budget before the owner placed a thing.
    if (CountPlayerObjects(*house) >= HOUSE_MAX_OBJECTS)
    {
        error = "Your house is full. Remove something first.";
        return false;
    }

    GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(goEntry);
    if (!info)
    {
        error = "There is no object with that id.";
        return false;
    }
    if (info->displayId && !sGameObjectDisplayInfoStore.LookupEntry(info->displayId))
    {
        error = "That object has no model and would be invisible.";
        return false;
    }

    if (distance < 0.0f) distance = 0.0f;
    if (distance > HOUSE_PLACE_DISTANCE_MAX) distance = HOUSE_PLACE_DISTANCE_MAX;

    Map* map = player->GetMap();
    float const face = player->GetOrientation();

    HouseObject o;
    o.houseId = house->id;
    o.goEntry = goEntry;
    // SPAWNED TURNED TO FACE YOU, which is half a turn off the way you are
    // looking. Setting it to your own orientation points the model the same way
    // you are pointing -- so you place a chair and are looking at the back of
    // it, and the first thing anybody does is turn it round.
    //
    // Note this makes the placement default what the turn page calls "Face Away
    // from Player", not "Face Player". Those two labels are named for the
    // orientation VALUE and not for what you end up seeing, and which way a
    // model actually faces is the artist's choice -- plenty of doodads have
    // their front on the far side. Left alone deliberately: the buttons are a
    // pair and either can be the one you want.
    o.o = MapManager::NormalizeOrientation(face + M_PI_F);

    // THREE ANSWERS TO "WHERE", RESOLVED IN ONE PLACE, and this is that place:
    // the point the caller was handed, then the chalk mark, then `distance`
    // yards ahead. The mark is consulted HERE rather than at the two call sites
    // for two reasons -- there is then exactly one rule about where an object
    // ends up, and the mark can only be spent by a placement that really
    // happened, which is what makes "it goes out when it is used" true without
    // anybody having to remember to clear it.
    //
    // AN EXPLICIT CLICK BEATS A STANDING MARK. A crate that raised its own
    // reticle has already been aimed, so its point wins and the mark is left
    // alone for the next thing that has no aim of its own.
    float mark[3];
    bool const usedMark = !at && GetMark(player, mark);
    if (usedMark)
        at = mark;

    // WORK OUT WHERE IT GOES BEFORE TAKING AN ID. PlaceAtPoint can refuse, and
    // AllocateObjectGuid is a counter that never comes back down -- so doing it
    // the other way round burns a furniture id every time somebody aims badly.
    //
    // A MARK IS RE-CHECKED HERE, not trusted from when it was set. It was
    // already guarded once by CheckMarkPoint, so this can only ever fire
    // because the player walked away from their own mark -- and refusing is
    // then the right answer. Falling back to two-yards-ahead would put the
    // object somewhere they did not ask for, at the exact moment they were
    // being most specific about where they wanted it.
    if (at)
    {
        if (!PlaceAtPoint(player, at, o, error))
            return false;
    }
    else
        PlaceAheadOfPlayer(player, distance, o);

    // ONE LIFT, AFTER ALL THREE ROUTES AGREE ON A HEIGHT. Here rather than
    // inside each of them because it is the same correction for the same
    // reason every time -- see HOUSE_PLACE_LIFT -- and because a route that
    // forgot it would only show up on flat models, months later, as a rug that
    // flickers in one house and not another.
    o.z += HOUSE_PLACE_LIFT;

    o.guid = AllocateObjectGuid();
    if (!o.guid)
    {
        error = "Out of furniture ids.";
        return false;
    }
    uint32 const guid = o.guid;

    // Zero rotation, exactly as .gobject add does -- GameObject::Create derives
    // the quaternion from the orientation.
    o.rot0 = o.rot1 = o.rot2 = o.rot3 = 0.0f;
    o.scale = scale;                            // 0 = the template's own size
    o.source = HOUSE_SOURCE_PLAYER;             // template rows come from StampTemplate
    o.slot   = AllocateSlot(*house);
    o.itemEntry = itemEntry;                    // 0 unless it came out of a bag

    CharacterDatabase.PExecute(
        "INSERT INTO house_object (id, house_id, slot, go_entry, item_entry, x, y, z, o, rot0, rot1, rot2, rot3, scale, placed_at, source) "
        "VALUES (%u, %u, %u, %u, %u, %f, %f, %f, %f, 0, 0, 0, 0, %f, " UI64FMTD ", %u)",
        o.guid, o.houseId, o.slot, o.goEntry, o.itemEntry, o.x, o.y, o.z, o.o, o.scale, uint64(time(nullptr)), uint32(o.source));

    // house_object is the authority on size, the same way the loader treats it.
    if (o.scale > 0.0f)
        sGuidObjectScaling.AddOrEdit(
            ObjectGuid(HIGHGUID_GAMEOBJECT, o.goEntry, o.guid).GetRawValue(), o.scale);

    m_objects[guid] = o;
    house->objects.push_back(guid);

    RegisterObjectWithMap(map, o);
    SpawnObjectNow(map, o);

    // WHAT YOU JUST PLACED IS WHAT YOU MEANT. Placing then nudging is the
    // whole decorating loop, and the alternative -- the new piece competing
    // with everything else already within ten yards for "nearest" -- is
    // exactly the ambiguity the selection exists to end. This is the one
    // auto-selection in housing, and it is safe because the intent is not in
    // any doubt: you asked for this object a moment ago.
    m_selected[player->GetGUIDLow()] = guid;

    // AND IT IS ARRANGEABLE AT ONCE, edit mode or not: AddToEdit starts a
    // per-object session when there is none, so the piece can be right-clicked
    // the moment it lands and the menu's Done ends that session again.
    //
    // ONE FLASH, NOT TWO. Its Reconcile pass re-sends the object as a GOOBER,
    // which is the same despawn-and-respawn the selection blink used to do
    // here -- so the explicit FlashObject that stood on this line was a second
    // one in the same frame, on an object the client had only just been given.
    AddToEdit(player, guid);
    RefreshSelectionMarker(player);

    // SPENT, AND ONLY NOW. Everything above this line can refuse -- a full
    // house, an entry with no model, a mark you have walked away from -- and
    // every one of those leaves the mark standing, so the chalk is not
    // something you can lose by aiming at a house that turned out to be full.
    if (usedMark)
        ClearMark(player);

    return true;
}

// THE ONE PATH THAT REARRANGES A PIECE: position, facing and size in one
// take-out-and-put-back, one row update and one GameObjectData update. Drag,
// move, turn and resize all come here, for one piece or for a whole group, so
// they cannot drift apart.
//
// Each part runs only when it changed. A plain move must not call
// UpdateRotationFields, which rebuilds the quaternion from the yaw alone.
void HouseMgr::ApplyPlacement(Map* map, Placement const& p)
{
    HouseObject& o = *p.obj;
    float const no = MapManager::NormalizeOrientation(p.o);
    bool const moved   = p.x != o.x || p.y != o.y || p.z != o.z;
    bool const turned  = no != o.o;
    bool const resized = p.scale > 0.0f && p.scale != o.scale;
    if (!moved && !turned && !resized)
        return;

    GameObjectData* data = const_cast<GameObjectData*>(sObjectMgr.GetGOData(o.guid));

    // Remove using the OLD position: the cell is computed from the data, so
    // this has to happen before the coordinates change.
    if (moved && data && map)
        map->GetPersistentState()->RemoveGameobjectFromGrid(o.guid, data);

    o.x = p.x; o.y = p.y; o.z = p.z; o.o = no;
    if (resized)
        o.scale = p.scale;

    if (data)
    {
        data->position.x = o.x;
        data->position.y = o.y;
        data->position.z = o.z;
        data->position.o = o.o;
        if (moved && map)
            map->GetPersistentState()->AddGameobjectToGrid(o.guid, data);
    }

    CharacterDatabase.PExecute("UPDATE house_object SET x = %f, y = %f, z = %f, o = %f, scale = %f WHERE id = %u",
                               o.x, o.y, o.z, o.o, o.scale, o.guid);

    // See ScaleObject for why the core's own registry carries the size.
    ObjectGuid const fullGuid(HIGHGUID_GAMEOBJECT, o.goEntry, o.guid);
    if (resized)
        sGuidObjectScaling.AddOrEdit(fullGuid.GetRawValue(), o.scale);

    if (!map)
        return;

    // Remove and re-Add the SAME object rather than deleting and respawning it.
    //
    // GameObject::Delete() does not remove anything at once. It queues the
    // object on the map removal list, which is drained at the end of the map
    // update. A replacement created with the same guid in between is then torn
    // out by the deferred cleanup, because the map indexes objects by guid.
    // Symptom: the object vanishes on resize and never comes back until the
    // grid reloads.
    //
    // Remove(go, false) leaves the object alive and takes it out of the world,
    // so the client gets a despawn and then a fresh creation block from Add.
    // That is what .gobject scale does (Commands.cpp:9620).
    if (GameObject* go = map->GetGameObject(fullGuid))
    {
        map->Remove(go, false);
        go->Relocate(o.x, o.y, o.z, o.o);
        if (moved)
        {
            go->SetFloatValue(GAMEOBJECT_POS_X, o.x);
            go->SetFloatValue(GAMEOBJECT_POS_Y, o.y);
            go->SetFloatValue(GAMEOBJECT_POS_Z, o.z);
        }
        if (turned)
            go->SetFloatValue(GAMEOBJECT_FACING, o.o);
        if (resized)
            go->SetObjectScale(o.scale);

        // GAMEOBJECT_FACING alone does nothing visible. The client renders a
        // gameobject from the ROTATION quaternion, and UpdateRotationFields
        // is what recomputes it from the orientation just set -- it is the
        // second half of what GameObject::Create does and is easy to miss,
        // because the facing field looks like it ought to be enough.
        // Without it the server, the database and .house list all agree the
        // object turned while the model never moves.
        if (turned || resized)
            go->UpdateRotationFields();

        map->Add(go);
        go->Refresh();
    }
    else if (resized)
    {
        // Not spawned: its grid is not loaded. Create reads the registry when
        // the grid comes in.
        SpawnObjectNow(map, o);
    }
}

// Check every placement, then apply every one. One placement off the map
// refuses the lot, so a group never half-moves.
//
// The map check is the same guard OffsetObject has always had, and here it is
// the only one: a turn or a resize can carry a far member a long way, and
// that is not a typo the way a large nudge is. An off-map row crashes the
// server at the owner's next login (HOUSE_MAX_NUDGE has the story).
bool HouseMgr::PlaceAll(Map* map, std::vector<Placement> const& all, std::string& error)
{
    for (Placement const& p : all)
        if (!MaNGOS::IsValidMapCoord(p.x, p.y, p.z, p.o))
        {
            error = all.size() > 1 ? "Too far for one of the pieces." : "Too far.";
            return false;
        }

    for (Placement const& p : all)
        ApplyPlacement(map, p);
    return true;
}

// Look the object up, confirm it is in the house the player is standing in, and
// confirm it is theirs to rearrange.
static HouseObject* Resolve(HouseMgr& mgr, Player* player, uint32 guid, House*& house,
                            std::map<uint32, HouseObject>& objects, std::string& error)
{
    if (!CheckOwner(mgr, player, house, error))
        return nullptr;

    auto itr = objects.find(guid);
    if (itr == objects.end() || itr->second.houseId != house->id)
    {
        error = "That object is not in this house.";
        return nullptr;
    }

    // Everything that rearranges furniture -- drag, move, rotate, scale, and
    // the gear menu behind all four -- comes through here, which is why the
    // fabric gate belongs at this one point rather than at each verb.
    if (IsHouseFabric(itr->second))
    {
        error = "That came with the house.";
        return nullptr;
    }

    return &itr->second;
}

// .house object drag -- snap the object to where you are standing. The blunt one.
bool HouseMgr::DragObject(Player* player, uint32 guid, std::string& error)
{
    House* house = nullptr;
    HouseObject* o = Resolve(*this, player, guid, house, m_objects, error);
    if (!o)
        return false;

    // The same hair off the floor a fresh placement gets, and for the same
    // reason: drag is "put it where I am standing", so it lands coplanar with
    // whatever is under the player's feet unless it is lifted. Measured from
    // the PLAYER, not from the object, so dragging a rug back and forth across
    // a room does not walk it into the ceiling.
    //
    // A GROUP COMES ALONG: the clicked piece lands at the player's feet and
    // every other member keeps its place relative to it.
    float const dx = player->GetPositionX() - o->x;
    float const dy = player->GetPositionY() - o->y;
    float const dz = player->GetPositionZ() + HOUSE_PLACE_LIFT - o->z;

    std::vector<HouseObject*> members;
    GroupMembers(*o, members);

    std::vector<Placement> all;
    for (HouseObject* m : members)
        all.push_back({ m, m->x + dx, m->y + dy, m->z + dz, m->o, 0.0f });

    if (!PlaceAll(player->GetMap(), all, error))
        return false;
    RefreshSelectionMarker(player);
    return true;
}

// .house object move <x> <y> <z> -- nudge by an offset in yards along the world axes,
// which is the only way to place something you cannot stand on top of. Z is up.
bool HouseMgr::OffsetObject(Player* player, uint32 guid, float dx, float dy, float dz, std::string& error)
{
    House* house = nullptr;
    HouseObject* o = Resolve(*this, player, guid, house, m_objects, error);
    if (!o)
        return false;

    // A nudge is measured in yards across a room. Anything bigger is a typo,
    // and a coordinate off the map is worse than lost: WorldObject::Relocate
    // asserts on it inside GameObject::Create BEFORE the position check that
    // would have skipped the object, so the row crashes the server on every
    // login of the house's owner from then on. One object at z = -800046 did
    // exactly that on 2026-09-04.
    //
    // PlaceAll checks the map coordinate of every member.
    if (std::fabs(dx) > HOUSE_MAX_NUDGE || std::fabs(dy) > HOUSE_MAX_NUDGE || std::fabs(dz) > HOUSE_MAX_NUDGE)
    {
        error = "Too far.";
        return false;
    }

    std::vector<HouseObject*> members;
    GroupMembers(*o, members);

    std::vector<Placement> all;
    for (HouseObject* m : members)
        all.push_back({ m, m->x + dx, m->y + dy, m->z + dz, m->o, 0.0f });

    if (!PlaceAll(player->GetMap(), all, error))
        return false;

    // NOTHING TO CHASE ANY MORE. A gear used to stand beside each object and
    // had to be moved with it -- the rule was "whatever moves a piece of
    // furniture moves its handle", and it lived here rather than in the gossip
    // handler because the chat commands and the addon's pad bypass that. The
    // furniture is its own handle now, so it takes its menu with it for free.
    RefreshSelectionMarker(player);
    return true;
}

// .house object rotate <degrees> -- spin by that much. With faceMe, point it the way the
// player is pointing instead.
// faceMe picks what the angle is measured FROM: the way the player is pointing,
// or the way the object already points. Everything else is the same rotation,
// which is what lets "face away from me" be "face me, plus 180" rather than a
// third code path.
bool HouseMgr::TurnObject(Player* player, uint32 guid, float degrees, bool faceMe, std::string& error)
{
    House* house = nullptr;
    HouseObject* o = Resolve(*this, player, guid, house, m_objects, error);
    if (!o)
        return false;

    float const from = faceMe ? player->GetOrientation() : o->o;

    // A GROUP SWINGS AROUND THE CLICKED PIECE. Every member turns by the same
    // angle, and its spot turns by that angle about the pivot, so a chair in
    // front of the table stays in front of it. "Face me" needs nothing of its
    // own: it only picks the angle the pivot ends up at.
    float const theta = from + degrees * (M_PI_F / 180.0f) - o->o;
    float const c = cos(theta);
    float const s = sin(theta);

    std::vector<HouseObject*> members;
    GroupMembers(*o, members);

    std::vector<Placement> all;
    for (HouseObject* m : members)
    {
        float const rx = m->x - o->x;
        float const ry = m->y - o->y;
        all.push_back({ m, o->x + rx * c - ry * s, o->y + rx * s + ry * c, m->z, m->o + theta, 0.0f });
    }

    if (!PlaceAll(player->GetMap(), all, error))
        return false;
    // The glow is re-summoned after every change, turns included: see
    // RefreshSelectionMarker.
    RefreshSelectionMarker(player);
    return true;
}

// .house object scale -- resize one piece of furniture.
//
// THE CORE ALREADY HAS PER-OBJECT SCALING and it would have been a mistake to
// build a second one. GameObject::Create ends with
//
//     SetObjectScale(sGuidObjectScaling.GetScale(GetGUID(), goinfo->size))
//
// so a scale registered against a full guid is applied every time the object is
// created -- including every grid reload, for free. tw_world.object_scaling is
// its table and .gobject scale is its other caller. gameobject_template.size
// was never an option: it is shared by every object of an entry, so resizing
// one chair would resize all of them in every house.
//
// house_object keeps its own copy anyway, because tw_char.house is the
// authority for housing and a house that came back without its proportions
// after somebody tidied the world DB would be a nasty surprise. LoadFromDB
// pushes those values back into the registry at startup, so the two cannot
// drift.
//
// The object is respawned rather than adjusted in place. That is what
// .gobject scale does too -- Remove, set, Add -- because the 1.12 client builds
// a gameobject from its creation block and will not re-render one already on
// screen. Same reason .house object handle had to patch the template.
bool HouseMgr::ScaleObject(Player* player, uint32 guid, float factor, bool relative, std::string& error)
{
    House* house = nullptr;
    HouseObject* o = Resolve(*this, player, guid, house, m_objects, error);
    if (!o)
        return false;

    GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(o->goEntry);
    float const base = info && info->size > 0.0f ? info->size : 1.0f;

    // A stored 0 means "never resized", so a relative step starts from the
    // template's own size rather than from zero.
    float const current = o->scale > 0.0f ? o->scale : base;
    float wanted = relative ? current * factor : base * factor;

    // Clamped to the range .gobject scale enforces, and floored rather than
    // rejected: a slip on a x0.5 button should not shrink something to nothing.
    if (wanted < 0.1f) wanted = 0.1f;
    if (wanted > 50.0f) wanted = 50.0f;

    // A GROUP SCALES AS A SET: every member by the same factor, and every gap
    // to the clicked piece by it too, height included, so a table and chairs
    // at twice the size are a table and chairs at twice the size. The factor
    // comes from the clicked piece after its own clamp, which covers "Set
    // size" and "Reset size" as well.
    //
    // A member that would pass a limit refuses the lot. Flooring it the way
    // the clicked piece is floored would change the set's proportions.
    float const f = wanted / current;

    std::vector<HouseObject*> members;
    GroupMembers(*o, members);

    std::vector<Placement> all;
    for (HouseObject* m : members)
    {
        if (m == o)
        {
            all.push_back({ m, m->x, m->y, m->z, m->o, wanted });
            continue;
        }

        GameObjectInfo const* mInfo = sObjectMgr.GetGameObjectInfo(m->goEntry);
        float const mBase = mInfo && mInfo->size > 0.0f ? mInfo->size : 1.0f;
        float const mWanted = (m->scale > 0.0f ? m->scale : mBase) * f;
        if (mWanted < 0.1f || mWanted > 50.0f)
        {
            error = mWanted < 0.1f ? "Too small for one of the pieces." : "Too big for one of the pieces.";
            return false;
        }
        all.push_back({ m, o->x + (m->x - o->x) * f, o->y + (m->y - o->y) * f, o->z + (m->z - o->z) * f,
                        m->o, mWanted });
    }

    if (!PlaceAll(player->GetMap(), all, error))
        return false;

    // The object under the handle just changed shape; the handle itself sits a
    // fixed distance above the anchor and has not moved. Refreshed anyway, the
    // same as the three above.
    RefreshSelectionMarker(player);
    return true;
}

// The teardown half of removal, with no player in sight: live object, grid
// entry, GOData, database row, memory. Shared by RemoveObject (player standing
// there), MoveHouseTo and DeleteTemplate (nobody inside -- the map is found by
// instance id and may legitimately be unloaded, in which case only the
// persistent side needs cleaning).
void HouseMgr::PurgeObject(House& house, uint32 guid)
{
    auto itr = m_objects.find(guid);
    if (itr == m_objects.end())
        return;

    Map* map = sMapMgr.FindMap(HOUSE_MAP_ID, house.instanceId);

    if (map)
        if (GameObject* go = map->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, itr->second.goEntry, guid)))
        {
            go->SetRespawnTime(0);              // do not persist a respawn
            go->Delete();
        }

    if (GameObjectData* data = const_cast<GameObjectData*>(sObjectMgr.GetGOData(guid)))
        if (map)
            map->GetPersistentState()->RemoveGameobjectFromGrid(guid, data);

    sObjectMgr.DeleteGOData(guid);
    CharacterDatabase.PExecute("DELETE FROM house_object WHERE id = %u", guid);

    house.objects.erase(std::remove(house.objects.begin(), house.objects.end(), guid), house.objects.end());
    m_objects.erase(itr);
}

bool HouseMgr::RemoveObject(Player* player, uint32 guid, std::string& error, uint32* returnedItem)
{
    if (returnedItem)
        *returnedItem = 0;

    House* house = nullptr;
    if (!CheckOwner(*this, player, house, error))
        return false;

    auto itr = m_objects.find(guid);
    if (itr == m_objects.end() || itr->second.houseId != house->id)
    {
        error = "That object is not in this house.";
        return false;
    }

    // Deleting the inn you live in is not decorating. The rule and the reason
    // it has no developer bypass live on IsHouseFabric; this was the first of
    // the five gates, and moving/turning/scaling joined it in Resolve once it
    // turned out a resident could still drag the walls apart. The internal
    // paths (MoveHouseTo, DeleteTemplate) go through PurgeObject and are
    // unaffected.
    if (IsHouseFabric(itr->second))
    {
        error = "That came with the house.";
        return false;
    }

    // GIVE THE ITEM BACK BEFORE ANYTHING IS DESTROYED. A piece of furniture
    // that came out of a bag goes back into one, and the order is the whole
    // safety of it: CanStoreNewItem first, and a refusal leaves the object
    // standing exactly where it was. Purging first and then discovering there
    // is no room would destroy something a player paid for.
    //
    // Rows with itemEntry 0 -- .house object add, and anything stamped from a
    // template -- are destroyed as they always were. There is no item to
    // return, so there is nothing to decide.
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

    // Before the object goes, or the handle outlives what it was marking. This
    // covers every route in -- the gear's own menu, .house object del, and the
    // addon button -- rather than only the one that happened to be written
    // first.
    DropFromEdit(player, guid);

    PurgeObject(*house, guid);

    // AFTER the purge, so GetSelectedObject can no longer find the row and the
    // refresh resolves to "nothing selected" -- which takes the glow down. Run
    // before it, the object would still be there and the glow would be put
    // straight back on something about to vanish.
    RefreshSelectionMarker(player);
    return true;
}

//== furniture groups ========================================================
//
// VALIDATED ON READ, like the selection. A group is the set of loaded,
// non-fabric pieces of one house sharing a groupNo, and it is live only with
// two or more. Picking up, a purge, a reset and a move of house all change the
// set without any group code running, and the next read sees the result.

void HouseMgr::GroupMembers(HouseObject const& o, std::vector<HouseObject*>& out)
{
    out.clear();

    auto self = m_objects.find(o.guid);
    if (self == m_objects.end())
        return;
    out.push_back(&self->second);

    House* house = GetHouseById(o.houseId);
    if (!o.groupNo || IsHouseFabric(o) || !house)
        return;

    for (uint32 guid : house->objects)
    {
        if (guid == o.guid)
            continue;
        auto itr = m_objects.find(guid);
        if (itr != m_objects.end() && itr->second.groupNo == o.groupNo && !IsHouseFabric(itr->second))
            out.push_back(&itr->second);
    }
}

uint32 HouseMgr::GroupSize(uint32 guid)
{
    HouseObject const* o = GetObject(guid);
    if (!o)
        return 0;

    std::vector<HouseObject*> members;
    GroupMembers(*o, members);
    return uint32(members.size());
}

// Counts stale numbers too: a lone piece still holding group 4 must not end
// up in a new group 4 with two pieces it was never grouped with.
uint32 HouseMgr::AllocateGroupNo(House const& house) const
{
    uint32 highest = 0;
    for (uint32 guid : house.objects)
    {
        std::map<uint32, HouseObject>::const_iterator itr = m_objects.find(guid);
        if (itr != m_objects.end() && itr->second.groupNo > highest)
            highest = itr->second.groupNo;
    }
    return highest + 1;
}

bool HouseMgr::JoinGroup(Player* player, uint32 guid, uint32 withGuid, std::string& error)
{
    if (guid == withGuid)
    {
        error = "Choose a different piece to group with.";
        return false;
    }

    House* house = nullptr;
    HouseObject* o = Resolve(*this, player, guid, house, m_objects, error);
    if (!o)
        return false;
    HouseObject* with = Resolve(*this, player, withGuid, house, m_objects, error);
    if (!with)
        return false;

    std::vector<HouseObject*> mine, theirs;
    GroupMembers(*o, mine);
    GroupMembers(*with, theirs);

    if (std::find(mine.begin(), mine.end(), with) != mine.end())
    {
        error = "Those are already grouped.";
        return false;
    }
    if (mine.size() + theirs.size() > HOUSE_GROUP_MAX)
    {
        error = "A group holds at most " + std::to_string(HOUSE_GROUP_MAX) + " pieces.";
        return false;
    }

    // The number of the group being joined, else the clicked piece's own,
    // else a new one. Fewest rows change that way.
    uint32 target = 0;
    if (theirs.size() > 1)
        target = with->groupNo;
    else if (mine.size() > 1)
        target = o->groupNo;
    else
        target = AllocateGroupNo(*house);

    // One row per piece. Each write is to a different row, so the order the
    // four async workers run them in does not matter.
    mine.insert(mine.end(), theirs.begin(), theirs.end());
    for (HouseObject* m : mine)
    {
        if (m->groupNo != target)
        {
            m->groupNo = target;
            CharacterDatabase.PExecute("UPDATE house_object SET group_no = %u WHERE id = %u", target, m->guid);
        }
        FlashObject(player, m->guid);
    }
    RefreshSelectionMarker(player);
    return true;
}

bool HouseMgr::LeaveGroup(Player* player, uint32 guid, std::string& error)
{
    House* house = nullptr;
    HouseObject* o = Resolve(*this, player, guid, house, m_objects, error);
    if (!o)
        return false;

    std::vector<HouseObject*> members;
    GroupMembers(*o, members);
    if (members.size() < 2)
    {
        error = ObjectLabel(guid) + " is not in a group.";
        return false;
    }

    // members[0] is o. With two pieces, the other one is let go as well:
    // a group of one is no group, and a stale number would show in the list.
    size_t const release = members.size() == 2 ? 2 : 1;
    for (size_t i = 0; i < release; ++i)
    {
        members[i]->groupNo = 0;
        CharacterDatabase.PExecute("UPDATE house_object SET group_no = 0 WHERE id = %u", members[i]->guid);
    }
    FlashObject(player, guid);
    RefreshSelectionMarker(player);
    return true;
}

// .house object clear -- empty the house. One RemoveObject per row rather than a bulk
// DELETE, because each object also owns a live GameObject, a GameObjectData
// entry, a slot in the instance grid set and possibly an edit handle; the bulk
// version would clear the database and leave all four behind until a restart.
// A hundred objects is the ceiling, so a hundred deletes is the worst case.
//
// The copy is not optional: RemoveObject erases from house->objects, so
// iterating it directly invalidates the iterator on the first removal.
bool HouseMgr::ClearHouse(Player* player, uint32& removed, uint32& kept, uint32& noRoom, std::string& error)
{
    House* house = nullptr;
    if (!CheckOwner(*this, player, house, error))
        return false;

    std::vector<uint32> const doomed = house->objects;

    removed = 0;
    kept = 0;
    noRoom = 0;
    for (uint32 guid : doomed)
    {
        // A row that came from an item is HANDED BACK by RemoveObject rather
        // than destroyed, so clearing a house full of bought furniture can run
        // out of bag space part way. That is counted apart from the fabric,
        // because "kept the 3 that came with the house" would be a lie about
        // three chairs still standing that the player has room to carry.
        std::string why;
        if (RemoveObject(player, guid, why))
            ++removed;
        else if (why == "Your bags are full.")
            ++noRoom;
        else
            ++kept;                             // template-sourced, for a player
    }
    return true;
}

//== the instance script =====================================================
//
// map_template.script_name = 'instance_player_house' for map 169. Reached
// through Map::CreateInstanceData, which runs when a copy of the map is
// created and before any grid is loaded -- which is exactly when the furniture
// has to be registered.

struct instance_player_house : public InstanceData
{
    explicit instance_player_house(Map* pMap) : InstanceData(pMap), m_handleTimer(0)
    {
        sHouseMgr.OnHouseMapCreated(pMap);
    }

    void Initialize() override {}
    bool IsEncounterInProgress() const override { return false; }

    // Edit-mode gears follow the player around rather than all standing there at
    // once, which needs somebody to notice they have moved. This is the seam for
    // it: Map::Update already calls us, once per house, and no core patch is
    // needed to get here.
    //
    // The timer belongs here rather than in the manager because it is per-map
    // state and this is the per-map object.
    void Update(uint32 diff) override
    {
        if (m_handleTimer > diff)
        {
            m_handleTimer -= diff;
            return;
        }
        m_handleTimer = HOUSE_EDIT_INTERVAL;
        sHouseMgr.UpdateEditSessions(instance);

        // The way back out. Shares the handle throttle because it is the same
        // kind of job -- make sure the right temporary summons are standing --
        // and because a portal that reappears within a second of being lost is
        // indistinguishable from one that never went.
        sHouseMgr.UpdateExitPortal(instance);

        // And the gear beside it, which carries the house's control panel.
        // Same job again -- keep the right temporary summons standing.
        sHouseMgr.UpdateExitMarker(instance);

        // And the one job here that is not about summons: a visitor whose
        // party has ended goes home. It rides this throttle rather than
        // hooking the group code because a party can end six ways and a poll
        // catches all of them -- see EvictLapsedVisitors in HouseMgr.h.
        sHouseMgr.EvictLapsedVisitors(instance);

        // And the arrival nudge's later beats. See NudgeOccupants for why a
        // sweep rather than one delay.
        sHouseMgr.RunPendingNudges(instance);

        // Animals called over: re-anchor the arrived, put down the stuck.
        sHouseMgr.RunCritterWalks(instance);
    }

    uint32 m_handleTimer;
};

InstanceData* GetInstanceData_instance_player_house(Map* pMap)
{
    return new instance_player_house(pMap);
}

void AddSC_player_house()
{
    Script* pNewScript = new Script;
    pNewScript->Name = "instance_player_house";
    pNewScript->GetInstanceData = &GetInstanceData_instance_player_house;
    pNewScript->RegisterSelf();
}
