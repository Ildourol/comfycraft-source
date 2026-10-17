/*
 * .house -- the player-facing side of housing.
 *
 * These are deliberately position-based rather than target-based: gameobjects
 * cannot be selected the way a creature can in 1.12, so "stand next to the
 * chair and type .house remove" is the only interaction that reads naturally.
 */

#include "Housing/HouseMgr.h"

#include "Chat.h"
#include "ComfyCommands.h"
#include "Language.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "World.h"
#include "GameObject.h"
#include "DBCStores.h"
#include "Database/DatabaseEnv.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <vector>

namespace
{
    // How close you have to stand to act on a piece of furniture.
    float const HOUSE_REACH = 10.0f;

    // Phase 1 keeps ordinary players to pure decoration. A private instance
    // limits the damage, but a mailbox or a summoning circle placed there is
    // still a real gameplay shortcut, and some types (transports, traps) simply
    // misbehave when spawned by hand. Phase 3 replaces this with a curated
    // catalogue; until then, anyone at SEC_DEVELOPER or above skips the check
    // so the catalogue can be explored.
    bool IsDecorative(GameObjectInfo const* info)
    {
        switch (info->type)
        {
            case GAMEOBJECT_TYPE_GENERIC:
            case GAMEOBJECT_TYPE_CHAIR:
            case GAMEOBJECT_TYPE_MAP_OBJECT:
                return true;
            default:
                return false;
        }
    }

    // What to call a piece of furniture in a message: its name and its number,
    // together, because each answers a question the other cannot. The name
    // catches the failure worth catching -- having moved something other than
    // what you meant is obvious the instant the line reads "Iron Anvil" -- and
    // the number is the half you can type back at a command.
    //
    // The NUMBER is house_object.slot, never the guid. Guids are still accepted
    // as input; nothing prints them.
    //
    // ONE helper, so every command says the same thing the same way -- move,
    // rotate, scale, drag, delete, select, unselect and edit all print through
    // here, which is also why pointing it at ObjectDisplayName fixed the names
    // in all nine at once. Before that they printed gameobject_template.name
    // raw, which for most of the catalogue is the model PATH.
    // It MOVED to HouseMgr, and this is the shorthand the file kept. The gear
    // needed the same spelling and could not reach a static in here, which is
    // what made it the one route printing a bare number.
    std::string ObjectLabel(uint32 objectGuid)
    {
        return HouseMgr::ObjectLabel(objectGuid);
    }

    // The label for a line about a move, a turn or a drag: "Wooden Table (3)
    // and 4 more" when a group came along. The count is the check that the
    // whole group moved.
    std::string MovedLabel(uint32 objectGuid)
    {
        std::string label = HouseMgr::ObjectLabel(objectGuid);
        uint32 const size = sHouseMgr.GroupSize(objectGuid);
        if (size > 1)
            label += " and " + std::to_string(size - 1) + " more";
        return label;
    }

    // `#<id>` -- the unambiguous way to name an object.
    //
    // A bare trailing id already worked, and still does. It cannot work
    // EVERYWHERE, though, because the middle of these commands is a run of bare
    // numbers and a bare number cannot be told from a distance or an angle:
    // `.house object rotate 8000042` means "spin it eight million degrees", and
    // `.house object move away 8000042` means "shove it eight million yards".
    // Those two spellings have no other form at all -- the bare `rotate` means
    // "face me", so there is nowhere for an id to go.
    //
    // A `#` in front settles it, because ExtractFloat and ExtractUInt32 both
    // refuse a token starting with one and leave the arguments untouched for
    // this to read. So the rule is one sentence: the id goes last, and `#` in
    // front of it is never wrong.
    //
    // Since object numbers became per-house slots this bites less often -- `#3`
    // rather than `#8000042` -- but not less HARD: `rotate 3` still means three
    // degrees, and the bare `rotate` still means "face me" with nowhere to put
    // a number.
    uint32 ExtractHashId(char*& args)
    {
        if (!args || *args != '#')
            return 0;

        char* end = nullptr;
        unsigned long const raw = strtoul(args + 1, &end, 10);
        if (end == args + 1 || !raw || raw > 0xFFFFFFFFul)
            return 0;

        // It has to BE the token, not the front of one: `#12abc` is a typo, and
        // reading it as 12 would act on a real object nobody named.
        if (*end && *end != ' ' && *end != '\t')
            return 0;

        while (*end == ' ' || *end == '\t')
            ++end;

        args = end;
        return uint32(raw);
    }

    // Nearest piece of this house's furniture that the player may rearrange, or
    // 0. Skipping the fabric here is not a second gate -- Resolve is the gate --
    // it stops the nearest wall from shadowing the lamp you actually meant, so
    // "nothing of yours within 10 yards" stays literally true.
    uint32 NearestObject(Player* player, House const* house, float& outDist)
    {
        uint32 best = 0;
        float bestDist = HOUSE_REACH;
        for (HouseObject const* o : sHouseMgr.GetObjects(house->id))
        {
            if (IsHouseFabric(*o))
                continue;

            float d = player->GetDistance(o->x, o->y, o->z);
            if (d < bestDist)
            {
                bestDist = d;
                best = o->guid;
            }
        }
        outDist = bestDist;
        return best;
    }
}

bool ComfyCommands::HandleHouseGoCommand(ChatHandler* handler, char* /*args*/)
{
    Player* player = handler->m_session->GetPlayer();
    std::string error;
    if (!sHouseMgr.SendPlayerHome(player, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    return true;
}

// .house reset [confirm] -- back to the beginning, wherever you stand: a
// claimed home is re-stamped from its own door, a template is emptied but
// keeps its exit. Two-step like clear, and the warning says exactly which of
// those it is about to do, because the two are very different sizes of loss.
bool ComfyCommands::HandleHouseResetCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    House* house = sHouseMgr.GetHouseAt(player);
    if (!house)
    {
        handler->SendSysMessage("You are not in a house.");
        handler->SetSentErrorMessage(true);
        return false;
    }
    if (!sHouseMgr.CanEditHouse(player, *house))
    {
        handler->SendSysMessage("This is not your house.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    bool const isTemplate = house->authorsTemplate != 0;

    std::string what;
    if (isTemplate)
        what = "empty this template and keep its exit (the saved snapshot only changes on the next save)";
    else
    {
        HousePortal const* door = sHouseMgr.GetPortal(house->portalId);
        HouseTemplate const* tpl = (door && door->templateId) ? sHouseMgr.GetTemplate(door->templateId) : nullptr;
        // NO LONGER "what you placed is lost". It is packed into storage first,
        // and the line below counts exactly how much of it survives -- so this
        // half says what happens to the ROOM and leaves the furniture to the
        // forecast, rather than the two contradicting each other.
        if (tpl)
            what = "re-stamp everything from \"" + tpl->name + "\"";
        else
            what = "empty the house completely (your door has no template)";
    }

    if (!handler->ExtractLiteralArg(&args, "confirm"))
    {
        uint32 const count = uint32(sHouseMgr.GetObjects(house->id).size());
        handler->PSendSysMessage("This will %s. %u objects go, and there is no undo.", what.c_str(), count);

        // WHAT IT ACTUALLY COSTS, counted rather than assumed. "What you placed
        // is lost" was true when a reset destroyed everything; now most of it is
        // packed into storage first, and a warning that overstates the damage
        // gets ignored just as fast as one that understates it.
        if (!isTemplate)
        {
            std::string const forecast = sHouseMgr.StowForecast(*house);
            if (!forecast.empty())
                handler->SendSysMessage(forecast.c_str());
        }

        handler->SendSysMessage("Type .house reset confirm if you mean it.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string error;
    if (!sHouseMgr.ResetHouse(player, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (isTemplate)
        handler->SendSysMessage("The template is bare again; the exit stayed. .house template save publishes the new layout.");
    else
        handler->SendSysMessage("Your house is as it was the day you claimed it.");
    return true;
}

bool ComfyCommands::HandleHouseObjectAddCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    uint32 entry;
    if (!handler->ExtractUint32KeyFromLink(&args, "Hgameobject_entry", entry) || !entry)
    {
        handler->SendSysMessage("Usage: .house object add <gameobject entry> [yards in front]");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // Optional, because a building wants more room in front of it than a mug.
    float distance = HOUSE_PLACE_DISTANCE;
    handler->ExtractFloat(&args, distance);

    GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(entry);
    if (!info)
    {
        handler->PSendSysMessage(LANG_GAMEOBJECT_NOT_EXIST, entry);
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (!IsDecorative(info) && handler->GetAccessLevel() < SEC_DEVELOPER)
    {
        handler->PSendSysMessage("\"%s\" is not something you can put in a house.", HouseMgr::ObjectDisplayName(entry).c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    // ASKED BEFORE PLACING, because PlaceObject spends the mark on success and
    // there would be nothing left to ask afterwards. Safe to trust: the only
    // way a mark exists here and is not the thing that got used is PlaceObject
    // refusing outright, and then this message is never printed.
    bool const onMark = sHouseMgr.HasMark(player);

    std::string error;
    if (!sHouseMgr.PlaceObject(player, entry, distance, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    // ONE LINE, AND IT ENDS BY SAYING WHAT TO DO NEXT. Placing is the single
    // most repeated action in housing, so this is the line a decorator reads
    // more than any other -- it used to spend its second half restating
    // mechanics ("It is number 3, and selected", "%.0f yards in front of you")
    // that the object standing in front of you already demonstrates.
    //
    // "Right-click to adjust" earns its place because it is the one thing the
    // room does NOT show: that the piece you just put down is its own handle,
    // with no `.house edit on` needed. Everything else here is the shared
    // ObjectLabel, so the name and number match the select line, the gear and
    // the list exactly.
    uint32 const placed = sHouseMgr.GetSelectedObject(player);
    std::string const label = placed ? HouseMgr::ObjectLabel(placed)
                                     : HouseMgr::ObjectDisplayName(entry);

    if (onMark)
        handler->PSendSysMessage("%s placed on your chalk mark. Right-click to adjust.", label.c_str());
    else
        handler->PSendSysMessage("%s placed. Right-click to adjust.", label.c_str());
    return true;
}

// drag / move / turn / resize / remove all act on the same target, so they
// share the lookup. Returns 0 and has already reported the problem if there is
// nothing to act on.
//
// THREE ANSWERS, IN THIS ORDER, and the order is the whole design:
//
//   1. the number you typed    -- you said which, so nothing else gets a vote
//   2. the object you selected -- you said which EARLIER, and it stands
//   3. the nearest in reach    -- the guess, and now only the last resort
//
// The nearest-object rule used to be the only one, and it is a good default
// that makes a bad only-option: two chairs at a table are half a yard apart,
// and the piece you want to nudge is often one you cannot stand next to. What
// keeps step 2 from becoming its own surprise is that every caller names the
// object it acted on, so "Moved Iron Anvil" is right there when you meant the
// candle.
uint32 ComfyCommands::HouseTarget(ChatHandler* handler, char* &args, House*& house, bool useSelection)
{
    Player* player = handler->m_session->GetPlayer();

    house = sHouseMgr.GetHouseAt(player);
    if (!house)
    {
        handler->SendSysMessage("You are not in a house.");
        handler->SetSentErrorMessage(true);
        return 0;
    }

    // `#n` first: ExtractUInt32 refuses the '#' and would leave it as unread
    // junk, which used to fail silently -- see below.
    uint32 typed = ExtractHashId(args);

    if (!typed && !handler->ExtractOptUInt32(&args, typed, 0))
    {
        // A SILENT `return 0` LIVED HERE, and it is how a mistyped number
        // looked exactly like nothing happening. Anything left over at this
        // point is text where a number should be, so say which rule was broken
        // rather than leaving the player to guess at the grammar.
        handler->PSendSysMessage("I cannot read \"%s\" as an object number. The number goes LAST, and # in front of it always works:", args);
        handler->SendSysMessage("  .house object move away 2 #3        .house object rotate #3");
        handler->SetSentErrorMessage(true);
        return 0;
    }

    // A number is a per-house SLOT, the small one .house object list prints.
    // A raw guid still resolves, because anything at or above HOUSE_GO_GUID_MIN
    // cannot be a slot -- but nothing hands them out any more.
    uint32 guid = 0;
    if (typed)
    {
        guid = sHouseMgr.ResolveObjectRef(*house, typed);
        if (!guid)
        {
            handler->PSendSysMessage("Nothing in this house is number %u. .house object list prints them.", typed);
            handler->SetSentErrorMessage(true);
            return 0;
        }
    }

    // `.house object select` passes false: it is the command for CHANGING what
    // is selected, so resolving through the selection would make its bare form
    // a no-op at exactly the moment it is asked to move on.
    if (!guid && useSelection)
        guid = sHouseMgr.GetSelectedObject(player);

    if (!guid)
    {
        float dist = 0.0f;
        guid = NearestObject(player, house, dist);
        if (!guid)
        {
            handler->PSendSysMessage("Nothing of yours within %g yards. Stand closer, or use the number from .house object list.", HOUSE_REACH);
            handler->SetSentErrorMessage(true);
            return 0;
        }
    }
    return guid;
}

// .house object select [id] -- choose the object every other command will act
// on. Bare form takes the nearest, which is the same object the bare commands
// would have picked, so it is also the way to PIN that guess before walking
// away from it.
//
// The GEAR says so in its own greeting, which it redraws on the toggle; the
// chat command has no menu to show state, so it says so here. One line each,
// and neither repeats the other -- an earlier pass put the announcement inside
// SelectObject so both routes shared it, and the result was the gear printing
// two lines about one thing.
bool ComfyCommands::HandleHouseObjectSelectCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    // `select none` still works. It reads naturally, it is what the panel
    // button sent before `unselect` existed, and it costs three words.
    if (handler->ExtractLiteralArg(&args, "none") || handler->ExtractLiteralArg(&args, "clear")
        || handler->ExtractLiteralArg(&args, "off"))
        return HandleHouseObjectUnselectCommand(handler, args);

    House* house = nullptr;
    uint32 const guid = HouseTarget(handler, args, house, false);
    if (!guid)
        return false;

    std::string error;
    if (!sHouseMgr.SelectObject(player, guid, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    // The trailing "Use .house object unselect to clear it." is gone. It was on
    // every select, and selecting happens constantly while decorating -- a
    // second sentence teaching the same escape hatch for the tenth time is what
    // makes a chat frame unreadable. It is in `.house` and on the gear.
    handler->PSendSysMessage("Selected %s.", ObjectLabel(guid).c_str());
    return true;
}

// .house object unselect -- the way back to "whatever is nearest", as a verb of
// its own rather than an argument. It is what the select message tells you to
// type, and a command you are told to use should be a command.
bool ComfyCommands::HandleHouseObjectUnselectCommand(ChatHandler* handler, char* /*args*/)
{
    Player* player = handler->m_session->GetPlayer();

    // Read it BEFORE clearing, so the line can name what it let go of. Saying
    // WHICH matters more here than anywhere: the selection was invisible, so
    // this is the last chance to confirm it was the one you thought it was.
    uint32 const was = sHouseMgr.GetSelectedObject(player);
    std::string const label = was ? ObjectLabel(was) : std::string();
    sHouseMgr.ClearSelection(player);

    if (was)
        handler->PSendSysMessage("Unselected %s. Commands act on the nearest object again.", label.c_str());
    else
        handler->SendSysMessage("Nothing was selected. Commands act on the nearest object.");
    return true;
}

// .house object group [id] -- put the target in the same group as the
// selected piece. The right-click menu's "Group with" row, typed: select the
// table once, then group each chair with it.
bool ComfyCommands::HandleHouseObjectGroupCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    uint32 const withGuid = sHouseMgr.GetSelectedObject(player);
    if (!withGuid)
    {
        handler->SendSysMessage("Select the piece to group with first: .house object select <number>.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // false: the bare form must not resolve to the selection itself.
    House* house = nullptr;
    uint32 const guid = HouseTarget(handler, args, house, false);
    if (!guid)
        return false;

    std::string error;
    if (!sHouseMgr.JoinGroup(player, guid, withGuid, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    handler->PSendSysMessage("Grouped %s with %s: %u pieces.", ObjectLabel(guid).c_str(),
                             ObjectLabel(withGuid).c_str(), sHouseMgr.GroupSize(guid));
    return true;
}

// .house object ungroup [id] -- take one piece out of its group.
bool ComfyCommands::HandleHouseObjectUngroupCommand(ChatHandler* handler, char* args)
{
    House* house = nullptr;
    uint32 const guid = HouseTarget(handler, args, house);
    if (!guid)
        return false;

    std::string error;
    if (!sHouseMgr.LeaveGroup(handler->m_session->GetPlayer(), guid, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    handler->PSendSysMessage("Ungrouped %s.", ObjectLabel(guid).c_str());
    return true;
}

// .house object chalk [clear] -- what the Decorator's Chalk set, in words.
//
// NAMED AFTER THE ITEM, not after the state, and not `mark`. `.house object
// marker` already exists one letter away and tunes the selection glow's model
// -- two commands whose names differ by one character, for two different
// things, is a bug waiting to be typed. Naming this after the object in your
// bag also makes it findable from the only direction anybody arrives from:
// you are holding a Decorator's Chalk and you want to know what it did.
//
// THE CHALK IS THE VERB AND THIS IS NOT A SECOND ONE. There is deliberately no
// way to SET a mark from here: the whole reason the item exists is that a dot
// command cannot raise the client's ground reticle, so a `.house object chalk
// <x> <y> <z>` would be asking a player to type coordinates at the one part of
// housing built to stop them doing that.
//
// What it is for is the two things the item cannot do -- say where the mark
// actually is, and rub one out without spending it on furniture you did not
// want.
bool ComfyCommands::HandleHouseObjectChalkCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    if (handler->ExtractLiteralArg(&args, "clear") || handler->ExtractLiteralArg(&args, "none")
        || handler->ExtractLiteralArg(&args, "off"))
    {
        if (!sHouseMgr.HasMark(player))
        {
            handler->SendSysMessage("There is no chalk mark here.");
            return true;
        }

        sHouseMgr.ClearMark(player);
        handler->SendSysMessage("Chalk mark rubbed out.");
        return true;
    }

    float at[3];
    if (!sHouseMgr.GetMark(player, at))
    {
        handler->SendSysMessage("No chalk mark. Use the Decorator's Chalk and click the floor.");
        return true;
    }

    // Distance rather than coordinates, because a number you cannot walk to is
    // not an answer -- and the glow standing on it is the real answer anyway.
    // This is for "is it still there, and did I leave it in the other room".
    handler->PSendSysMessage("Chalk mark is %.1f yards away. The next thing you place lands on it.",
                    player->GetDistance(at[0], at[1], at[2]));
    return true;
}

// .house object drag [id] -- snap it to where you stand.
bool ComfyCommands::HandleHouseObjectDragCommand(ChatHandler* handler, char* args)
{
    House* house = nullptr;
    uint32 guid = HouseTarget(handler, args, house);
    if (!guid)
        return false;

    std::string error;
    if (!sHouseMgr.DragObject(handler->m_session->GetPlayer(), guid, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->PSendSysMessage("Dragged %s to you.", MovedLabel(guid).c_str());
    return true;
}

// .house object move <x> <y> <z> [id] -- nudge by yards along the world axes.
// Z is up, so `.house object move 0 0 1` raises it a yard. .gps tells you which
// way X and Y run.
// Two forms. The world-axis one is exact and scriptable; the direction words are
// resolved against the player's own facing.
//
// THE WORDS HAVE TO BE RESOLVED HERE, not in the addon. There is no API in the
// 1.12 client for the player's own orientation -- GetPlayerFacing is 3.x -- so
// an addon literally cannot work out which way "left" is and has no choice but
// to ask the server. That is also why the addon's own compass was stuck on
// absolute world axes until this existed.
bool ComfyCommands::HandleHouseObjectMoveCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    char* const save = args;

    if (!handler->ExtractFloat(&args, dx) || !handler->ExtractFloat(&args, dy) || !handler->ExtractFloat(&args, dz))
    {
        // A partly-consumed numeric parse leaves args advanced, so rewind before
        // trying to read the same text as a word.
        args = save;
        dx = dy = dz = 0.0f;

        bool away    = handler->ExtractLiteralArg(&args, "away")    || handler->ExtractLiteralArg(&args, "forward");
        bool towards = !away && (handler->ExtractLiteralArg(&args, "towards") || handler->ExtractLiteralArg(&args, "toward")
                                 || handler->ExtractLiteralArg(&args, "back"));
        bool left    = !away && !towards && handler->ExtractLiteralArg(&args, "left") != nullptr;
        bool right   = !away && !towards && !left && handler->ExtractLiteralArg(&args, "right") != nullptr;
        bool up      = !away && !towards && !left && !right && handler->ExtractLiteralArg(&args, "up") != nullptr;
        bool down    = !away && !towards && !left && !right && !up && handler->ExtractLiteralArg(&args, "down") != nullptr;

        if (!away && !towards && !left && !right && !up && !down)
        {
            handler->SendSysMessage("Usage: .house object move <away|towards|left|right|up|down> [yards]");
            handler->SendSysMessage("  or:  .house object move <x> <y> <z>   exact yards along the world axes, Z is up.");
            handler->SendSysMessage("  .house object drag              snaps it to where you are standing instead.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        float dist = 1.0f;
        handler->ExtractFloat(&args, dist);

        float const face = player->GetOrientation();
        if      (away)    { dx =  cos(face) * dist;               dy =  sin(face) * dist; }
        else if (towards) { dx = -cos(face) * dist;               dy = -sin(face) * dist; }
        else if (left)    { dx =  cos(face + M_PI_F / 2) * dist;  dy =  sin(face + M_PI_F / 2) * dist; }
        else if (right)   { dx =  cos(face - M_PI_F / 2) * dist;  dy =  sin(face - M_PI_F / 2) * dist; }
        else if (up)      { dz =  dist; }
        else              { dz = -dist; }
    }

    House* house = nullptr;
    uint32 guid = HouseTarget(handler, args, house);
    if (!guid)
        return false;

    std::string error;
    if (!sHouseMgr.OffsetObject(player, guid, dx, dy, dz, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->PSendSysMessage("Moved %s by (%.2f, %.2f, %.2f).", MovedLabel(guid).c_str(), dx, dy, dz);
    return true;
}

// .house object rotate <degrees> [id] -- spin it. Bare form points it your way.
// Yaw only, by decision rather than limitation: the client renders a full
// quaternion (1,378 stock spawns tilt), rot0-3 round-trip through our schema,
// and the day leaning paintings are wanted the missing piece is one
// Euler-to-quaternion write command.
bool ComfyCommands::HandleHouseObjectRotateCommand(ChatHandler* handler, char* args)
{
    // `rotate away` -- point it the other way. Checked before the number,
    // since ExtractFloat would otherwise swallow the word and read 0 degrees.
    float degrees = 0.0f;
    bool faceMe = true;
    bool away = handler->ExtractLiteralArg(&args, "away") != nullptr;
    if (away)
        degrees = 180.0f;
    else
        faceMe = !handler->ExtractFloat(&args, degrees);

    House* house = nullptr;
    uint32 guid = HouseTarget(handler, args, house);
    if (!guid)
        return false;

    std::string error;
    if (!sHouseMgr.TurnObject(handler->m_session->GetPlayer(), guid, degrees, faceMe, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (away)
        handler->PSendSysMessage("Turned %s to face away from you.", MovedLabel(guid).c_str());
    else if (faceMe)
        handler->PSendSysMessage("Turned %s to face the way you do.", MovedLabel(guid).c_str());
    else
        handler->PSendSysMessage("Spun %s by %.0f degrees.", MovedLabel(guid).c_str(), degrees);
    return true;
}

bool ComfyCommands::HandleHouseObjectDelCommand(ChatHandler* handler, char* args)
{
    House* house = nullptr;
    uint32 guid = HouseTarget(handler, args, house);
    if (!guid)
        return false;

    // Read the name BEFORE removing the row, or the one message that matters
    // most -- WHAT you just deleted -- is the one that cannot be written.
    std::string const label = ObjectLabel(guid);

    std::string error;
    uint32 returned = 0;
    if (!sHouseMgr.RemoveObject(handler->m_session->GetPlayer(), guid, error, &returned))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    // One command, two endings, and the difference is worth saying: furniture
    // that came out of a bag went back into one, and everything else is gone
    // for good. The gear's own button has always said "Picked up" -- it is now
    // telling the truth on both routes.
    //
    // AND THE PICK-UP LINE IS WHERE MOVING SOMETHING IS TAUGHT. There is no
    // reposition verb: putting a piece somewhere else is picking it up and
    // placing it again, which since the crates carry a ground reticle is two
    // clicks. That is only obvious to somebody who already knows, so the line
    // that hands the crate back is the one place to say it. It stays ONE line.
    if (returned)
        handler->PSendSysMessage("Picked up %s. Right-click to place it again.", label.c_str());
    else
        handler->PSendSysMessage("Removed %s.", label.c_str());
    return true;
}

// .house object edit [id] -- one gear, on one object, without switching global
// edit mode on. The partial-session machinery has backed this since the day
// placing an object started summoning its own gear; this merely gives it a
// command. Bare form toggles the nearest object's gear.
bool ComfyCommands::HandleHouseObjectEditCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    House* house = nullptr;
    uint32 guid = HouseTarget(handler, args, house);
    if (!guid)
        return false;

    if (!sHouseMgr.CanEditHouse(player, *house))
    {
        handler->SendSysMessage("This is not your house.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // An explicit id is the one way to reach the fabric here, since HouseTarget
    // already skips it when picking the nearest. Say why rather than doing
    // nothing quietly -- Reconcile would drop it and the message below would
    // lie about it.
    if (HouseObject const* o = sHouseMgr.GetObject(guid))
        if (IsHouseFabric(*o))
        {
            handler->SendSysMessage("That came with the house.");
            handler->SetSentErrorMessage(true);
            return false;
        }

    // Global edit owns the whole room and Reconcile rebuilds its wanted set
    // from the house on every pass, so a per-object toggle would be undone
    // before the next tick. Say so rather than flickering. (An `excluded` set
    // that would have made this work was built and removed on 2026-09-03 --
    // see HouseEdit in HouseMgr.h for why a global session with a hole in it
    // is the wrong shape.)
    if (sHouseMgr.IsEditingAll(player))
    {
        handler->SendSysMessage("Edit mode is on for everything. .house edit off first.");
        return true;
    }

    // Toggle on the WANTED set, which since the gears went is simply the set of
    // objects this player has been told are clickable.
    if (sHouseMgr.InEditSession(player, guid))
    {
        sHouseMgr.DropFromEdit(player, guid);
        handler->PSendSysMessage("%s released.", ObjectLabel(guid).c_str());
    }
    else
    {
        sHouseMgr.AddToEdit(player, guid);
        handler->PSendSysMessage("%s lights up. Right-click it to arrange.", ObjectLabel(guid).c_str());
    }
    return true;
}

// .house object clear -- take everything out. Bare, it only reports and warns;
// the word `confirm` is what actually does it.
//
// The addon puts a confirmation button in front of this, but the command has to
// carry its own guard as well: it is one word away from .house object list, it
// is typed by hand, and undo does not exist. Making the destructive form the
// longer one to type is the cheapest protection available.
bool ComfyCommands::HandleHouseObjectClearCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    House* house = sHouseMgr.GetHouseAt(player);
    if (!house)
    {
        handler->SendSysMessage("You are not in a house.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // What anyone can clear is the placements; the things a template stamped
    // in stay whatever they type or whatever their rank -- stamped rows are
    // only editable in the template itself. In a template's authoring house
    // every row IS a placement, so authoring clears fine.
    uint32 const total = uint32(sHouseMgr.GetObjects(house->id).size());
    uint32 const count = sHouseMgr.CountPlayerObjects(*house);
    if (!count)
    {
        handler->SendSysMessage(total ? "Nothing here is yours to clear. It all came with the house."
                             : "Your house is already empty.");
        return true;
    }

    if (!handler->ExtractLiteralArg(&args, "confirm"))
    {
        handler->PSendSysMessage("This will remove %u objects from your house, and there is no undo.", count);
        handler->SendSysMessage("Type .house object clear confirm if you mean it.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 removed = 0, kept = 0, noRoom = 0;
    std::string error;
    if (!sHouseMgr.ClearHouse(player, removed, kept, noRoom, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (kept)
        handler->PSendSysMessage("Removed %u objects, and kept the %u that came with the house.", removed, kept);
    else if (!noRoom)
        handler->PSendSysMessage("Removed %u objects. Your house is empty.", removed);
    else
        handler->PSendSysMessage("Removed %u objects.", removed);

    // Said separately rather than folded into the count, because it is the one
    // line that asks for something: those objects are still standing.
    if (noRoom)
        handler->PSendSysMessage("%u would not fit in your bags and are still here.", noRoom);
    return true;
}

// .house object handle [displayId] [scale] -- what the edit gear looks like.
//
// A tuning command, not a feature. gameobject_template has no reload in this
// core, so trying a marker model from SQL costs a full restart every time;
// applied at summon time instead, a new look is .house edit off and on again.
// Bare form reports, "reset" goes back to the template.
//
// Whatever this settles on belongs in sql/custom as a template change. Nothing
// here is saved.
// .house object marker [<displayId>] [scale] [lift] | reset -- what the glow on
// the selected object looks like, live. Same reason as the handle and the
// portal before it: gameobject_template has no reload, so tuning this from SQL
// costs a restart per attempt, and picking how big a marker should be is
// exactly the job that wants twenty tries.
//
// It re-summons at the end rather than asking you to reselect: the display and
// size come out of the cached template at summon time, so an existing glow is
// already the old one and always would be.
bool ComfyCommands::HandleHouseObjectMarkerCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    if (handler->ExtractLiteralArg(&args, "reset"))
    {
        sHouseMgr.SetSelectMarkLook(0, HOUSE_SELECT_MARK_SCALE, HOUSE_SELECT_MARK_LIFT);
        sHouseMgr.RefreshSelectionMarker(player);
        handler->PSendSysMessage("Selection glow back to the compiled default: scale %g, lift %g.",
                        HOUSE_SELECT_MARK_SCALE, HOUSE_SELECT_MARK_LIFT);
        return true;
    }

    uint32 display = 0;
    float scale = 0.0f;
    float lift = sHouseMgr.GetSelectMarkLift();

    handler->ExtractOptUInt32(&args, display, 0);
    handler->ExtractFloat(&args, scale);
    handler->ExtractFloat(&args, lift);

    if (!display && scale <= 0.0f)
    {
        handler->SendSysMessage("Usage: .house object marker <displayId> [scale] [lift]");
        handler->SendSysMessage("   or: .house object marker reset");
        handler->SendSysMessage("  The glow standing on the selected object. 21700 is WhiteGlow_High.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    sHouseMgr.SetSelectMarkLook(display, scale, lift);
    sHouseMgr.RefreshSelectionMarker(player);
    handler->PSendSysMessage("Selection glow: display %u, scale %g, lift %g.",
                    display, scale, lift);
    return true;
}

//== the world-side entrances =================================================
//
// Developer commands, one handler per leaf -- the nested command table
// consumes the subcommand words, so each of these sees only its own tail.
// `.house template exit` is the opposite number for the way out.

// .house entrance create <template|none> [name...] -- a new door where you
// stand, bound to the inside it leads to. It lands three yards AHEAD of you
// facing back, so placing it and then walking forward is what uses it.
bool ComfyCommands::HandleHouseEntranceCreateCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    char* tplTok = handler->ExtractLiteralArg(&args);
    if (!tplTok)
    {
        handler->SendSysMessage("Usage: .house entrance create <template|none> [name]");
        handler->SendSysMessage("Templates: .house template list. `none` makes a door that claims an empty house.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 templateId = 0;
    if (strcmp(tplTok, "none") != 0)
    {
        HouseTemplate const* tpl = sHouseMgr.GetTemplateByName(tplTok);
        if (!tpl)
        {
            handler->PSendSysMessage("No template called \"%s\". .house template list shows them; `none` is allowed.", tplTok);
            handler->SetSentErrorMessage(true);
            return false;
        }
        templateId = tpl->id;
    }

    std::string name = args ? args : "";
    while (!name.empty() && name[name.size() - 1] == ' ')
        name.erase(name.size() - 1);

    uint32 newId = 0;
    std::string error;
    if (!sHouseMgr.AddPortal(player, name, templateId, newId, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->PSendSysMessage("Entrance %u added. .house entrance list shows them all.", newId);
    return true;
}

bool ComfyCommands::HandleHouseEntranceListCommand(ChatHandler* handler, char* /*args*/)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;

    std::vector<HousePortal const*> portals = sHouseMgr.GetPortals();
    if (portals.empty())
    {
        handler->SendSysMessage("No entrances placed. Stand in a doorway and use .house entrance create.");
        return true;
    }

    uint32 home = 0;
    if (player)
        if (House const* h = sHouseMgr.GetHouseByAccount(player->GetSession()->GetAccountId()))
            home = h->portalId;

    for (size_t i = 0; i < portals.size(); ++i)
    {
        HousePortal const* p = portals[i];
        std::string tplName = "no template";
        if (p->templateId)
            if (HouseTemplate const* tpl = sHouseMgr.GetTemplate(p->templateId))
                tplName = tpl->name;
        handler->PSendSysMessage("%u: %s map %u at %.1f %.1f %.1f [%s]%s",
                        p->id, p->name.empty() ? "(unnamed)" : p->name.c_str(),
                        p->map, p->x, p->y, p->z, tplName.c_str(),
                        p->id == home ? "  <- your home" : "");
    }
    return true;
}

// .house entrance move [id] -- bring a door to three yards ahead of you.
// Placing a door is overwhelmingly an act of ADJUSTMENT -- a yard left, turned
// to face the other way -- which is why moving is its own verb and not a side
// effect of anything else.
bool ComfyCommands::HandleHouseEntranceMoveCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    uint32 id = 0;
    if (!handler->ExtractUInt32(&args, id))
    {
        if (HousePortal const* spot = sHouseMgr.GetPortalNear(player))
            id = spot->id;
    }
    if (!id)
    {
        handler->PSendSysMessage("Stand within %g yards of an entrance, or give its id: .house entrance move <id>",
                        HOUSE_PORTAL_NEAR);
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string error;
    if (!sHouseMgr.MovePortal(player, id, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->PSendSysMessage("Entrance %u moved here.", id);
    return true;
}

bool ComfyCommands::HandleHouseEntranceRemoveCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;

    uint32 id = 0;
    if (!handler->ExtractUInt32(&args, id))
    {
        // Ranged, because this one DELETES. Unranged it took the nearest door
        // on the map, which from the wrong end of Ironforge is a door you have
        // never seen and cannot get back.
        HousePortal const* spot = sHouseMgr.GetPortalNear(player);
        if (!spot)
        {
            handler->PSendSysMessage("Stand within %g yards of an entrance, or give its id: .house entrance remove <id>",
                            HOUSE_PORTAL_NEAR);
            handler->SetSentErrorMessage(true);
            return false;
        }
        id = spot->id;
    }

    std::string error;
    if (!sHouseMgr.RemovePortal(id, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->PSendSysMessage("Entrance %u removed.", id);
    return true;
}

bool ComfyCommands::HandleHouseEntranceWhereCommand(ChatHandler* handler, char* /*args*/)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;

    // The one command that stays UNRANGED: "which is nearest" is the whole
    // question. It reports the distance because that is now what decides
    // whether the bare form of the other four will reach it.
    HousePortal const* spot = sHouseMgr.GetNearestPortal(player);
    if (!spot)
    {
        handler->SendSysMessage("No entrance on this map. .house entrance list shows them all.");
        return true;
    }

    float const dist = player->GetDistance(spot->x, spot->y, spot->z);

    handler->PSendSysMessage("Nearest entrance: %u, map %u at %.1f %.1f %.1f, %.1f yards away%s.",
                    spot->id, spot->map, spot->x, spot->y, spot->z, dist,
                    dist <= HOUSE_PORTAL_NEAR ? "" : ", too far for the bare commands");
    return true;
}

// A developer utility these days -- players claim their home at a door's gear,
// where claiming and homing are one act. Non-destructive.
bool ComfyCommands::HandleHouseEntranceHomeCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;

    uint32 id = 0;
    if (!handler->ExtractUInt32(&args, id))
    {
        // No id given: the one you are standing at, which is the whole point
        // of being able to walk up to a door and claim it.
        HousePortal const* spot = sHouseMgr.GetPortalNear(player);
        if (!spot)
        {
            handler->PSendSysMessage("Stand within %g yards of an entrance, or give its id: .house entrance home <id>",
                            HOUSE_PORTAL_NEAR);
            handler->SetSentErrorMessage(true);
            return false;
        }
        id = spot->id;
    }

    std::string error;
    if (!sHouseMgr.SetHomePortal(player, id, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->PSendSysMessage("Entrance %u is your home now.", id);
    return true;
}

// .house entrance template <template|none> [id] -- retro-bind a door to the
// inside it leads to. Houses already claimed through it keep their copies:
// stamping copies, it never references back.
bool ComfyCommands::HandleHouseEntranceTemplateCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;

    char* tplTok = handler->ExtractLiteralArg(&args);
    if (!tplTok)
    {
        handler->SendSysMessage("Usage: .house entrance template <template|none> [id]");
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 templateId = 0;
    if (strcmp(tplTok, "none") != 0)
    {
        HouseTemplate const* tpl = sHouseMgr.GetTemplateByName(tplTok);
        if (!tpl)
        {
            handler->PSendSysMessage("No template called \"%s\". .house template list shows them.", tplTok);
            handler->SetSentErrorMessage(true);
            return false;
        }
        templateId = tpl->id;
    }

    uint32 id = 0;
    if (!handler->ExtractUInt32(&args, id))
    {
        if (HousePortal const* spot = sHouseMgr.GetPortalNear(player))
            id = spot->id;
    }
    if (!id)
    {
        handler->PSendSysMessage("Stand within %g yards of an entrance, or give its id: .house entrance template <template|none> <id>",
                        HOUSE_PORTAL_NEAR);
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string error;
    if (!sHouseMgr.SetPortalTemplate(id, templateId, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    if (templateId)
        handler->PSendSysMessage("Entrance %u now leads to \"%s\". Houses already claimed through it are untouched.", id, tplTok);
    else
        handler->PSendSysMessage("Entrance %u is untemplated now. Claiming there gives an empty house.", id);
    return true;
}

// Same tuning knob as .house object handle, and for the same reason:
// gameobject_template has no reload, so trying portal models from SQL would
// cost a restart each time. Not saved -- what it settles on belongs in
// sql/custom/027.
bool ComfyCommands::HandleHouseEntranceModelCommand(ChatHandler* handler, char* args)
{
    if (handler->ExtractLiteralArg(&args, "reset"))
    {
        sHouseMgr.SetPortalLook(0, 0.0f);
        handler->SendSysMessage("Portals reset to the gameobject_template rows.");
        return true;
    }

    uint32 display = 0;
    if (!handler->ExtractUInt32(&args, display) || !display)
    {
        uint32 const cur = sHouseMgr.GetPortalDisplay();
        if (!cur)
            handler->SendSysMessage("Portals are using the gameobject_template rows as-is.");
        else
            handler->PSendSysMessage("Portal override: display %u, scale %g.", cur, sHouseMgr.GetPortalScale());
        handler->SendSysMessage("Usage: .house entrance model <displayId> [scale]   .house entrance model reset");
        handler->SendSysMessage("       21585 purple spell portal (the current row)");
        handler->SendSysMessage("       4393 Darnassus  4394 Ironforge  4395 Orgrimmar");
        handler->SendSysMessage("       4396 Stormwind  4397 Thunder Bluff  4398 Undercity");
        return true;
    }

    float scale = 1.0f;
    handler->ExtractFloat(&args, scale);
    if (scale <= 0.0f)
        scale = 1.0f;

    sHouseMgr.SetPortalLook(display, scale);
    handler->PSendSysMessage("Portals now display %u at scale %g.", display, scale);
    handler->SendSysMessage("The one inside a house reappears within a second; re-enter to see it.");
    return true;
}

// The gear in the middle of a portal: how big, and how high it floats.
// Live, because gameobject_template has no reload and picking a size is the
// job that wants twenty tries. Not saved -- see sql/custom/034.
bool ComfyCommands::HandleHouseEntranceHandleCommand(ChatHandler* handler, char* args)
{
    if (handler->ExtractLiteralArg(&args, "reset"))
    {
        sHouseMgr.SetPortalMarkScale(0.0f);
        sHouseMgr.SetPortalMarkOffset(0.0f, 0.0f, HOUSE_PORTAL_MARK_LIFT);
        sHouseMgr.SetMarkRange(HOUSE_MARK_SHOW_RANGE, HOUSE_MARK_HIDE_RANGE);
        handler->PSendSysMessage("Portal gear back to the row: scale %g at %g %g %g, %g/%g yards.",
                        sHouseMgr.GetPortalMarkScale(), sHouseMgr.GetPortalMarkX(),
                        sHouseMgr.GetPortalMarkY(), sHouseMgr.GetPortalMarkZ(),
                        sHouseMgr.GetMarkShow(), sHouseMgr.GetMarkHide());
        return true;
    }

    // Two numbers, the second optional: a bare `range 6` keeps the gap it
    // already had rather than making you restate it, which is what you want
    // when you are only feeling for the near edge.
    if (handler->ExtractLiteralArg(&args, "range"))
    {
        float show = 0.0f, hide = 0.0f;
        if (!handler->ExtractFloat(&args, show))
        {
            handler->SendSysMessage("Usage: .house entrance handle range <show> [hide]");
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (!handler->ExtractFloat(&args, hide))
            hide = show + (sHouseMgr.GetMarkHide() - sHouseMgr.GetMarkShow());

        sHouseMgr.SetMarkRange(show, hide);
        handler->PSendSysMessage("Portal gears appear within %g yards, go past %g.",
                        sHouseMgr.GetMarkShow(), sHouseMgr.GetMarkHide());

        // WORTH SAYING WHENEVER IT BITES. The portal takes you through at
        // HOUSE_PORTAL_RANGE, so anything at or under that is a gear you can
        // never stand close enough to see.
        if (sHouseMgr.GetMarkShow() <= HOUSE_PORTAL_RANGE + 0.5f)
            handler->PSendSysMessage("That is inside the %g yards where the portal takes you through. Try more.",
                            HOUSE_PORTAL_RANGE);
        return true;
    }

    if (handler->ExtractLiteralArg(&args, "scale"))
    {
        float scale = 0.0f;
        if (!handler->ExtractFloat(&args, scale) || scale <= 0.0f)
        {
            handler->SendSysMessage("Usage: .house entrance handle scale <n>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        sHouseMgr.SetPortalMarkScale(scale);
        handler->PSendSysMessage("Portal gear now scale %g. It reappears within a second.",
                        sHouseMgr.GetPortalMarkScale());
        return true;
    }

    if (handler->ExtractLiteralArg(&args, "move"))
    {
        // Yards on the WORLD axes, Z up -- the same convention .house object
        // move uses. All three are required: a partial move would silently
        // mean "and zero the others", which is not what anybody types it for.
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!handler->ExtractFloat(&args, x) || !handler->ExtractFloat(&args, y) || !handler->ExtractFloat(&args, z))
        {
            handler->SendSysMessage("Usage: .house entrance handle move <x> <y> <z>   (yards from the portal, Z up)");
            handler->SetSentErrorMessage(true);
            return false;
        }
        sHouseMgr.SetPortalMarkOffset(x, y, z);
        handler->PSendSysMessage("Portal gear now at %g %g %g from the portal.",
                        sHouseMgr.GetPortalMarkX(), sHouseMgr.GetPortalMarkY(),
                        sHouseMgr.GetPortalMarkZ());
        return true;
    }

    handler->PSendSysMessage("Portal gear: scale %g, offset %g %g %g from the portal.",
                    sHouseMgr.GetPortalMarkScale(), sHouseMgr.GetPortalMarkX(),
                    sHouseMgr.GetPortalMarkY(), sHouseMgr.GetPortalMarkZ());
    handler->PSendSysMessage("Appears within %g yards, goes past %g.",
                    sHouseMgr.GetMarkShow(), sHouseMgr.GetMarkHide());
    handler->SendSysMessage("Usage: .house entrance handle range <show> [hide]");
    handler->SendSysMessage("       .house entrance handle scale <n>");
    handler->SendSysMessage("       .house entrance handle move <x> <y> <z>");
    handler->SendSysMessage("       .house entrance handle reset");
    return true;
}

//== templates ================================================================
//
// The authoring loop: create (which travels there), decorate with the normal
// object commands, place the exit, save. What SAVE freezes is what claims
// stamp; unsaved edits never reach a player.

// One word, because .house entrance create takes the name as a single token.
bool ComfyCommands::HandleHouseTemplateCreateCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    char* name = handler->ExtractLiteralArg(&args);
    if (!name || handler->ExtractLiteralArg(&args))
    {
        handler->SendSysMessage("Usage: .house template create <name>   (one word)");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string error;
    if (!sHouseMgr.CreateTemplate(player, name, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->PSendSysMessage("Template \"%s\" created. You are standing in it. Decorate, place .house template exit, then .house template save.", name);
    return true;
}

// .house template exit -- the way out, placed rather than computed, and part
// of the template rather than of the house stamped from it. `.house entrance
// move` is this command's opposite number: that one refuses to run inside a
// house, this one refuses to run anywhere but inside a template, because
// GetAuthoredTemplateAt asks which template you are standing in and there is no
// answer out in the world -- or in somebody's front room.
//
// It writes the AUTHORING house's exit, exactly as it always did; what changed
// is only who may call it. `template save` is what copies that into the frozen
// snapshot, so placing an exit and not saving leaves claims on the old one.
bool ComfyCommands::HandleHouseTemplateExitCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    std::string error;

    if (handler->ExtractLiteralArg(&args, "where"))
    {
        House* house = sHouseMgr.GetAuthoredTemplateAt(player, error);
        if (!house)
        {
            handler->SendSysMessage(error.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        float x, y, z, o;
        sHouseMgr.GetExitPosition(*house, x, y, z, o);
        handler->PSendSysMessage("Way out: %.1f %.1f %.1f%s.", x, y, z,
                        house->exitSet ? "" : " (where it started)");
        return true;
    }

    if (handler->ExtractLiteralArg(&args, "reset"))
    {
        if (!sHouseMgr.ClearExitPortal(player, error))
        {
            handler->SendSysMessage(error.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
        handler->SendSysMessage("Way out moved back to where it started. .house template save publishes it.");
        return true;
    }

    if (!sHouseMgr.SetExitPortal(player, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->SendSysMessage("Way out placed three yards ahead, and residents will arrive facing away from it.");
    handler->SendSysMessage(".house template save publishes it to anyone who claims this template.");
    return true;
}

bool ComfyCommands::HandleHouseTemplateSaveCommand(ChatHandler* handler, char* /*args*/)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    uint32 saved = 0;
    bool savedExit = false;
    std::string error;
    if (!sHouseMgr.SaveTemplate(player, saved, savedExit, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    // It used to say "and the exit" whichever way this went, which is a lie on
    // the template that most needs telling: one nobody has placed a way out in
    // yet. Claims still work without one -- GetExitPosition falls back -- they
    // just all land on the same compiled-in spot, wherever the front door is.
    handler->PSendSysMessage("Template saved: %u objects%s. This is what a claim gets now.",
                    saved, savedExit ? " and the exit" : "");
    if (!savedExit)
        handler->SendSysMessage("No way out placed. Residents will arrive at the default spot. .house template exit sets it.");
    return true;
}

bool ComfyCommands::HandleHouseTemplateGotoCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    char* name = handler->ExtractLiteralArg(&args);
    if (!name)
    {
        handler->SendSysMessage("Usage: .house template goto <name>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    HouseTemplate const* tpl = sHouseMgr.GetTemplateByName(name);
    if (!tpl)
    {
        handler->PSendSysMessage("No template called \"%s\". .house template list shows them.", name);
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string error;
    if (!sHouseMgr.GotoTemplate(player, tpl->id, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    return true;
}

bool ComfyCommands::HandleHouseTemplateDeleteCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session ? handler->m_session->GetPlayer() : nullptr;

    char* name = handler->ExtractLiteralArg(&args);
    if (!name)
    {
        handler->SendSysMessage("Usage: .house template delete <name>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    HouseTemplate const* tpl = sHouseMgr.GetTemplateByName(name);
    if (!tpl)
    {
        handler->PSendSysMessage("No template called \"%s\".", name);
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string error;
    if (!sHouseMgr.DeleteTemplate(player, tpl->id, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->PSendSysMessage("Template \"%s\" deleted. Houses stamped from it keep everything.", name);
    return true;
}

bool ComfyCommands::HandleHouseTemplateListCommand(ChatHandler* handler, char* /*args*/)
{
    std::vector<HouseTemplate const*> templates = sHouseMgr.GetTemplates();
    if (templates.empty())
    {
        handler->SendSysMessage("No templates. .house template create <name> starts one.");
        return true;
    }

    std::vector<HousePortal const*> portals = sHouseMgr.GetPortals();
    for (HouseTemplate const* t : templates)
    {
        uint32 doors = 0;
        for (HousePortal const* p : portals)
            if (p->templateId == t->id)
                ++doors;

        handler->PSendSysMessage("%u: %s, %u objects, %s, %u door(s)%s",
                        t->id, t->name.c_str(), uint32(t->objects.size()),
                        t->exitSet ? "exit set" : "NO EXIT", doors,
                        t->savedAt ? "" : "  (never saved, stamps nothing yet)");
    }
    return true;
}

// .house object scale <factor> [id] -- resize one piece of furniture.
//
// A bare number multiplies what is there now, so "scale 1.2" four times keeps
// growing. "scale = 2" sets it against the template's own size instead, and
// "scale reset" is "= 1" -- without an absolute form there is no way back to
// normal except guessing the inverse of everything you did.
bool ComfyCommands::HandleHouseObjectScaleCommand(ChatHandler* handler, char* args)
{
    bool relative = true;
    float factor = 0.0f;

    if (handler->ExtractLiteralArg(&args, "reset"))
    {
        relative = false;
        factor = 1.0f;
    }
    else
    {
        if (handler->ExtractLiteralArg(&args, "="))
            relative = false;
        if (!handler->ExtractFloat(&args, factor) || factor <= 0.0f)
        {
            handler->SendSysMessage("Usage: .house object scale <factor> [id]     multiplies the current size");
            handler->SendSysMessage("   or: .house object scale = <factor> [id]   sets it against the normal size");
            handler->SendSysMessage("   or: .house object scale reset [id]");
            handler->SetSentErrorMessage(true);
            return false;
        }
    }

    House* house = nullptr;
    uint32 guid = HouseTarget(handler, args, house);
    if (!guid)
        return false;

    std::string const label = ObjectLabel(guid);

    std::string error;
    if (!sHouseMgr.ScaleObject(handler->m_session->GetPlayer(), guid, factor, relative, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    HouseObject const* o = sHouseMgr.GetObject(guid);
    uint32 const size = sHouseMgr.GroupSize(guid);
    if (size > 1)
        handler->PSendSysMessage("%s is now %g times its normal size. %u more resized with it.",
                                 label.c_str(), o ? o->scale : factor, size - 1);
    else
        handler->PSendSysMessage("%s is now %g times its normal size.", label.c_str(), o ? o->scale : factor);
    return true;
}

// .house visit <name> -- go to that character owner's house, if they lead your
// party. Naming a character of your own is just .house go.
bool ComfyCommands::HandleHouseVisitCommand(ChatHandler* handler, char* args)
{
    char* name = handler->ExtractQuotedOrLiteralArg(&args);
    if (!name)
    {
        handler->SendSysMessage("Usage: .house visit <character name>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // An empty error with no trip is a KNOCK on a locked door, which printed
    // its own line; that is the command working, not failing.
    std::string error;
    if (!sHouseMgr.VisitHouse(handler->m_session->GetPlayer(), name, error) && !error.empty())
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    return true;
}

// .house admit|refuse <name> -- the owner's answer to a knock on a locked door.
// The ComfyUI popup types these; they work by hand for anybody without it.
bool ComfyCommands::HouseAnswerKnock(ChatHandler* handler, char* args, bool admit)
{
    char* name = handler->ExtractQuotedOrLiteralArg(&args);
    if (!name)
    {
        handler->SendSysMessage(admit ? "Usage: .house admit <character name>"
                                      : "Usage: .house refuse <character name>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string error;
    if (!sHouseMgr.AnswerKnock(handler->m_session->GetPlayer(), name, admit, error))
    {
        if (!error.empty())
            handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    return true;
}

bool ComfyCommands::HandleHouseAdmitCommand(ChatHandler* handler, char* args)
{
    return HouseAnswerKnock(handler, args, true);
}

bool ComfyCommands::HandleHouseRefuseCommand(ChatHandler* handler, char* args)
{
    return HouseAnswerKnock(handler, args, false);
}

// .house settings [<name> [on|off]] -- the handful of choices that are the
// player's rather than the server's. See the HouseSetting enum for what each
// one does and why it is a setting at all.
//
// BARE TOGGLES, the way `.house edit` does. Both of these are booleans, and a
// player who types the name of one has already decided they want the other
// state -- making them spell it out is a step that answers nothing. The
// explicit form is still accepted, because a macro wants to SET rather than
// flip.
//
// Reachable with no house and from anywhere, unlike everything else in this
// file. A preference is not an action on a room: refusing to let somebody set
// one up before they have claimed a house would be a rule with nothing behind
// it.
bool ComfyCommands::HandleHouseSettingCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();
    if (!player)
        return false;

    char* nameArg = handler->ExtractLiteralArg(&args);

    // NO NAME: THE LIST, AND THE LIST IS THE INSTRUCTIONS. This is the only
    // place a player finds out these exist, so it has to leave them able to
    // type the next thing without guessing: the exact word, the state it is in,
    // what turning it off does, and the other way to get here. Four lines for
    // two settings is not verbose, it is the difference between a list and a
    // list you can act on.
    if (!nameArg)
    {
        handler->SendSysMessage("Housing settings. Use .house settings <name> to turn one on or off:");
        for (uint32 i = 0; i < HOUSE_SETTING_MAX; ++i)
        {
            HouseSetting const s = HouseSetting(i);

            // Ids are permanent, so a retired setting leaves a hole rather than
            // being deleted -- see HouseSetting. Every loop over the enum skips
            // them.
            if (!HouseMgr::IsSettingLive(s))
                continue;

            handler->PSendSysMessage("  %s: %s%s", HouseMgr::SettingName(s),
                            HouseMgr::SettingLabel(s, sHouseMgr.GetSetting(player, s)).c_str(),
                            HouseMgr::SettingIsGlobal(s) ? "   (server-wide)" : "");

            std::string const hint = HouseMgr::SettingHint(s);
            if (!hint.empty())
                handler->PSendSysMessage("      %s", hint.c_str());
        }
        handler->SendSysMessage("Or click Settings on the panel beside the way out.");
        return true;
    }

    std::string word = nameArg;
    std::transform(word.begin(), word.end(), word.begin(), ::tolower);

    HouseSetting setting;
    if (!HouseMgr::SettingFromWord(word, setting))
    {
        // NAME THEM RATHER THAN PRINTING A SHAPE. "Usage: .house setting
        // <name>" tells somebody who mistyped nothing they did not know; the
        // list of what exists is the only answer that helps.
        // The separator counts LIVE settings rather than indices: retired slots
        // leave holes in the enum, and `i ? ", " : ""` would open the list with
        // a stray comma the moment slot 0 became one of them.
        std::ostringstream names;
        bool first = true;
        for (uint32 i = 0; i < HOUSE_SETTING_MAX; ++i)
        {
            HouseSetting const s = HouseSetting(i);
            if (!HouseMgr::IsSettingLive(s))
                continue;

            names << (first ? "" : ", ") << HouseMgr::SettingName(s);
            first = false;
        }

        handler->PSendSysMessage("No such setting. There is: %s.", names.str().c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    // A GLOBAL SETTING IS EVERYBODY'S, so it is not one player's to flip. The
    // refusal names the scope rather than the rank: "you cannot" answers a
    // question nobody asked, where "this one is the server's" explains why the
    // row is on the page at all.
    if (HouseMgr::SettingIsGlobal(setting) && handler->GetAccessLevel() < SEC_DEVELOPER)
    {
        handler->PSendSysMessage("%s is server-wide, and set by whoever runs it.",
                        HouseMgr::SettingTitle(setting));
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 value = sHouseMgr.GetSetting(player, setting) ? 0 : 1;
    if (char* valueArg = handler->ExtractLiteralArg(&args))
    {
        std::string v = valueArg;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);

        if (!HouseMgr::SettingValueFromWord(setting, v, value))
        {
            // The hint comes with it: somebody who typed a word that is not
            // on or off does not need to be told the shape again, they need to
            // know what the two states are.
            handler->PSendSysMessage("%s is on or off. %s", HouseMgr::SettingName(setting),
                            HouseMgr::SettingHint(setting).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
    }

    sHouseMgr.SetSetting(player, setting, value);

    // The TITLE here, not the name you typed -- you have just typed it, and
    // "Point-and-click placing: off" reads back as a sentence where the
    // underscored form reads as an echo. One line, with the note folded in:
    // SettingNote is empty for every state that speaks for itself.
    std::string line = std::string(HouseMgr::SettingTitle(setting)) + ": " +
                       HouseMgr::SettingLabel(setting, value) + ".";
    if (char const* note = HouseMgr::SettingNote(setting, value))
        if (*note)
            line += std::string(" ") + note;

    handler->SendSysMessage(line.c_str());

    return true;
}

// .house edit [on|off] -- make every piece of furniture you own clickable.
// Bare `.house edit` toggles, which is what anybody actually types.
bool ComfyCommands::HandleHouseEditCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    // IsEditingAll, NOT IsEditing. Placing something starts a per-object
    // session so the new piece can be clicked straight away, and reading that
    // here would make the next bare `.house edit` mean "off" -- when what the
    // player wants, having just put a chair down, is the whole room lit up.
    // This command owns the global mode and nothing else.
    bool on = !sHouseMgr.IsEditingAll(player);
    if (char* word = handler->ExtractLiteralArg(&args))
    {
        if (!strcmp(word, "on"))
            on = true;
        else if (!strcmp(word, "off"))
            on = false;
        else
        {
            handler->SendSysMessage("Usage: .house edit [on|off]");
            handler->SetSentErrorMessage(true);
            return false;
        }
    }

    std::string error;
    if (!sHouseMgr.SetEditMode(player, on, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (on)
        handler->SendSysMessage("Edit mode on. Right-click anything of yours to move, turn or pick it up.");
    else
        handler->SendSysMessage("Edit mode off.");
    return true;
}

// THE ADDON'S COPY OF THE LIST, over the channel Turtle's own threat meter
// already uses. `Player::SendAddonMessage` builds a CHAT_MSG_GUILD packet in
// LANG_ADDON with "prefix\tbody", which the client demultiplexes into a
// CHAT_MSG_ADDON event -- so this reaches the addon and never touches the chat
// frame.
//
// THAT IS WHY THIS IS NOT SCREEN-SCRAPING. The obvious way to give the addon a
// list was to have it read the lines the command already prints, and it would
// have worked -- at the price of suppressing them again, which in 1.12 means
// hooking ChatFrame_OnEvent globally, because ChatFrame_AddMessageEventFilter
// is 2.x and is not in this client (0 hits in WoW.exe). A real channel that was
// already proven in this build beat a hook that breaks other addons.
//
// Three message kinds, so the addon can rebuild atomically rather than patching:
//
//   H <selectedSlot> <editMode> <placed> <cap>       begin -- clears its buffer
//   O <slot>:<yards>:<flags>:<entry>:<name>;...      rows, chunked
//   E <count>                                        commit
//
// The ENTRY rides along so the panel can put the object's picture beside its
// name: the addon ships a thumbnail per display id and a gameobject entry is
// what indexes them. The server never needs it -- it is carried purely because
// the addon cannot derive it from anything else it is given.
//
// Chunked because a chat packet is not the place to discover a length limit,
// and a house may hold 100 player objects on top of its fabric.
namespace
{
    uint32 const HOUSE_SYNC_CHUNK = 180;    // characters of body, not a hard limit

    // The separators must not appear inside a name. Sanitising here rather than
    // escaping keeps the addon's parser to one string.find with four captures,
    // which matters in Lua 5.0 where there is no gmatch to lean on.
    std::string SyncSafe(std::string v)
    {
        for (char& c : v)
            if (c == ';' || c == ':' || c == '\t' || c == '\n' || c == '\r')
                c = ' ';
        return v;
    }
}

static bool IsBuilding(uint32 goEntry)
{
    GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(goEntry);
    return info && info->type == GAMEOBJECT_TYPE_MAP_OBJECT;
}

static void SendHouseObjectSync(Player* player, House* house)
{
    // THE SLOT, NOT THE GUID. GetSelectedObject answers in guids, because that
    // is what every internal caller wants -- and everything the addon sees is
    // slots. Sending the raw answer put a seven-digit number where a 1-or-2
    // digit one was expected, so no row ever matched it and the panel's
    // selection highlight was wiped by the very sync that was meant to confirm
    // it. The optimistic highlight flashed and vanished, which reads as the
    // highlight being broken rather than as the wrong number arriving.
    uint32 selectedSlot = 0;
    if (house)
        if (uint32 const sel = sHouseMgr.GetSelectedObject(player))
            if (HouseObject const* o = sHouseMgr.GetObject(sel))
                selectedSlot = o->slot;

    std::ostringstream head;
    head << "H " << selectedSlot
         << " " << (sHouseMgr.IsEditingAll(player) ? 1 : 0)
         << " " << (house ? sHouseMgr.CountPlayerObjects(*house) : 0)
         << " " << uint32(HOUSE_MAX_OBJECTS)
         // Whether .obj answers this player, so the panel offers its world mode
         // only to a GM rather than printing "no such command" to everybody.
         << " " << (player->GetSession()->GetSecurity() >= SEC_DEVELOPER ? 1 : 0);
    player->SendAddonMessage("CHOUSE", head.str());

    uint32 sent = 0;
    if (house)
    {
        std::string chunk;
        std::vector<HouseObject const*> objects = sHouseMgr.GetObjects(house->id);

        for (HouseObject const* o : objects)
        {
            std::ostringstream rec;
            rec << o->slot << ':'
                << uint32(player->GetDistance(o->x, o->y, o->z) + 0.5f) << ':'
                // 1 came with the house, 2 a building, which the client will
                // not draw at another size; the panel greys its size buttons.
                << ((IsHouseFabric(*o) ? 1u : 0u) | (IsBuilding(o->goEntry) ? 2u : 0u)) << ':'
                << o->goEntry << ':'
                << SyncSafe(HouseMgr::PieceName(*o));

            if (!chunk.empty() && chunk.size() + rec.str().size() > HOUSE_SYNC_CHUNK)
            {
                player->SendAddonMessage("CHOUSE", "O " + chunk);
                chunk.clear();
            }
            if (!chunk.empty())
                chunk += ';';
            chunk += rec.str();
            ++sent;
        }

        if (!chunk.empty())
            player->SendAddonMessage("CHOUSE", "O " + chunk);
    }

    std::ostringstream tail;
    tail << "E " << sent;
    player->SendAddonMessage("CHOUSE", tail.str());
}

// The room, printed. Lifted out of the list command so the control panel on the
// exit gear can show the same thing -- one implementation, so a menu and a
// command can never describe the room two ways.
void HousePrintObjectList(Player* player)
{
    House* house = sHouseMgr.GetHouseAt(player);
    if (!house)
        return;

    ChatHandler handler(player);

    std::vector<HouseObject const*> objects = sHouseMgr.GetObjects(house->id);

    // A SENTENCE, NOT A RECORD. This header read "House 4: 7 of 100 placed."
    // -- three numbers, one of which (the house id) is a database key no
    // resident can use for anything, leading a list they asked for by clicking
    // "List Furniture in Room". It is the first line of the most-read output
    // housing produces, so it says what the list IS.
    //
    // The cap stays, because it is the one number that changes what you can do
    // next. Template furniture is still counted separately: it does not eat the
    // budget, and a stamped house whose totals silently disagreed with the row
    // count would look like a bug in the list.
    uint32 const placed = sHouseMgr.CountPlayerObjects(*house);
    uint32 const fabric = uint32(objects.size()) - placed;

    // The empty case is its own line and then stops. Printing a header over
    // nothing invites you to scroll looking for the rows.
    if (objects.empty())
    {
        handler.SendSysMessage("Your home has no furniture yet. Right-click a crate, or /house to browse.");
        return;
    }

    if (fabric)
        handler.PSendSysMessage("Your home has this furniture (%u of %u, plus %u that came with the house):",
                        placed, uint32(HOUSE_MAX_OBJECTS), fabric);
    else
        handler.PSendSysMessage("Your home has this furniture (%u of %u):",
                        placed, uint32(HOUSE_MAX_OBJECTS));

    // The house id and the instance id are debugging handles and nothing a
    // resident can use, so neither rides along on the line everyone reads. This
    // is the only place in game they are visible, though, so a developer still
    // gets both -- the house id moved down here when the header became a
    // sentence.
    if (player->GetSession()->GetSecurity() >= SEC_DEVELOPER)
        handler.PSendSysMessage("  house %u, instance %u", house->id, house->instanceId);

    // Which one the bare commands will act on. Worth a marker rather than a
    // separate command to ask: a selection you cannot see is a selection that
    // will eventually move the wrong thing.
    uint32 const selected = sHouseMgr.GetSelectedObject(player);

    for (HouseObject const* o : objects)
    {
        // NUMBER | NAME | DISTANCE, separated rather than aligned. Padding the
        // columns is the reflex and it cannot work here: the client's chat font
        // is proportional, so spaces line nothing up and only make the line
        // longer. A separator does the job a monospace column would.
        //
        // The SLOT, not the guid. This list is where a number comes from, so it
        // is the one place the two had to stop disagreeing.
        std::ostringstream line;
        line << "  " << o->slot << " | " << HouseMgr::PieceName(*o)
             << " | " << uint32(player->GetDistance(o->x, o->y, o->z) + 0.5f) << " yd";

        // Everything else is a suffix, and only when it is true. Three columns
        // that are sometimes four is not three columns.
        if (o->scale > 0.0f)
            line << "  x" << o->scale;
        if (o->source == HOUSE_SOURCE_TEMPLATE)
            line << "  (came with the house)";
        if (sHouseMgr.GroupSize(o->guid) > 1)
            line << "  group " << o->groupNo;
        if (o->guid == selected)
            line << "  <- selected";

        handler.SendSysMessage(line.str().c_str());
    }
}

bool ComfyCommands::HandleHouseObjectListCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();

    // `.house object list addon` is the panel's refresh: the same list, over the
    // addon channel, printing NOTHING. Deliberately an argument rather than its
    // own subcommand -- it is the same question, and `.commands` should not grow
    // an entry no human would ever type.
    bool const forAddon = handler->ExtractLiteralArg(&args, "addon") != nullptr;

    House* house = sHouseMgr.GetHouseAt(player);
    if (!house)
    {
        // The panel is told so rather than left holding the last house's
        // furniture: an empty sync clears it. Silently, because walking out of
        // a house is not an error the way typing the command outside one is.
        if (forAddon)
        {
            SendHouseObjectSync(player, nullptr);
            return true;
        }
        handler->SendSysMessage("You are not in a house.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (forAddon)
    {
        SendHouseObjectSync(player, house);
        return true;
    }

    HousePrintObjectList(player);
    return true;
}

// .house furniture reload -- re-read tw_world.house_furniture_item.
//
// Furniture items are content: adding one is a row here and a row in
// item_template. Without this, authoring a piece would cost a ~50s restart per
// attempt, which is exactly the trade the core's 103 `.reload` subcommands
// exist to avoid -- and there is no stock reload for a table this repo
// invented.
//
// It does NOT reload item_template; that has its own `reload item_template`,
// and the two are independent. A mapping row whose item does not exist yet is
// skipped with an error rather than accepted, so the order to run them in is
// item_template first.
bool ComfyCommands::HandleHouseFurnitureReloadCommand(ChatHandler* handler, char* /*args*/)
{
    sHouseMgr.LoadFurnitureItems();
    handler->PSendSysMessage("Furniture items reloaded: %u.", sHouseMgr.FurnitureItemCount());
    return true;
}

//== furniture storage =======================================================

// Forty squares, drawn as forty lines would be unreadable -- so only the full
// ones are listed, each with the number of the square it is in. That number is
// what every command takes and what the addon's grid position is, so the two
// can never mean different things.
void HousePrintStorage(Player* player)
{
    ChatHandler handler(player);

    std::vector<HouseStorageStack> shelf = sHouseMgr.GetStorageStacks(player);

    if (shelf.empty())
    {
        handler.SendSysMessage("Your furniture storage is empty.");
        return;
    }

    // NO "of 40" ANY MORE. The shelf has no size to be a fraction of, and a
    // total the player can neither reach nor plan around is a number that only
    // ever reads as a warning.
    uint32 const used = sHouseMgr.GetStorageCount(player);
    handler.PSendSysMessage("Furniture storage: %u crate%s.",
                            used, used == 1 ? "" : "s");

    // ONE LINE PER KIND, not per square, so this reads the way the addon's
    // window does. Twelve crates were twelve lines, most of them the same words
    // twice over -- and a chat frame is narrow. The grouping is
    // GetStorageStacks' job now; this used to do its own double loop.
    //
    // THE NUMBER IS STILL A SQUARE: the lowest one holding that kind, so the
    // line stays something you can type at `.house storage take`. Grouping
    // without carrying a real square would have made this list prettier and
    // unusable, since the server's verb takes a square and nothing else.
    //
    // No categories here, deliberately. The window groups by type because the
    // addon has the generated table to do it with; teaching the core the same
    // buckets would be a third copy of a rule that already lives in
    // tools/categorise.js and would drift the first time one was renamed.
    for (auto const& s : shelf)
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(s.itemEntry);
        std::ostringstream line;
        line << "  " << s.slot << " | " << (proto ? proto->Name1.c_str() : "?");
        if (s.count > 1)
            line << "  x" << s.count;
        handler.SendSysMessage(line.str().c_str());
    }
    handler.SendSysMessage("  .house storage take <number> to fetch one back.");
}

// What the panel's "How this works" prints. Deliberately the short list rather
// than the whole command set -- docs/housing.txt is the whole command set, and a
// wall of chat is not a help page.
void HousePrintHouseHelp(Player* player)
{
    ChatHandler handler(player);
    handler.SendSysMessage("Your house:");
    handler.SendSysMessage("  Furniture is an item. Right-click a crate here to put it down.");
    handler.SendSysMessage("  .house object del      picks the nearest one back up");
    handler.SendSysMessage("  .house object list     numbers everything in the room");
    handler.SendSysMessage("  .house edit            makes everything you placed clickable");
    handler.SendSysMessage("  .house storage         a shelf for the crates you are not using");
    handler.SendSysMessage("  .house settings        how placing and the selection glow behave");
    handler.SendSysMessage("  .house visit <name>    their house, while they lead your party");
    handler.SendSysMessage("  .house admit <name>    let in a knock, while your door is locked");
    handler.SendSysMessage("  /house opens the addon, which is buttons for all of it.");
}

// The addon's copy of the shelf. Same three-message grammar as the object sync
// -- begin, chunked rows, commit -- but on its OWN PREFIX rather than new
// letters on the old one: the addon's parser splits on a single-character kind,
// and one grammar per prefix is what keeps it that way.
//
// ONE RECORD PER KIND, not per crate, which is what keeps this bounded by the
// dozen furniture items that exist rather than by how many the player owns.
// It was one record per occupied square, which was fine against a forty-square
// shelf and is not fine now that the shelf has no size: a thousand crates would
// have been a thousand records across dozens of addon packets, to describe a
// list of twelve rows.
//
// The head carries the TOTAL crates, so the window's counter needs no sum of
// its own; it used to carry the shelf size, which no longer exists.
static void SendStorageSync(Player* player)
{
    std::vector<HouseStorageStack> shelf = sHouseMgr.GetStorageStacks(player);

    std::ostringstream head;
    head << "H " << sHouseMgr.GetStorageCount(player);
    player->SendAddonMessage("CSTORE", head.str());

    std::string chunk;
    uint32 sent = 0;
    for (auto const& s : shelf)
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(s.itemEntry);

        std::ostringstream rec;
        rec << s.slot << ':' << s.itemEntry << ':' << s.count << ':'
            << SyncSafe(proto ? proto->Name1 : std::string("?"));

        if (!chunk.empty() && chunk.size() + rec.str().size() > HOUSE_SYNC_CHUNK)
        {
            player->SendAddonMessage("CSTORE", "O " + chunk);
            chunk.clear();
        }
        if (!chunk.empty())
            chunk += ';';
        chunk += rec.str();
        ++sent;
    }
    if (!chunk.empty())
        player->SendAddonMessage("CSTORE", "O " + chunk);

    std::ostringstream tail;
    tail << "E " << sent;
    player->SendAddonMessage("CSTORE", tail.str());
}

// EVERY CHANGE PUSHES A FRESH SHELF. The window cannot see the server change
// anything on its own -- it only ever asked once, when it opened -- so without
// this a crate moved by any route leaves the grid describing the shelf as it
// was. Cheap enough to send unconditionally: it is at most three chat packets,
// and a client with no addon listening drops them silently.
//
// Pushed at the addon rather than pulled by it, because the click that asks for
// the window happens on the SERVER -- somebody right-clicked the gear. With no
// addon listening this does nothing at all, which is why the gossip option
// prints the chat list beside it.
void HouseOpenStorageWindow(Player* player)
{
    SendStorageSync(player);
    player->SendAddonMessage("CHOUSE", "U storage");
}

// .house storage [addon | put [all | <entry> [square]] | take <square>]
//
// One command with sub-words rather than four leaves, because they are four
// views of one shelf and the bare form is the one people will type. Every
// branch refuses outside your own house -- storage is a cupboard in the house,
// and CheckOwner in HouseMgr says so in the same words the rest of housing does.
bool ComfyCommands::HandleHouseStorageCommand(ChatHandler* handler, char* args)
{
    Player* player = handler->m_session->GetPlayer();
    std::string error;

    if (handler->ExtractLiteralArg(&args, "addon"))
    {
        SendStorageSync(player);
        return true;
    }

    if (handler->ExtractLiteralArg(&args, "put"))
    {
        // `put <entry> [square]` is the ADDON's form -- it knows exactly which
        // crate was dropped and which square it landed on. Tried after the
        // literal so `put all` is never read as a number.
        uint32 entry = 0;
        if (!handler->ExtractLiteralArg(&args, "all") && handler->ExtractUInt32(&args, entry) && entry)
        {
            uint32 slot = 0;
            handler->ExtractOptUInt32(&args, slot, 0);       // 0 = the first free square

            if (!sHouseMgr.StorageDeposit(player, entry, slot, error))
            {
                handler->SendSysMessage(error.c_str());
                handler->SetSentErrorMessage(true);
                return false;
            }
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
            handler->PSendSysMessage("Put the %s away.", proto ? proto->Name1.c_str() : "furniture");
            SendStorageSync(player);
            return true;
        }

        // Bare `put` is `put all`. Sweeping the bags is what this is for, and a
        // form that needs an argument to do the obvious thing is a form people
        // stop typing.
        uint32 moved = 0;
        if (!sHouseMgr.StorageDepositAll(player, moved, error))
        {
            handler->SendSysMessage(error.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
        handler->PSendSysMessage("Put %u away.", moved);
        HousePrintStorage(player);
        SendStorageSync(player);
        return true;
    }

    // `move <from> <to>` is the addon's, and only the addon's: dragging a crate
    // from one square to another is not something anyone will type.
    if (handler->ExtractLiteralArg(&args, "move"))
    {
        uint32 from = 0, to = 0;
        if (!handler->ExtractUInt32(&args, from) || !handler->ExtractUInt32(&args, to))
        {
            handler->SendSysMessage("Usage: .house storage move <from> <to>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (!sHouseMgr.StorageMove(player, from, to, error))
        {
            handler->SendSysMessage(error.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
        SendStorageSync(player);
        return true;
    }

    if (handler->ExtractLiteralArg(&args, "take"))
    {
        uint32 slot = 0;
        if (!handler->ExtractUInt32(&args, slot) || !slot)
        {
            handler->SendSysMessage("Which one? Use .house storage take <number>. The numbers are in .house storage.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // THE NUMBER IS THE SQUARE, not a position in a list. That is the whole
        // point of slots: a crate keeps its number until somebody moves it, so
        // a number read a minute ago still names the same thing.
        //
        // Read BEFORE the withdraw, because afterwards there is nothing left to
        // ask what it was.
        uint32 const entry = sHouseMgr.GetStorageAt(player, slot);

        if (!sHouseMgr.StorageWithdraw(player, slot, error))
        {
            handler->SendSysMessage(error.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
        handler->PSendSysMessage("Took the %s.", proto ? proto->Name1.c_str() : "furniture");
        SendStorageSync(player);
        return true;
    }

    // Bare form: just show it. Listing has to ask the gate explicitly --
    // GetStorage answers with an empty shelf whether you are at home or standing
    // in Ironforge, so without this "you have nothing stored" would be the reply
    // to a command that was refused.
    //
    // And it only ever LISTS. A bare form that swept the bags would be an action
    // nobody asked for, on the one word people will type to have a look.
    if (!sHouseMgr.StorageAllowed(player, error))
    {
        handler->SendSysMessage(error.c_str());
        handler->SetSentErrorMessage(true);
        return false;
    }
    HousePrintStorage(player);
    return true;
}
