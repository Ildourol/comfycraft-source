#include "ArrivalFloor.h"
#include "Map.h"
#include "Player.h"
#include "Creature.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "MotionMaster.h"
#include "MovementPacketSender.h"
#include "MoveSpline.h"
#include "ScriptMgr.h"
#include "BotNpc.h"
#include "Chat.h"
#include "ComfyCommands.h"
#include "GameObject.h"
#include "vmap/GameObjectModel.h"
#include "GridMap.h"

#include <atomic>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    uint32 const ARRIVAL_WATCH_MS    = 15000;   // how long a player is checked
    uint32 const ARRIVAL_HOLD_MS     = 1000;    // how long the floor z is held
    uint32 const ARRIVAL_HOLD_EVERY  = 250;     // between hold packets
    float  const ARRIVAL_HOLD_MOVED  = 0.5f;    // moved this far from arrival: the hold ends
    uint32 const ARRIVAL_CHECK_MS    = 500;     // between checks
    uint32 const ARRIVAL_MAX_LIFTS   = 6;       // a client without the model falls forever
    float  const ARRIVAL_SUNK        = 0.3f;    // below the floor by more than this is a fall

    float  const BEAT_RADIUS         = 60.0f;   // creatures sent again around each player
    float  const BEAT_LIFT           = 0.05f;   // the hop in lift mode: a real delta, invisible
    uint32 const BEAT_EVERY_DEFAULT  = 5000;

    std::atomic<uint8>  g_beatMode{uint8(ArrivalFloor::BeatMode::Plain)};
    std::atomic<uint32> g_beatEvery{BEAT_EVERY_DEFAULT};
    std::atomic<uint32> g_lastBeatCreatures{0};
    std::atomic<uint32> g_lastBeatBotNpcs{0};
    std::atomic<uint32> g_lastBeatPlayers{0};
    std::atomic<uint32> g_lastBeatMap{0};

    // Time since the last beat, per map instance. Maps update on several threads.
    std::mutex                          g_beatLock;
    std::unordered_map<uint64, uint32>  g_beatTimers;

    uint32 const HOUSING_MAP = 28;              // HouseMgr handles arrivals there

    struct Arrival
    {
        uint32 mapId      = 0;
        uint32 instanceId = 0;
        float  x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 elapsed    = 0;
        uint32 sinceCheck = 0;
        uint32 lifts      = 0;
        bool   lifting    = false;  // our own NearTeleportTo: its ack must not re-arm
        bool   moved      = false;  // walked, strafed or left the arrival point
        bool   hold       = false;  // arrived on a gameobject floor: hold z for a while
        uint32 sinceHold  = 0;
    };

    enum class Step { Check, Hold };

    struct Work
    {
        uint32  guid;
        Step    step;
        Arrival a;
    };

    std::mutex                          g_lock;
    std::unordered_map<uint32, Arrival> g_arrivals;
    std::atomic<uint32>                 g_count{0};

    bool HasHeight(float h) { return h > INVALID_HEIGHT; }

    // A gameobject floor at z, with no terrain or static WMO at that height.
    bool OnGameObjectFloor(Map const* map, float x, float y, float z)
    {
        float const floor = map->GetDynamicTreeHeight(x, y, z + 0.5f, 1.0f);
        if (!HasHeight(floor) || std::fabs(floor - z) > 0.5f)
            return false;

        float const ground = map->GetTerrain()->GetHeightStatic(x, y, z, true, 2.0f);
        return !(HasHeight(ground) && ground > z - 0.5f);
    }

    // The hold: a heartbeat to the player's own client with its current x, y
    // and facing, and the arrival floor's z. A client obeys a server position
    // for its own mover, so it does not fall while the building loads. The x
    // and y are the client's own, so a player who has moved is not pulled back.
    // False when the player has started to walk or jump: the hold ends there.
    //
    // THE PACKET IS BUILT FROM A COPY. The first version wrote the floor z into
    // m_movementInfo. Each client packet is read on top of that copy and checked
    // by the anticheat against it, so the client's packets could be refused:
    // the server then never saw the player move, held for the full 3 s and sent
    // an old x and y. That was the jank.
    bool HoldPlayer(Player* p, Arrival const& a)
    {
        if (p->IsTaxiFlying() || p->GetTransport() || p->IsBeingTeleported())
            return false;

        if (p->m_movementInfo.HasMovementFlag(MOVEFLAG_MASK_XZ | MOVEFLAG_JUMPING))
            return false;

        if (!p->IsWithinDist2d(a.x, a.y, ARRIVAL_HOLD_MOVED))
            return false;

        MovementInfo mi = p->m_movementInfo;
        mi.ChangePosition(p->GetPositionX(), p->GetPositionY(), a.z, p->GetOrientation());
        mi.RemoveMovementFlag(MOVEFLAG_MASK_MOVING);
        mi.UpdateTime(WorldTimer::getMSTime());

        WorldPacket data(MSG_MOVE_HEARTBEAT);
        data << p->GetPackGUID();
        data << mi;
        p->GetSession()->SendPacket(&data);
        return true;
    }

    bool IsAirborne(Player* p)
    {
        return p->m_movementInfo.HasMovementFlag(MOVEFLAG_JUMPING | MOVEFLAG_FALLINGFAR);
    }

    // Walked or strafed, or more than ARRIVAL_HOLD_MOVED from the arrival
    // point. A fall from gravity sets no walk flag and keeps x and y.
    bool HasMoved(Player* p, Arrival const& a)
    {
        return p->m_movementInfo.HasMovementFlag(MOVEFLAG_MASK_XZ)
            || !p->IsWithinDist2d(a.x, a.y, ARRIVAL_HOLD_MOVED);
    }

    // On the ground, with the client's z within ARRIVAL_SUNK of the server's
    // floor under its feet (terrain, a building or a gameobject). A player sunk
    // into a floor is under it, so the search from just above the feet does
    // not find it.
    //
    // Under a gameobject floor at arrival height, only a gameobject surface
    // counts: a lower storey of the house is one, and the terrain under the
    // house is where a player lands who walked before the house loaded.
    bool OnServerFloor(Map* map, Player* p, Arrival const& a)
    {
        if (IsAirborne(p))
            return false;

        float const x = p->GetPositionX();
        float const y = p->GetPositionY();
        float const z = p->GetPositionZ();

        float const floor = map->GetHeight(x, y, z + 0.5f, true, 1.0f);
        if (!HasHeight(floor) || std::fabs(floor - z) > ARRIVAL_SUNK)
            return false;

        if (z > a.z - ARRIVAL_SUNK)
            return true;

        float const top   = a.z + 0.5f;
        float const above = map->GetDynamicTreeHeight(x, y, top, top - z);
        if (!HasHeight(above) || above < z + ARRIVAL_SUNK)
            return true;

        float const feet = map->GetDynamicTreeHeight(x, y, z + 0.5f, 1.0f);
        return HasHeight(feet) && std::fabs(feet - z) <= ARRIVAL_SUNK;
    }

    // Fallen through a gameobject floor: put back on it, with the client's own x,
    // y and facing. True when a teleport went out.
    bool CheckPlayer(Map* map, Player* p, Arrival const& a)
    {
        if (p->IsTaxiFlying() || p->GetTransport() || p->IsBeingTeleported())
            return false;

        // In the air after a move of their own: a jump or a walk off an edge.
        // Wait for the landing; a verified step there ends the watch. Without
        // this, a jump down a storey after a login was pulled back up while
        // under the upper floor (2026-11-04). A fall in place is still
        // corrected in the air.
        if (a.moved && IsAirborne(p))
            return false;

        float const x = p->GetPositionX();
        float const y = p->GetPositionY();
        float const z = p->GetPositionZ();

        if (z > a.z - ARRIVAL_SUNK)
            return false;                       // not below where they came in

        // The gameobject floor under this x and y, searched from arrival height
        // down to the player.
        float const top   = a.z + 0.5f;
        float const floor = map->GetDynamicTreeHeight(x, y, top, top - z);
        if (!HasHeight(floor) || floor < z + ARRIVAL_SUNK)
            return false;                       // no gameobject floor above them

        // A fall from upstairs lands on the downstairs floor, which is a
        // gameobject too, so any gameobject floor above them counts.
        sLog.outString("ArrivalFloor: %s was %.2f yd under a gameobject floor at (%.1f, %.1f, %.2f) on map %u, put back",
                       p->GetName(), floor - z, x, y, floor, map->GetId());

        p->NearTeleportTo(x, y, floor, p->GetOrientation());
        return true;
    }

    // A creature the server holds still: alive, not in combat, not a pet, not on
    // a transport, on its idle generator, with no spline running.
    bool IsStill(Creature* c)
    {
        if (!c->IsInWorld() || !c->IsAlive() || c->IsPet() || c->IsInCombat() || c->GetTransport())
            return false;

        if (c->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
            return false;

        return c->movespline->Finalized();
    }

    // A placed bot NPC (a Player) the server holds still: alive, not in combat,
    // not moving, not on a transport or a taxi, not being teleported.
    bool IsStill(Player* b)
    {
        if (!b->IsInWorld() || !b->IsAlive() || b->IsInCombat() || b->GetTransport() || b->IsTaxiFlying())
            return false;

        if (b->IsBeingTeleported() || b->m_movementInfo.HasMovementFlag(MOVEFLAG_MASK_MOVING | MOVEFLAG_JUMPING))
            return false;

        return b->movespline->Finalized();
    }

    bool IsRealPlayer(Player* p)
    {
        if (!p || !p->IsInWorld() || !p->GetSession() || Script_IsAIControlled(p))
            return false;

        std::string const& address = p->GetSession()->GetRemoteAddress();
        return !address.empty() && address != "disconnected/bot";
    }

    void SendBeat(Unit* u, ArrivalFloor::BeatMode mode)
    {
        float const x = u->GetPositionX();
        float const y = u->GetPositionY();
        float const z = u->GetPositionZ();
        float const o = u->GetOrientation();

        if (mode == ArrivalFloor::BeatMode::Lift)
            MovementPacketSender::SendTeleportToObservers(u, x, y, z + BEAT_LIFT, o);
        MovementPacketSender::SendTeleportToObservers(u, x, y, z, o);
    }

    // Each still creature and each still bot NPC within BEAT_RADIUS of a real
    // player, sent its own position again. One near two players is sent once.
    void BeatCreatures(Map* map, ArrivalFloor::BeatMode mode)
    {
        std::vector<Player*> real;
        std::vector<Player*> botNpcs;

        Map::PlayerList const& list = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = list.begin(); itr != list.end(); ++itr)
        {
            Player* p = itr->getSource();
            if (IsRealPlayer(p))
                real.push_back(p);
            else if (p && p->IsInWorld() && sBotNpcMgr.IsBotNpc(p->GetGUIDLow()))
                botNpcs.push_back(p);
        }

        if (real.empty())
            return;

        std::unordered_set<Creature*> seen;
        std::vector<Creature*>        send;

        for (Player* p : real)
        {
            std::list<Creature*> creatures;
            MaNGOS::AllCreaturesInRange check(p, BEAT_RADIUS);
            MaNGOS::CreatureListSearcher<MaNGOS::AllCreaturesInRange> searcher(creatures, check);
            Cell::VisitGridObjects(p, searcher, BEAT_RADIUS);

            for (Creature* c : creatures)
                if (seen.insert(c).second && IsStill(c))
                    send.push_back(c);
        }

        for (Creature* c : send)
            SendBeat(c, mode);

        uint32 bots = 0;
        for (Player* b : botNpcs)
        {
            bool inRange = false;
            for (Player* p : real)
                if (b->IsWithinDistInMap(p, BEAT_RADIUS))
                {
                    inRange = true;
                    break;
                }

            if (inRange && IsStill(b))
            {
                SendBeat(b, mode);
                ++bots;
            }
        }

        g_lastBeatCreatures = uint32(send.size());
        g_lastBeatBotNpcs   = bots;
        g_lastBeatPlayers   = uint32(real.size());
        g_lastBeatMap       = map->GetId();
    }

    void UpdateBeat(Map* map, uint32 diff)
    {
        ArrivalFloor::BeatMode const mode = ArrivalFloor::BeatMode(g_beatMode.load(std::memory_order_relaxed));
        if (mode == ArrivalFloor::BeatMode::Off || map->GetPlayers().isEmpty())
            return;

        uint64 const key   = (uint64(map->GetId()) << 32) | map->GetInstanceId();
        uint32 const every = g_beatEvery.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> guard(g_beatLock);
            uint32& t = g_beatTimers[key];
            t += diff;
            if (t < every)
                return;
            t = 0;
        }

        BeatCreatures(map, mode);
    }
}

void ArrivalFloor::SetBeat(BeatMode mode, uint32 everyMs)
{
    g_beatMode  = uint8(mode);
    g_beatEvery = everyMs;
}

ArrivalFloor::BeatMode ArrivalFloor::GetBeatMode()
{
    return BeatMode(g_beatMode.load());
}

uint32 ArrivalFloor::GetBeatEveryMs()
{
    return g_beatEvery.load();
}

void ArrivalFloor::GetLastBeat(uint32& creatures, uint32& botNpcs, uint32& players, uint32& mapId)
{
    creatures = g_lastBeatCreatures.load();
    botNpcs   = g_lastBeatBotNpcs.load();
    players   = g_lastBeatPlayers.load();
    mapId     = g_lastBeatMap.load();
}

void ArrivalFloor::Arm(Player* player)
{
    if (!player || !player->IsInWorld())
        return;

    Map* map = player->GetMap();
    if (!map || map->GetId() == HOUSING_MAP)
        return;

    bool const hold = OnGameObjectFloor(map, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());

    Arrival a;
    {
        std::lock_guard<std::mutex> guard(g_lock);

        auto itr = g_arrivals.find(player->GetGUIDLow());
        if (itr != g_arrivals.end() && itr->second.lifting)
        {
            itr->second.lifting = false;            // the ack of our own lift
            return;
        }

        a.mapId      = map->GetId();
        a.instanceId = map->GetInstanceId();
        a.x          = player->GetPositionX();
        a.y          = player->GetPositionY();
        a.z          = player->GetPositionZ();
        a.hold       = hold;

        if (hold)
            sLog.outString("ArrivalFloor: holding %s on a gameobject floor at z %.2f on map %u",
                           player->GetName(), a.z, map->GetId());

        g_arrivals[player->GetGUIDLow()] = a;
        g_count = uint32(g_arrivals.size());
    }

    // The first packet now, not on the next tick.
    if (hold)
        HoldPlayer(player, a);
}

void ArrivalFloor::Update(Map* map, uint32 diff)
{
    UpdateBeat(map, diff);

    if (!g_count.load(std::memory_order_relaxed))
        return;

    // Collect under the lock, act outside it: a lift's ack comes back to Arm.
    std::vector<Work> work;
    {
        std::lock_guard<std::mutex> guard(g_lock);

        for (auto itr = g_arrivals.begin(); itr != g_arrivals.end();)
        {
            Arrival& a = itr->second;
            if (a.mapId != map->GetId() || a.instanceId != map->GetInstanceId())
            {
                ++itr;
                continue;
            }

            Player* p = map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, itr->first));
            if (!p)
            {
                itr = g_arrivals.erase(itr);    // logged out or left the map
                continue;
            }

            if (a.lifting && !p->IsBeingTeleportedNear())
                a.lifting = false;              // the lift ended without an ack

            a.elapsed    += diff;
            a.sinceCheck += diff;
            a.sinceHold  += diff;

            // A verified step ends the watch: the player has moved on their
            // own, is on the ground, and the client's z is on a floor the
            // server has under them. The client has the floor loaded, so there
            // is nothing left to catch.
            if (!a.lifting && HasMoved(p, a))
            {
                a.moved = true;
                if (OnServerFloor(map, p, a))
                {
                    itr = g_arrivals.erase(itr);
                    continue;
                }
            }

            if (a.hold && a.elapsed >= ARRIVAL_HOLD_MS)
                a.hold = false;

            if (a.hold && a.sinceHold >= ARRIVAL_HOLD_EVERY)
            {
                a.sinceHold = 0;
                work.push_back({ itr->first, Step::Hold, a });
            }

            if (!a.hold && a.sinceCheck >= ARRIVAL_CHECK_MS && a.lifts < ARRIVAL_MAX_LIFTS)
            {
                a.sinceCheck = 0;
                work.push_back({ itr->first, Step::Check, a });
            }

            if (a.elapsed >= ARRIVAL_WATCH_MS)
                itr = g_arrivals.erase(itr);
            else
                ++itr;
        }

        g_count = uint32(g_arrivals.size());
    }

    for (Work const& w : work)
    {
        Player* p = map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, w.guid));
        if (!p || !p->IsInWorld())
            continue;

        if (w.step == Step::Hold)
        {
            if (!HoldPlayer(p, w.a))
            {
                std::lock_guard<std::mutex> guard(g_lock);
                auto itr = g_arrivals.find(w.guid);
                if (itr != g_arrivals.end())
                    itr->second.hold = false;
            }
            continue;
        }

        // Mark the lift before sending it: the ack can arrive on this thread
        // before Update runs again.
        {
            std::lock_guard<std::mutex> guard(g_lock);
            auto itr = g_arrivals.find(w.guid);
            if (itr == g_arrivals.end())
                continue;
            itr->second.lifting = true;
        }

        bool const lifted = CheckPlayer(map, p, w.a);

        std::lock_guard<std::mutex> guard(g_lock);
        auto itr = g_arrivals.find(w.guid);
        if (itr == g_arrivals.end())
            continue;
        if (lifted)
            ++itr->second.lifts;
        else
            itr->second.lifting = false;
    }
}

// .floorbeat [off | plain | lift] [seconds]. In memory only: a restart puts back
// plain every 5 s. With no argument it shows the mode and the last beat.
bool ComfyCommands::HandleFloorBeatCommand(ChatHandler* handler, char* args)
{
    using ArrivalFloor::BeatMode;

    BeatMode mode  = ArrivalFloor::GetBeatMode();
    uint32   every = ArrivalFloor::GetBeatEveryMs();

    if (handler->ExtractLiteralArg(&args, "off"))
        mode = BeatMode::Off;
    else if (handler->ExtractLiteralArg(&args, "plain"))
        mode = BeatMode::Plain;
    else if (handler->ExtractLiteralArg(&args, "lift"))
        mode = BeatMode::Lift;

    if (*args)
    {
        uint32 seconds;
        if (!handler->ExtractUInt32(&args, seconds) || seconds < 1 || seconds > 600)
        {
            handler->SendSysMessage(".floorbeat [off | plain | lift] [seconds, 1 to 600]");
            handler->SetSentErrorMessage(true);
            return false;
        }
        every = seconds * IN_MILLISECONDS;
    }

    ArrivalFloor::SetBeat(mode, every);

    char const* name = mode == BeatMode::Off ? "off" : mode == BeatMode::Plain ? "plain" : "lift";
    uint32 creatures, botNpcs, players, mapId;
    ArrivalFloor::GetLastBeat(creatures, botNpcs, players, mapId);

    if (mode == BeatMode::Off)
        handler->SendSysMessage("Floor beat: off.");
    else
        handler->PSendSysMessage("Floor beat: %s, every %u s. Last beat: %u creature(s) and %u bot NPC(s) near %u player(s) on map %u.",
                                 name, every / IN_MILLISECONDS, creatures, botNpcs, players, mapId);
    return true;
}

// .floorprobe [above]: the server's floor under the selected unit, or under
// you. The ray starts `above` yd over the unit's feet (default 0.5) and looks
// 20 yd down. It prints the terrain and static building height, the gameobject
// height the map returns, the height the core uses (the higher of the two),
// and every gameobject the ray hits, each tested on its own. Built to check
// the gameobject height search (vmap/DynamicTree.cpp), 2026-11-04.
bool ComfyCommands::HandleFloorProbeCommand(ChatHandler* handler, char* args)
{
    Player* self = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
    if (!self)
    {
        handler->SendSysMessage("Use .floorprobe in game.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    Unit* unit = handler->GetSelectedUnit();
    if (!unit || !unit->IsInWorld() || unit->GetMap() != self->GetMap())
        unit = self;

    float above = 0.5f;
    if (*args && (!handler->ExtractFloat(&args, above) || above < 0.0f || above > 50.0f))
    {
        handler->SendSysMessage(".floorprobe [yards above the feet, 0 to 50]");
        handler->SetSentErrorMessage(true);
        return false;
    }

    float const searchDist = 20.0f;
    Map* map = unit->GetMap();
    float const x = unit->GetPositionX();
    float const y = unit->GetPositionY();
    float const feet = unit->GetPositionZ();
    float const from = feet + above;

    float const statics = map->GetTerrain()->GetHeightStatic(x, y, from, true, searchDist);
    float const dynamic = map->GetDynamicTreeHeight(x, y, from, searchDist);
    float const used    = map->GetHeight(x, y, from, true, searchDist);

    auto show = [](float h, char* buf, size_t size)
    {
        if (h > INVALID_HEIGHT)
            snprintf(buf, size, "%.2f", h);
        else
            snprintf(buf, size, "none");
    };
    char s[32], d[32], u[32];
    show(statics, s, sizeof(s));
    show(dynamic, d, sizeof(d));
    show(used, u, sizeof(u));

    // Each line goes to the chat and to the server log, so the result can be
    // read without a screenshot.
    auto say = [handler](char const* fmt, auto... a)
    {
        char line[512];
        snprintf(line, sizeof(line), fmt, a...);
        handler->SendSysMessage(line);
        sLog.outString("FloorProbe: %s", line);
    };

    say("Floor probe: %s at (%.2f, %.2f) z %.2f, map %u, from %.2f down %.0f yd.", unit->GetName(), x, y, feet, map->GetId(), from, searchDist);
    say("Terrain and buildings: %s. Gameobjects: %s. Used: %s.", s, d, u);

    std::list<GameObject*> objects;
    MaNGOS::AllGameObjectsInRange check(unit, BEAT_RADIUS);
    MaNGOS::GameObjectListSearcher<MaNGOS::AllGameObjectsInRange> searcher(objects, check);
    Cell::VisitGridObjects(unit, searcher, BEAT_RADIUS);

    G3D::Ray const ray(G3D::Vector3(x, y, from), G3D::Vector3(0.0f, 0.0f, -1.0f));
    uint32 hits = 0;
    for (GameObject* go : objects)
    {
        GameObjectModel* model = GameObjectModel::construct(go);
        if (!model)
            continue;

        float dist = searchDist;
        if (model->intersectRay(ray, dist, false))
        {
            ++hits;
            say("- %.2f: %s, guid %u, entry %u, %s, size %.2f",
                                     from - dist, go->GetGOInfo()->name.c_str(), go->GetGUIDLow(), go->GetEntry(),
                                     model->name.c_str(), go->GetObjectScale());
        }
        delete model;
    }

    if (!hits)
        say("%s", "- No gameobject under this point.");
    return true;
}
