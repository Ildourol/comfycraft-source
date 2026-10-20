#include "DungeonRun.h"
#include "DungeonBosses.h"
#include "Map.h"
#include "MapPersistentStateMgr.h"
#include "Player.h"
#include "AccountMgr.h"
#include "Creature.h"
#include "Chat.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Timer.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace
{
    std::atomic<bool> s_tablesPresent{false};

    // Entry -> its rows. Built once, on first use; the data is static.
    std::unordered_multimap<uint32, DungeonBoss const*> const& BossesByEntry()
    {
        static std::unordered_multimap<uint32, DungeonBoss const*> const index = []
        {
            std::unordered_multimap<uint32, DungeonBoss const*> m;
            for (DungeonBoss const& row : DUNGEON_BOSSES)
                m.emplace(row.entry, &row);
            return m;
        }();
        return index;
    }

    bool HasBosses(uint32 mapId)
    {
        for (DungeonBoss const& row : DUNGEON_BOSSES)
            if (row.mapId == mapId)
                return true;
        return false;
    }

    bool IsSplit(uint32 mapId)
    {
        for (DungeonBossWing const& w : DUNGEON_BOSS_WINGS)
            if (w.mapId == mapId)
                return true;
        return false;
    }

    char const* WingName(uint32 mapId, uint8 wing)
    {
        for (DungeonBossWing const& w : DUNGEON_BOSS_WINGS)
            if (w.mapId == mapId && w.wing == wing)
                return w.name;
        return nullptr;
    }

    // A slot is one boss, or one "one of" group: rows that share a oneOf id
    // fill the same slot.
    uint32 SlotId(DungeonBoss const& row)
    {
        return row.oneOf ? 0x80000000u | row.oneOf : row.entry;
    }

    // The required slots of a dungeon or wing.
    uint32 SlotCount(uint32 mapId, uint8 wing)
    {
        std::vector<uint32> seen;
        for (DungeonBoss const& row : DUNGEON_BOSSES)
            if (row.mapId == mapId && row.wing == wing && !row.optional)
                if (std::find(seen.begin(), seen.end(), SlotId(row)) == seen.end())
                    seen.push_back(SlotId(row));
        return seen.size();
    }

    // The slots whose kills make the last-boss time. Empty: no last boss.
    std::vector<uint32> LastSlots(uint32 mapId, uint8 wing)
    {
        std::vector<uint32> slots;
        for (DungeonLastBoss const& last : DUNGEON_LAST_BOSSES)
            if (last.mapId == mapId && last.wing == wing)
                for (DungeonBoss const& row : DUNGEON_BOSSES)
                    if (row.mapId == mapId && row.wing == wing && row.entry == last.entry)
                        if (std::find(slots.begin(), slots.end(), SlotId(row)) == slots.end())
                            slots.push_back(SlotId(row));
        return slots;
    }

    struct Member
    {
        uint32 guid;
        std::string name;
        uint8 cls;
        uint8 level;
        bool bot;
    };

    // Random bots and companions run on RNDBOT and COMPANION accounts, the
    // same test lib/charts.js makes on the website. Recorded at entry, so
    // the flag stays when the character is deleted later.
    bool IsBot(Player* player)
    {
        std::string account;
        if (WorldSession* session = player->GetSession())
            sAccountMgr.GetName(session->GetAccountId(), account);
        for (char& c : account)
            if (c >= 'a' && c <= 'z')
                c = c - 'a' + 'A';
        return account.rfind("RNDBOT", 0) == 0 || account.rfind("COMPANION", 0) == 0;
    }

    struct Kill
    {
        uint32 entry;
        uint32 slot;
        uint32 ms;
    };

    struct Run
    {
        uint32 startMs = 0;        // WorldTimer::getMSTime()
        time_t startTime = 0;
        bool untimed = false;      // the instance had kills before it was seen
        bool wingKnown = false;
        uint8 wing = 0;            // 0 on a map with one part
        uint32 id = 0;             // dungeon_run.id, given at the first save
        bool lastDone = false;     // the last boss is dead
        bool done = false;         // every required boss is dead (the full run)
        uint32 required = 0;       // required slots with a kill
        // The board ranks a time only when no member was above the last
        // boss's level. maxLevel is the highest seen so far, at an entry or
        // a boss kill; lastMaxLevel is maxLevel when the last boss died.
        uint8 maxLevel = 0;
        uint8 lastMaxLevel = 0;
        uint8 bossLevel = 0;       // the last boss's level at the kill
        std::vector<Kill> kills;
        std::vector<Member> members;
    };

    struct Store
    {
        std::mutex lock;
        std::unordered_map<uint64, Run> runs;
    };

    // Never freed: ~DungeonPersistentState calls OnInstanceGone at shutdown,
    // after the static destructors would have run.
    Store& S()
    {
        static Store* store = new Store;
        return *store;
    }

    uint64 Key(uint32 mapId, uint32 instanceId)
    {
        return (uint64(mapId) << 32) | instanceId;
    }

    std::string Clock(uint32 ms)
    {
        uint32 const s = ms / IN_MILLISECONDS;
        char buf[32];
        if (s >= HOUR)
            snprintf(buf, sizeof(buf), "%u:%02u:%02u", s / HOUR, (s % HOUR) / MINUTE, s % MINUTE);
        else
            snprintf(buf, sizeof(buf), "%u:%02u", s / MINUTE, s % MINUTE);
        return buf;
    }

    std::string DungeonName(Map const* map, uint8 wing)
    {
        std::string name = map->GetMapName();
        if (char const* w = WingName(map->GetId(), wing))
            name = name + " (" + w + ")";
        return name;
    }

    void Broadcast(Map* map, std::string const& text)
    {
        for (auto const& ref : map->GetPlayers())
            if (Player* player = ref.getSource())
                ChatHandler(player).SendSysMessage(text.c_str());
    }

    std::atomic<uint32> s_lastRunId{0};

    std::string MsOrNull(uint32 ms)
    {
        return ms ? std::to_string(ms) : "NULL";
    }

    // Saved at the last boss and again at the full run. PExecute is not
    // ordered between transactions, so each save writes everything it knows
    // and neither can undo the other: COALESCE keeps a time once written,
    // and the boss and member rows are INSERT IGNORE on their keys.
    void Save(uint32 mapId, Run& run, uint32 lastMs, uint32 fullMs)
    {
        if (!s_tablesPresent.load(std::memory_order_relaxed))
            return;
        if (!run.id)
            run.id = ++s_lastRunId;

        CharacterDatabase.BeginTransaction();
        CharacterDatabase.PExecute(
            "INSERT INTO dungeon_run (id, map, wing, started_at, last_boss_ms, full_ms, group_size, "
            "boss_level, last_max_level, max_level) "
            "VALUES ('%u', '%u', '%u', FROM_UNIXTIME('%u'), %s, %s, '%u', '%u', '%u', '%u') "
            "ON DUPLICATE KEY UPDATE last_boss_ms = COALESCE(last_boss_ms, VALUES(last_boss_ms)), "
            "full_ms = COALESCE(full_ms, VALUES(full_ms)), group_size = GREATEST(group_size, VALUES(group_size)), "
            "max_level = GREATEST(max_level, VALUES(max_level))",
            run.id, mapId, uint32(run.wing), uint32(run.startTime), MsOrNull(lastMs).c_str(), MsOrNull(fullMs).c_str(),
            uint32(run.members.size()), uint32(run.bossLevel), uint32(run.lastMaxLevel), uint32(run.maxLevel));

        std::ostringstream bosses;
        bosses << "INSERT IGNORE INTO dungeon_run_boss (run_id, entry, elapsed_ms) VALUES ";
        for (size_t i = 0; i < run.kills.size(); ++i)
            bosses << (i ? ", " : "") << "(" << run.id << ", " << run.kills[i].entry << ", " << run.kills[i].ms << ")";
        CharacterDatabase.Execute(bosses.str().c_str());

        std::ostringstream members;
        members << "INSERT IGNORE INTO dungeon_run_member (run_id, slot, guid, name, class, level, is_bot) VALUES ";
        for (size_t i = 0; i < run.members.size(); ++i)
        {
            std::string name = run.members[i].name;
            CharacterDatabase.escape_string(name);
            members << (i ? ", " : "") << "(" << run.id << ", " << i << ", " << run.members[i].guid << ", '" << name << "', "
                    << uint32(run.members[i].cls) << ", " << uint32(run.members[i].level) << ", " << (run.members[i].bot ? 1 : 0) << ")";
        }
        CharacterDatabase.Execute(members.str().c_str());
        CharacterDatabase.CommitTransaction();
    }
}

void DungeonRun::Load()
{
    // A write to a missing table or column is ASSERT(false) in
    // HandleMySQLError, so the columns the saves name are probed once and
    // nothing is written without them.
    bool present = false;
    if (QueryResult* probe = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() AND ("
            "(table_name = 'dungeon_run' AND column_name IN "
            "('last_boss_ms', 'full_ms', 'boss_level', 'last_max_level', 'max_level')) OR "
            "(table_name = 'dungeon_run_boss' AND column_name = 'elapsed_ms') OR "
            "(table_name = 'dungeon_run_member' AND column_name IN ('slot', 'is_bot')))"))
    {
        present = probe->Fetch()[0].GetUInt32() == 8;
        delete probe;
    }
    if (present)
        if (QueryResult* result = CharacterDatabase.Query("SELECT COALESCE(MAX(id), 0) FROM dungeon_run"))
        {
            s_lastRunId.store(result->Fetch()[0].GetUInt32());
            delete result;
        }
    s_tablesPresent.store(present, std::memory_order_relaxed);
    if (!present)
        sLog.outString(">> Dungeon runs: not saved (apply sql/custom/161_dungeon_runs.sql to tw_char).");
}

bool DungeonRun::TablesPresent()
{
    return s_tablesPresent.load(std::memory_order_relaxed);
}

void DungeonRun::OnPlayerEnter(DungeonMap* map, Player* player)
{
    uint32 const mapId = map->GetId();
    if (!HasBosses(mapId))
        return;

    std::string line;
    {
        std::lock_guard<std::mutex> guard(S().lock);
        auto it = S().runs.find(Key(mapId, map->GetInstanceId()));
        if (it == S().runs.end())
        {
            Run run;
            run.startMs = WorldTimer::getMSTime();
            run.startTime = time(nullptr);
            run.untimed = map->GetPersistanceState() && map->GetPersistanceState()->HasCreatureRespawnTimes();
            run.wingKnown = !IsSplit(mapId);
            it = S().runs.emplace(Key(mapId, map->GetInstanceId()), std::move(run)).first;

            if (it->second.untimed)
                line = "No run timer: this instance has kills from before a restart.";
            else if (it->second.wingKnown)
                line = "Run timer started: " + DungeonName(map, 0) + ", " + std::to_string(SlotCount(mapId, 0)) + " bosses.";
            else
                line = "Run timer started: " + DungeonName(map, 0) + ".";
        }
        else if (!it->second.untimed && !it->second.done)
        {
            Run const& run = it->second;
            line = "Run timer: " + DungeonName(map, run.wing) + ", " + Clock(WorldTimer::getMSTimeDiffToNow(run.startMs));
            if (run.wingKnown)
                line += ", " + std::to_string(run.required) + "/" + std::to_string(SlotCount(mapId, run.wing)) + " bosses";
            line += ".";
        }

        Run& run = it->second;
        if (!run.untimed && !run.done)
        {
            bool known = false;
            for (Member const& m : run.members)
                known = known || m.guid == player->GetGUIDLow();
            if (!known)
                run.members.push_back({ player->GetGUIDLow(), player->GetName(), player->GetClass(), uint8(player->GetLevel()), IsBot(player) });
            run.maxLevel = std::max<uint8>(run.maxLevel, player->GetLevel());
        }
    }

    if (!line.empty())
        ChatHandler(player).SendSysMessage(line.c_str());
}

void DungeonRun::OnCreatureKilled(Creature* creature)
{
    Map* map = creature->GetMap();
    if (!map || !map->IsDungeon())
        return;

    DungeonBoss const* row = nullptr;
    auto range = BossesByEntry().equal_range(creature->GetEntry());
    for (auto it = range.first; it != range.second && !row; ++it)
        if (it->second->mapId == map->GetId())
            row = it->second;
    if (!row)
        return;

    std::string killLine, clearLine;
    {
        std::lock_guard<std::mutex> guard(S().lock);
        auto it = S().runs.find(Key(map->GetId(), map->GetInstanceId()));
        if (it == S().runs.end())
            return;
        Run& run = it->second;
        if (run.untimed || run.done)
            return;

        if (!run.wingKnown)
        {
            run.wing = row->wing;
            run.wingKnown = true;
        }
        if (row->wing != run.wing)
            return;

        uint32 const slot = SlotId(*row);
        for (Kill const& k : run.kills)
            if (k.slot == slot)
                return;

        uint32 const ms = WorldTimer::getMSTimeDiffToNow(run.startMs);
        run.kills.push_back({ creature->GetEntry(), slot, ms });

        // A level gained since entry counts, so the group is read again at
        // every boss kill. The player list is this map thread's own.
        for (auto const& ref : map->GetPlayers())
            if (Player* player = ref.getSource())
                run.maxLevel = std::max<uint8>(run.maxLevel, player->GetLevel());

        std::vector<uint32> const lastSlots = LastSlots(map->GetId(), run.wing);
        if (std::find(lastSlots.begin(), lastSlots.end(), slot) != lastSlots.end())
            run.bossLevel = std::max<uint8>(run.bossLevel, creature->GetLevel());

        uint32 const total = SlotCount(map->GetId(), run.wing);
        if (row->optional)
            killLine = std::string(creature->GetName()) + ", " + Clock(ms) + ".";
        else
            killLine = std::string(creature->GetName()) + " " + std::to_string(++run.required) + "/" + std::to_string(total) + ", " + Clock(ms) + ".";

        bool lastNow = false;
        if (!run.lastDone)
        {
            lastNow = !lastSlots.empty();
            for (uint32 s : lastSlots)
                lastNow = lastNow && std::any_of(run.kills.begin(), run.kills.end(), [s](Kill const& k) { return k.slot == s; });
            run.lastDone = lastNow;
            if (lastNow)
                run.lastMaxLevel = run.maxLevel;
        }
        bool const fullNow = !row->optional && run.required >= total;
        run.done = fullNow;

        if (lastNow || fullNow)
        {
            std::string const name = DungeonName(map, run.wing);
            std::string const group = ", group of " + std::to_string(run.members.size()) + ".";
            char const* what = lastNow && fullNow ? "last boss and full run" : lastNow ? "last boss" : "full run";
            clearLine = name + ": " + what + " in " + Clock(ms) + group;
            if ((lastNow ? run.lastMaxLevel : run.maxLevel) > run.bossLevel)
                clearLine += " Not ranked: a member was above level " + std::to_string(run.bossLevel) + ".";
            Save(map->GetId(), run, lastNow ? ms : 0, fullNow ? ms : 0);
            sLog.outString("Dungeon run: %s: %s in %s, group of %u (run %u, instance %u).",
                name.c_str(), what, Clock(ms).c_str(), uint32(run.members.size()), run.id, map->GetInstanceId());
        }
    }

    Broadcast(map, killLine);
    if (!clearLine.empty())
        Broadcast(map, clearLine);
}

void DungeonRun::OnInstanceGone(uint32 mapId, uint32 instanceId)
{
    std::lock_guard<std::mutex> guard(S().lock);
    S().runs.erase(Key(mapId, instanceId));
}
