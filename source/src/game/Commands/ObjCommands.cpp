/*
 * .obj -- the objects made in the Workshop, by short id.
 *
 * /admin/objects writes gameobject_template entries 100500 to 100999 and
 * nothing else (website/lib/gameobjects.js). That block is what makes a short
 * id free: #1 is 100500, #7 is 100506, #500 is 100999. Nothing is stored. A
 * placed copy is numbered by guid among the copies of its object, so #7.2 is
 * the second copy of #7. Removing a copy renumbers the ones after it.
 *
 *   .obj list                       every object of ours, with how many are placed
 *   .obj info [object]              what it is, what it gives, and every copy
 *   .obj near [yards]               ours around you, nearest first; selects the nearest
 *   .obj where <object>             every copy of one object
 *   .obj go <object or copy>        teleport to a copy
 *   .obj place <object>             one here, facing your way, saved (also add)
 *   .obj select [object or copy]    pick a copy for move, turn and remove
 *   .obj move [object or copy]      the copy to where you stand
 *   .obj turn [object or copy]      the copy to face your way
 *   .obj remove [object or copy]    the copy out of the world and the table (also del, unplace)
 *   .obj back <object> <seconds>    how long every copy stays gone after use
 *
 * An <object> is `7`, `#7`, the full entry, or a name. A copy is `7.2`.
 *
 * WITH NO COPY GIVEN, move, turn and remove act on the selected object when it
 * is one of ours (`.obj near`, `.obj select` or `.gobject select` set it), then
 * on the nearest of ours within 20 yards, and otherwise refuse. The 1.12 client
 * cannot target a gameobject, so the selection is the server's.
 *
 * THE RESPAWN TIME IS PER COPY in this core (`gameobject.spawntimesecsmin`) and
 * only matters when the object disappears when used. `place` therefore takes no
 * number: a copy that stays is placed with 0 (GameObject::Update returns early
 * on a zero delay, which is what keeps a used goober standing for everybody),
 * and one that disappears copies the time from its siblings, or 5 minutes for
 * the first. The website's "Comes back after" writes every copy's row and then
 * sends `obj back` so the running server agrees (lib/gamecommand.js).
 *
 * Underneath it is `.gobject`: the same create, relocate and delete steps as
 * HandleGameObjectAddCommand, ...MoveCommand, ...TurnCommand and
 * ...DeleteCommand in Commands.cpp, with one short line in place of their guid
 * dumps.
 */

#include "Chat.h"
#include "ComfyCommands.h"
#include "CellImpl.h"
#include "Database/DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Language.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>
#include <string>
#include <vector>

namespace
{
    uint32 const OBJ_FIRST = 100500;
    uint32 const OBJ_LAST = 100999;
    float const OBJ_REACH = 20.0f;      // "nearest" for select, move, turn, remove
    int32 const OBJ_DEFAULT_BACK = 300; // the first copy of an object that disappears
    uint32 const OBJ_NEAR_LINES = 15;

    bool IsOurs(uint32 entry) { return entry >= OBJ_FIRST && entry <= OBJ_LAST; }
    uint32 ShortId(uint32 entry) { return entry - OBJ_FIRST + 1; }

    std::string Lower(std::string s)
    {
        for (char& c : s)
            c = char(std::tolower((unsigned char)c));
        return s;
    }

    std::string Trim(std::string const& s)
    {
        size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos)
            return "";
        size_t b = s.find_last_not_of(" \t");
        return s.substr(a, b - a + 1);
    }

    bool Consumable(GameObjectInfo const* info)
    {
        if (info->type == GAMEOBJECT_TYPE_CHEST)
            return info->chest.consumable != 0;
        if (info->type == GAMEOBJECT_TYPE_GOOBER)
            return info->goober.consumable != 0;
        return false;
    }

    std::string Duration(int32 secs)
    {
        char buf[32];
        if (secs % 60 == 0)
            snprintf(buf, sizeof(buf), "%d min", secs / 60);
        else
            snprintf(buf, sizeof(buf), "%d s", secs);
        return buf;
    }

    struct Copy
    {
        uint32 guid;
        GameObjectData const* data;
    };

    // Every placed copy of one object, in guid order: the order copy numbers count in.
    std::vector<Copy> CopiesOf(uint32 entry)
    {
        std::vector<Copy> out;
        auto worker = [&](GameObjectDataPair const& p)
        {
            if (p.second.id == entry)
                out.push_back({ p.first, &p.second });
            return false;
        };
        sObjectMgr.DoGOData(worker);
        std::sort(out.begin(), out.end(), [](Copy const& a, Copy const& b) { return a.guid < b.guid; });
        return out;
    }

    uint32 CopyNumber(uint32 entry, uint32 guid)
    {
        std::vector<Copy> copies = CopiesOf(entry);
        for (size_t i = 0; i < copies.size(); ++i)
            if (copies[i].guid == guid)
                return uint32(i + 1);
        return 0;
    }

    std::string Label(uint32 entry, uint32 copy = 0)
    {
        GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(entry);
        char buf[48];
        if (copy)
            snprintf(buf, sizeof(buf), "#%u.%u", ShortId(entry), copy);
        else
            snprintf(buf, sizeof(buf), "#%u", ShortId(entry));
        return std::string(buf) + " " + (info ? info->name : std::string("?"));
    }

    std::string Label(GameObject* go)
    {
        return Label(go->GetEntry(), CopyNumber(go->GetEntry(), go->GetDBTableGUIDLow()));
    }

    // What the player typed: an object, and maybe which copy of it.
    struct Ref
    {
        uint32 entry = 0;
        uint32 copy = 0;
    };

    // On a refusal the message is sent here and the caller marks it sent:
    // SetSentErrorMessage is protected, and only the handler is a friend.
    bool ParseRef(ChatHandler* handler, std::string text, Ref& ref)
    {
        text = Trim(text);
        if (!text.empty() && text[0] == '#')
            text = text.substr(1);
        if (text.empty())
            return false;

        bool numeric = std::all_of(text.begin(), text.end(), [](char c) { return std::isdigit((unsigned char)c) || c == '.'; });
        if (numeric)
        {
            size_t dot = text.find('.');
            uint32 n = uint32(std::strtoul(text.substr(0, dot).c_str(), nullptr, 10));
            if (dot != std::string::npos)
                ref.copy = uint32(std::strtoul(text.substr(dot + 1).c_str(), nullptr, 10));

            if (n >= 1 && n <= ShortId(OBJ_LAST))
                ref.entry = OBJ_FIRST + n - 1;
            else if (IsOurs(n))
                ref.entry = n;
            else
            {
                handler->PSendSysMessage("%u is not one of ours. The ids run from #1 to #%u.", n, ShortId(OBJ_LAST));
                return false;
            }

            if (!sObjectMgr.GetGameObjectInfo(ref.entry))
            {
                handler->PSendSysMessage("There is no object #%u. .obj list shows them.", ShortId(ref.entry));
                return false;
            }
            return true;
        }

        // A NAME: exact first, then a part of one. Two matches is a refusal with
        // the ids, never the first, the same as .npcbot.
        std::string want = Lower(text);
        std::vector<uint32> exact, part;
        for (uint32 e = OBJ_FIRST; e <= OBJ_LAST; ++e)
        {
            GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(e);
            if (!info)
                continue;
            std::string name = Lower(info->name);
            if (name == want)
                exact.push_back(e);
            else if (name.find(want) != std::string::npos)
                part.push_back(e);
        }
        std::vector<uint32> const& found = exact.empty() ? part : exact;

        if (found.empty())
        {
            handler->PSendSysMessage("No object is called \"%s\". .obj list shows them.", text.c_str());
            return false;
        }
        if (found.size() > 1)
        {
            std::string ids;
            for (uint32 e : found)
                ids += (ids.empty() ? "#" : ", #") + std::to_string(ShortId(e));
            handler->PSendSysMessage("%u objects match \"%s\": %s. Use the id.", uint32(found.size()), text.c_str(), ids.c_str());
            return false;
        }
        ref.entry = found[0];
        return true;
    }

    // Ours within `range` of the player, nearest first. `entry` 0 is any of ours.
    std::vector<GameObject*> OursNear(Player* me, float range, uint32 entry)
    {
        std::list<GameObject*> all;
        MaNGOS::AllGameObjectsInRange check(me, range);
        MaNGOS::GameObjectListSearcher<MaNGOS::AllGameObjectsInRange> searcher(all, check);
        Cell::VisitGridObjects(me, searcher, range);

        std::vector<GameObject*> out;
        for (GameObject* go : all)
        {
            if (!IsOurs(go->GetEntry()) || !go->GetDBTableGUIDLow())
                continue;
            if (entry && go->GetEntry() != entry)
                continue;
            out.push_back(go);
        }
        std::sort(out.begin(), out.end(), [me](GameObject* a, GameObject* b) { return me->GetDistance(a) < me->GetDistance(b); });
        return out;
    }

    GameObject* LoadedCopy(Player* me, uint32 entry, uint32 guid)
    {
        return me->GetMap()->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, entry, guid));
    }

    // The copy move, turn, remove and select act on. See the file comment.
    GameObject* PickCopy(ChatHandler* handler, Player* me, std::string const& arg)
    {
        if (Trim(arg).empty())
        {
            GameObject* sel = me->GetMap()->GetGameObject(me->GetSelectedGobj());
            if (sel && IsOurs(sel->GetEntry()) && sel->GetDBTableGUIDLow())
                return sel;

            std::vector<GameObject*> around = OursNear(me, OBJ_REACH, 0);
            if (!around.empty())
                return around.front();

            handler->PSendSysMessage("No object of ours within %.0f yd. Name one, or .obj near 100.", OBJ_REACH);
            return nullptr;
        }

        Ref ref;
        if (!ParseRef(handler, arg, ref))
            return nullptr;

        if (ref.copy)
        {
            std::vector<Copy> copies = CopiesOf(ref.entry);
            if (ref.copy > copies.size())
            {
                handler->PSendSysMessage("#%u has %u placed. .obj where %u lists them.", ShortId(ref.entry), uint32(copies.size()), ShortId(ref.entry));
                return nullptr;
            }
            Copy const& c = copies[ref.copy - 1];
            GameObject* go = c.data->position.mapId == me->GetMapId() ? LoadedCopy(me, ref.entry, c.guid) : nullptr;
            if (!go)
            {
                handler->PSendSysMessage("#%u.%u is not near you. .obj go %u.%u takes you there.", ShortId(ref.entry), ref.copy, ShortId(ref.entry), ref.copy);
            }
            return go;
        }

        std::vector<GameObject*> around = OursNear(me, OBJ_REACH, ref.entry);
        if (!around.empty())
            return around.front();

        handler->PSendSysMessage("No #%u within %.0f yd. .obj where %u lists them.", ShortId(ref.entry), OBJ_REACH, ShortId(ref.entry));
        return nullptr;
    }

    // The time a new copy of this object gets. See the file comment.
    int32 BackTimeFor(uint32 entry, GameObjectInfo const* info)
    {
        if (!Consumable(info))
            return 0;
        for (Copy const& c : CopiesOf(entry))
        {
            int32 t = std::abs(c.data->spawntimesecsmin);
            if (t > 0)
                return t;
        }
        return OBJ_DEFAULT_BACK;
    }

    // One line per copy, numbered: `where`, and `info` under its summary.
    void PrintCopies(ChatHandler* handler, Player* me, uint32 entry, std::vector<Copy> const& copies)
    {
        for (size_t i = 0; i < copies.size(); ++i)
        {
            WorldLocation const& p = copies[i].data->position;
            if (p.mapId == me->GetMapId())
                handler->PSendSysMessage("#%u.%u map %u, %.0f yd", ShortId(entry), uint32(i + 1), p.mapId,
                    me->GetDistance(p.x, p.y, p.z));
            else
                handler->PSendSysMessage("#%u.%u map %u at %.0f %.0f", ShortId(entry), uint32(i + 1), p.mapId, p.x, p.y);
        }
    }

    void Usage(ChatHandler* handler)
    {
        handler->SendSysMessage("Usage: .obj list | info | near | where | go | place | select | move | turn | remove");
        handler->SendSysMessage("  .obj list                     every object of ours");
        handler->SendSysMessage("  .obj info [object]            what it is, and every copy");
        handler->SendSysMessage("  .obj near [yards]               ours around you; selects the nearest");
        handler->SendSysMessage("  .obj where <object>           every copy, numbered");
        handler->SendSysMessage("  .obj go <object or copy>      go to a copy");
        handler->SendSysMessage("  .obj place <object>           one here, facing your way; also .obj add");
        handler->SendSysMessage("  .obj select [object or copy]  pick a copy");
        handler->SendSysMessage("  .obj move [object or copy]    the copy to where you stand");
        handler->SendSysMessage("  .obj turn [object or copy]    the copy to face your way");
        handler->SendSysMessage("  .obj remove [object or copy]  the copy, for good; also .obj del, .obj unplace");
        handler->SendSysMessage("An object is 7, #7 or a name. A copy is 7.2. With no copy given, the selected one, then the nearest within 20 yd.");
    }
}

bool ComfyCommands::HandleObjCommand(ChatHandler* handler, char* args)
{
    char* sub = handler->ExtractLiteralArg(&args);
    std::string action = sub ? Lower(sub) : "";
    std::string rest = args ? Trim(args) : "";

    // The words .npcbot takes too: add and del, place and unplace.
    if (action == "add")
        action = "place";
    if (action == "del" || action == "delete" || action == "unplace")
        action = "remove";

    // ------------------------------------------------------------- back
    // First, because it is the one the console sends: the website's "Comes back
    // after" has already written the rows, and this tells the running server.
    if (action == "back")
    {
        size_t sp = rest.find_last_of(' ');
        if (sp == std::string::npos)
        {
            handler->SendSysMessage("Usage: .obj back <object> <seconds>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        Ref ref;
        if (!ParseRef(handler, rest.substr(0, sp), ref))
        {
            handler->SetSentErrorMessage(true);
            return false;
        }
        int32 secs = std::atoi(rest.substr(sp + 1).c_str());
        if (secs < 0)
            secs = 0;

        uint32 n = 0;
        for (Copy const& c : CopiesOf(ref.entry))
        {
            // The sign of the stored time is "spawned by default", which a
            // change of duration must not flip.
            GameObjectData& d = sObjectMgr.NewGOData(c.guid);
            d.spawntimesecsmin = d.spawntimesecsmin < 0 ? -secs : secs;
            d.spawntimesecsmax = d.spawntimesecsmax < 0 ? -secs : secs;
            if (Map* map = sMapMgr.FindMap(d.position.mapId))
                if (GameObject* go = map->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, ref.entry, c.guid)))
                    go->SetRespawnDelay(secs);
            ++n;
        }
        WorldDatabase.PExecute("UPDATE gameobject SET "
            "spawntimesecsmin = IF(spawntimesecsmin < 0, -%d, %d), "
            "spawntimesecsmax = IF(spawntimesecsmax < 0, -%d, %d) WHERE id = %u",
            secs, secs, secs, secs, ref.entry);

        if (secs)
            handler->PSendSysMessage("%s: %u placed, back %s after use.", Label(ref.entry).c_str(), n, Duration(secs).c_str());
        else
            handler->PSendSysMessage("%s: %u placed, never gone.", Label(ref.entry).c_str(), n);
        return true;
    }

    // ------------------------------------------------------------- list
    if (action == "list")
    {
        std::map<uint32, uint32> placed;
        auto worker = [&](GameObjectDataPair const& p)
        {
            if (IsOurs(p.second.id))
                ++placed[p.second.id];
            return false;
        };
        sObjectMgr.DoGOData(worker);

        uint32 shown = 0;
        for (uint32 e = OBJ_FIRST; e <= OBJ_LAST; ++e)
        {
            if (!sObjectMgr.GetGameObjectInfo(e))
                continue;
            uint32 n = placed.count(e) ? placed[e] : 0;
            if (n)
                handler->PSendSysMessage("%s, %u placed", Label(e).c_str(), n);
            else
                handler->PSendSysMessage("%s, not placed", Label(e).c_str());
            ++shown;
        }
        if (!shown)
            handler->SendSysMessage("No objects yet. Make one in the Workshop, under Objects.");
        return true;
    }

    Player* me = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!me)
    {
        handler->SendSysMessage("Only .obj list and .obj back work from the console.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // ------------------------------------------------------------- info
    if (action == "info")
    {
        uint32 entry = 0;
        GameObject* go = nullptr;
        Ref ref;
        if (!rest.empty() && rest.find('.') == std::string::npos)
        {
            if (!ParseRef(handler, rest, ref))
            {
                handler->SetSentErrorMessage(true);
                return false;
            }
            entry = ref.entry;
        }
        else
        {
            go = PickCopy(handler, me, rest);
            if (!go)
            {
                handler->SetSentErrorMessage(true);
                return false;
            }
            entry = go->GetEntry();
        }

        GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(entry);
        std::vector<Copy> copies = CopiesOf(entry);

        handler->PSendSysMessage("%s, entry %u, %u placed", Label(entry).c_str(), entry, uint32(copies.size()));

        uint32 quest = 0;
        if (info->type == GAMEOBJECT_TYPE_CHEST)
        {
            quest = info->chest.questId;
            std::string items;
            if (QueryResult* result = WorldDatabase.PQuery("SELECT item FROM gameobject_loot_template WHERE entry = %u", info->chest.lootId))
            {
                do
                {
                    ItemPrototype const* proto = sObjectMgr.GetItemPrototype((*result)[0].GetUInt32());
                    if (proto)
                        items += (items.empty() ? "" : ", ") + proto->Name1;
                } while (result->NextRow());
                delete result;
            }
            handler->PSendSysMessage("Gives %s.", items.empty() ? "nothing yet" : items.c_str());
        }
        else if (info->type == GAMEOBJECT_TYPE_GOOBER)
        {
            quest = info->goober.questId > 0 ? uint32(info->goober.questId) : 0;
            handler->SendSysMessage("Gives nothing. A click counts toward a quest.");
        }

        if (quest)
        {
            Quest const* q = sObjectMgr.GetQuestTemplate(quest);
            handler->PSendSysMessage("Only for somebody on \"%s\".", q ? q->GetTitle().c_str() : "?");
        }

        if (Consumable(info))
            handler->PSendSysMessage("Disappears when used, back after %s.", Duration(BackTimeFor(entry, info)).c_str());
        else
            handler->SendSysMessage("Stays when used.");

        if (go)
            handler->PSendSysMessage("This is copy %u, %.0f yd away.", CopyNumber(entry, go->GetDBTableGUIDLow()), me->GetDistance(go));
        PrintCopies(handler, me, entry, copies);
        return true;
    }

    // ------------------------------------------------------------- near
    if (action == "near")
    {
        float range = rest.empty() ? OBJ_REACH : float(std::atof(rest.c_str()));
        if (range <= 0.0f)
            range = OBJ_REACH;
        if (range > 500.0f)
            range = 500.0f;

        std::vector<GameObject*> around = OursNear(me, range, 0);
        if (around.empty())
        {
            handler->PSendSysMessage("No object of ours within %.0f yd.", range);
            return true;
        }
        for (size_t i = 0; i < around.size() && i < OBJ_NEAR_LINES; ++i)
            handler->PSendSysMessage("%s, %.0f yd", Label(around[i]).c_str(), me->GetDistance(around[i]));
        if (around.size() > OBJ_NEAR_LINES)
            handler->PSendSysMessage("and %u more", uint32(around.size() - OBJ_NEAR_LINES));
        me->SetSelectedGobj(around.front()->GetObjectGuid());
        return true;
    }

    // ------------------------------------------------------------- where
    if (action == "where")
    {
        Ref ref;
        if (!ParseRef(handler, rest, ref))
        {
            if (rest.empty())
                handler->SendSysMessage("Usage: .obj where <object>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        std::vector<Copy> copies = CopiesOf(ref.entry);
        if (copies.empty())
        {
            handler->PSendSysMessage("%s is not placed. .obj place %u puts one here.", Label(ref.entry).c_str(), ShortId(ref.entry));
            return true;
        }
        PrintCopies(handler, me, ref.entry, copies);
        return true;
    }

    // ------------------------------------------------------------- go
    if (action == "go")
    {
        Ref ref;
        if (!ParseRef(handler, rest, ref))
        {
            if (rest.empty())
                handler->SendSysMessage("Usage: .obj go <object or copy>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        std::vector<Copy> copies = CopiesOf(ref.entry);
        uint32 n = ref.copy ? ref.copy : 1;
        if (copies.empty() || n > copies.size())
        {
            handler->PSendSysMessage("#%u has %u placed.", ShortId(ref.entry), uint32(copies.size()));
            handler->SetSentErrorMessage(true);
            return false;
        }
        WorldLocation const& p = copies[n - 1].data->position;
        if (!handler->HandleGoHelper(me, p.mapId, p.x, p.y, &p.z, &p.o))
            return false;
        handler->PSendSysMessage("To %s.", Label(ref.entry, n).c_str());
        return true;
    }

    // ------------------------------------------------------------- place
    if (action == "place")
    {
        Ref ref;
        if (!ParseRef(handler, rest, ref))
        {
            if (rest.empty())
                handler->SendSysMessage("Usage: .obj place <object>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(ref.entry);
        if (info->displayId && !sGameObjectDisplayInfoStore.LookupEntry(info->displayId))
        {
            handler->PSendSysMessage("%s has a picture the client does not have (display %u).", Label(ref.entry).c_str(), info->displayId);
            handler->SetSentErrorMessage(true);
            return false;
        }

        int32 back = BackTimeFor(ref.entry, info);
        Map* map = me->GetMap();
        uint32 guid = sObjectMgr.GenerateStaticGameObjectLowGuid();
        if (!guid)
        {
            handler->SendSysMessage(LANG_NO_FREE_STATIC_GUID_FOR_SPAWN);
            handler->SetSentErrorMessage(true);
            return false;
        }

        GameObject* go = new GameObject;
        if (!go->Create(guid, info->id, map, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation(),
                        0.0f, 0.0f, 0.0f, 0.0f, GO_ANIMPROGRESS_DEFAULT, GO_STATE_READY))
        {
            delete go;
            return false;
        }
        go->SetRespawnTime(back);
        sWorld.GetMigration().SetAuthor(handler->m_session->GetUsername());
        go->SaveToDB(map->GetId());
        if (!go->LoadFromDB(guid, map))
        {
            delete go;
            return false;
        }
        map->Add(go);
        sObjectMgr.AddGameobjectToGrid(guid, sObjectMgr.GetGOData(guid));
        me->SetSelectedGobj(go->GetObjectGuid());

        handler->PSendSysMessage("Placed %s.", Label(ref.entry, CopyNumber(ref.entry, guid)).c_str());
        return true;
    }

    // ------------------------------------------------------------- the copy verbs
    if (action != "select" && action != "move" && action != "turn" && action != "remove")
    {
        Usage(handler);
        handler->SetSentErrorMessage(true);
        return false;
    }

    GameObject* go = PickCopy(handler, me, rest);
    if (!go)
    {
        handler->SetSentErrorMessage(true);
        return false;
    }
    std::string label = Label(go);

    if (action == "select")
    {
        me->SetSelectedGobj(go->GetObjectGuid());
        handler->PSendSysMessage("Selected %s, %.0f yd.", label.c_str(), me->GetDistance(go));
        return true;
    }

    if (action == "move" || action == "turn")
    {
        bool move = action == "move";
        float x = move ? me->GetPositionX() : go->GetPositionX();
        float y = move ? me->GetPositionY() : go->GetPositionY();
        float z = move ? me->GetPositionZ() : go->GetPositionZ();
        float o = move ? go->GetOrientation() : me->GetOrientation();

        // Out of the map and back in: the client only redraws a gameobject's
        // place or facing on a fresh create (see .gobject nudge).
        Map* map = go->GetMap();
        map->Remove(go, false);
        go->Relocate(x, y, z, o);
        go->SetFloatValue(GAMEOBJECT_POS_X, x);
        go->SetFloatValue(GAMEOBJECT_POS_Y, y);
        go->SetFloatValue(GAMEOBJECT_POS_Z, z);
        go->SetFloatValue(GAMEOBJECT_FACING, o);
        go->UpdateRotationFields();
        map->Add(go);

        sWorld.GetMigration().SetAuthor(handler->m_session->GetUsername());
        go->SaveToDB();
        go->Refresh();

        handler->PSendSysMessage(move ? "Moved %s here." : "Turned %s to face your way.", label.c_str());
        return true;
    }

    // remove
    if (go->GetOwnerGuid())
    {
        handler->PSendSysMessage("%s belongs to something else and was left alone.", label.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    if (me->GetSelectedGobj() == go->GetObjectGuid())
        me->SetSelectedGobj(ObjectGuid());
    go->SetRespawnTime(0);
    go->Delete();
    sWorld.GetMigration().SetAuthor(handler->m_session->GetUsername());
    go->DeleteFromDB();
    handler->PSendSysMessage("Removed %s.", label.c_str());
    return true;
}
