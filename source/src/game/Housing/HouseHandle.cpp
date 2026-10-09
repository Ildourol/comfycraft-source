/*
 * Edit mode -- click-to-decorate.
 *
 * `.house move 0 0 1` works, but typing offsets at a chair is not how anybody
 * wants to arrange a room. In edit mode the furniture itself highlights under
 * the cursor and right-clicking it opens a menu that nudges, spins, resizes and
 * picks up exactly that object.
 *
 * THE FURNITURE IS THE HANDLE, and it is one field swapped per viewer. While an
 * object is in your edit session Object::BuildValuesUpdate tells YOUR client --
 * and nobody else's -- that GAMEOBJECT_TYPE_ID is GOOBER, which is a type
 * GameObject::Use raises gossip for. The server still reads CHAIR everywhere,
 * so Update(), Use()'s switch and every GetGOInfo() union read stay correct, a
 * guest sees ordinary furniture, and nothing has to be put back on the way out.
 *
 * Until 2026-09-03 this summoned a small hovering gear beside every object and
 * you clicked that instead, because a chair could not be clicked at all. Three
 * findings from that design are worth keeping, since they still bound what is
 * possible here:
 *
 *  - FURNITURE CAN NEVER BE THE NPC ITSELF. CreatureModelData.dbc is 788
 *    models and every one is a .mdx; there is not a single .wmo. A chair can
 *    never wear its own model as a creature without a client patch -- which is
 *    why the marker was a separate object rather than the furniture wearing a
 *    creature's clothes.
 *
 *  - A HANDLE CANNOT BE INVISIBLE. The invisible stalker model is the reflex
 *    choice for a marker everywhere else in this DB and was exactly wrong here:
 *    the entire mechanism was clicking it. The same rule survives in the
 *    selection glow, which is a visible summon for a visible reason.
 *
 *  - THE MENU NEEDS NO DATABASE ROWS. GossipMenu::AddMenuItem and
 *    PlayerMenu::SendGossipMenu build a menu in code, so none of the gossip
 *    limits that shape sql/custom/003 and 004 apply here: no gossip_menu entry
 *    out of the nearly-full smallint space, no gossip_menu_option rows, no
 *    npc_option_npcflag trap. The one unavoidable row is the greeting, because
 *    SendGossipMenu takes an npc_text id rather than a string -- which is why
 *    each PAGE having its own title costs three rows (sql/custom/049).
 *
 * THE SESSION IS THE STATE AND IT IS MEMORY-ONLY. `wanted` is what you may
 * click; `marked` is what the client has actually been told, which lags by one
 * Reconcile pass because telling it costs a despawn and a respawn of the
 * object. Nothing is persisted, so a crash or a restart mid-edit leaves nothing
 * behind.
 *
 * Ownership is not checked separately in the menu below. The session is keyed
 * by the editing player, so a click can only ever reach a menu on an object
 * that player was already being shown as clickable -- and HouseMgr's own
 * CheckOwner still guards every mutation underneath.
 */

#include "Housing/HouseMgr.h"

#include "Chat.h"
#include "GossipDef.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "GameObject.h"

namespace
{
    // Our own sender id, so a stray click carrying somebody else's sender falls
    // through to the default handling instead of being read as one of ours.
    uint32 const HOUSE_SENDER = 700;

    // ONE GREETING PER PAGE. SendGossipMenu takes an npc_text id rather than a
    // string, so a page title is a database row and nothing else -- which is
    // why all four pages shared one for so long, and why it said "Which way
    // shall it go?" on the resize page.
    //
    // 6400030 is sql/custom/018 and is retitled in place by sql/custom/049;
    // the other three are new there.
    uint32 const HOUSE_TEXT_MAIN = 6400030;             // What would you like to do?
    uint32 const HOUSE_TEXT_MOVE = 6400035;             // Move the object
    uint32 const HOUSE_TEXT_TURN = 6400036;             // Turn the object
    uint32 const HOUSE_TEXT_SIZE = 6400037;             // Resize the object

    enum HouseHandleAction
    {
        ACT_PAGE_MAIN   = 1,
        ACT_PAGE_MOVE   = 2,
        ACT_PAGE_TURN   = 3,
        ACT_PAGE_SIZE   = 4,

        // AWAY AND TOWARDS, not forward and backward, because that is what
        // `.house object move` calls the same two directions. The menu and the
        // command steer one object and should not need translating between.
        ACT_AWAY        = 10,
        ACT_TOWARDS     = 11,
        ACT_LEFT        = 12,
        ACT_RIGHT       = 13,
        ACT_UP          = 14,
        ACT_DOWN        = 15,

        // One per page, all three outside the redraw bands below because each
        // sends its own page back and returns.
        ACT_STEP_MOVE   = 16,
        ACT_STEP_TURN   = 17,
        ACT_STEP_SIZE   = 18,

        ACT_TURN_L      = 20,
        ACT_TURN_R      = 21,
        ACT_TURN_L90    = 22,
        ACT_TURN_R90    = 23,
        ACT_FACE_ME     = 24,
        ACT_FACE_AWAY   = 25,

        // Size steps sit above the turn block on purpose -- the redraw at the
        // bottom of GossipSelect picks its page from contiguous ranges.
        ACT_BIGGER       = 26,
        ACT_BIGGER_LOTS  = 27,
        ACT_SMALLER      = 28,
        ACT_SMALLER_LOTS = 29,
        ACT_SIZE_RESET   = 33,
        ACT_SIZE_SET     = 34,   // coded: the client prompts for a number

        ACT_DRAG        = 30,
        ACT_DELETE      = 31,

        // Deliberately OUTSIDE the contiguous move/turn/size ranges the redraw
        // at the bottom of GossipSelect reads: it returns to the main page
        // itself, so falling into one of those bands would send it to the
        // wrong one.
        ACT_SELECT      = 35,

        // Furniture groups. Both return to the main page themselves, like
        // ACT_SELECT, and for the same reason sit outside the redraw bands.
        ACT_GROUP_JOIN  = 36,
        ACT_GROUP_LEAVE = 37,

        ACT_CLOSE       = 40,

        // Drawn only while global edit is on, so it is never the answer to a
        // click that could have meant ACT_CLOSE instead.
        ACT_EDIT_OFF    = 41,
    };

    // The steps each page cycles through, one list per kind. Coarse enough to
    // shove a house across the lawn, fine enough to seat a chair at a table.
    //
    // 0.1 yards is the new one and it is the point of the list: a quarter of a
    // yard is still a visible gap between a chair and the table it belongs at.
    // 100% is a doubling, which is as fast as anybody wants to resize by eye,
    // and the two 90-degree buttons on the turn page mean its list never needs
    // a coarse entry.
    float const STEPS_MOVE[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f };
    float const STEPS_TURN[] = { 5.0f, 15.0f, 45.0f };
    float const STEPS_SIZE[] = { 5.0f, 10.0f, 50.0f, 100.0f };

    struct StepKindInfo
    {
        float const* values;
        uint32       count;
        char const*  unit;                              // reads after the number
        uint32       cycleAction;                       // the option that advances it
        uint32       pageAction;                        // the page it lives on
    };

    // ONE ROW PER KIND, CARRYING EVERYTHING ABOUT IT. The alternative is passing
    // the unit and the two actions alongside the kind at each call site, where
    // three arguments have to agree and nothing checks that they do -- a page
    // cycling its neighbour's step would compile and read correctly.
    //
    // Indexed by HouseStepKind, so the order here is not free to change.
    StepKindInfo const STEP_KINDS[] =
    {
        { STEPS_MOVE, sizeof(STEPS_MOVE) / sizeof(STEPS_MOVE[0]), " yards",   ACT_STEP_MOVE, ACT_PAGE_MOVE },
        { STEPS_TURN, sizeof(STEPS_TURN) / sizeof(STEPS_TURN[0]), " degrees", ACT_STEP_TURN, ACT_PAGE_TURN },
        { STEPS_SIZE, sizeof(STEPS_SIZE) / sizeof(STEPS_SIZE[0]), "%",        ACT_STEP_SIZE, ACT_PAGE_SIZE },
    };

    // Back sits at the TOP of a submenu, not the bottom. A gossip list grows
    // downwards and this one changes length -- the step option is wider or
    // narrower depending on the number in it -- so a Back underneath the
    // options is never twice in the same place. At the top it always is.
    //
    // The blank row below it is a real option carrying a single space: a gossip
    // menu has no rule and no inert row, so a spacer has to be something
    // clickable. It redraws the page it is on, which is the closest thing to
    // doing nothing that a menu option can do.
    void AddSpacer(GossipMenu& menu, uint32 selfAction)
    {
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, " ", HOUSE_SENDER, selfAction);
    }

    void AddBackRow(GossipMenu& menu, uint32 selfAction)
    {
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Back", HOUSE_SENDER, ACT_PAGE_MAIN);
        AddSpacer(menu, selfAction);
    }

    // THE STEP SITS DIRECTLY UNDER BACK, above everything it governs, because
    // it is a setting and the rows below it are actions. Read top to bottom it
    // now says "move by one yard: away, towards, left, right" -- where with the
    // step at the bottom you had to look past the buttons to find out what they
    // would do.
    void AddStepRow(GossipMenu& menu, Player* player, HouseStepKind kind)
    {
        StepKindInfo const& info = STEP_KINDS[kind];

        std::ostringstream step;
        step << "Step: " << sHouseMgr.GetEditStep(player, kind) << info.unit
             << " (click to change)";

        menu.AddMenuItem(HOUSE_GOSSIP_ICON, step.str().c_str(), HOUSE_SENDER, info.cycleAction);
        AddSpacer(menu, info.pageAction);
    }

    // `note` is what JUST HAPPENED, for the one case the standing state cannot
    // express: having just unselected, the selection is gone, so a greeting
    // derived from it can only fall silent -- and a click that answers with
    // nothing reads as a click that did nothing. Everywhere else it is null and
    // the line describes the state, which is the honest default.
    void SendMain(Player* player, ObjectGuid handle, HouseObject const* obj, char const* note = nullptr)
    {
        GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
        player->PlayerTalkClass->ClearMenus();

        bool const selected = sHouseMgr.GetSelectedObject(player) == obj->guid;

        // THREE GROUPS, SEPARATED. The three pages are what you came for, the
        // selection is a different question about the same object, and the last
        // two are the ways OUT -- one that keeps the object and one that does
        // not. Run together as six rows they read as six equal buttons, and
        // "Pick up" ends up one careless row above "Done".
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Move it", HOUSE_SENDER, ACT_PAGE_MOVE);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Turn it", HOUSE_SENDER, ACT_PAGE_TURN);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Resize it", HOUSE_SENDER, ACT_PAGE_SIZE);

        AddSpacer(menu, ACT_PAGE_MAIN);

        // THE BRIDGE FROM THE MENU TO THE COMMANDS. This menu already knows
        // exactly which object you mean -- you right-clicked it -- and this is
        // what hands that certainty to `.house object move` and to the addon's
        // nudge pad, both of which otherwise fall back to guessing at the
        // nearest thing within ten yards.
        //
        // Explicit rather than a side effect of opening the menu: with global
        // edit on everything is clickable, and merely looking at one object
        // must not silently retarget every command you type afterwards.
        if (selected)
            menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Selected (click to clear)", HOUSE_SENDER, ACT_SELECT);
        else
            menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Select this one", HOUSE_SENDER, ACT_SELECT);

        // FURNITURE GROUPS, in the selection's block because grouping is built
        // on it: select the table, then open each chair and group it with the
        // table. The selection stays on the table, so the next chair is one
        // click too. Each row is drawn only where it can do something.
        uint32 const selectedGuid = sHouseMgr.GetSelectedObject(player);
        uint32 const groupSize = sHouseMgr.GroupSize(obj->guid);
        HouseObject const* selectedObj = selectedGuid ? sHouseMgr.GetObject(selectedGuid) : nullptr;
        bool const sameGroup = groupSize > 1 && selectedObj && selectedObj->groupNo == obj->groupNo;

        if (selectedObj && !selected && !sameGroup)
        {
            // The name alone: the number is for typing at a command, and a
            // button is clicked.
            std::string const label = "Group with " + HouseMgr::PieceName(*selectedObj);
            menu.AddMenuItem(HOUSE_GOSSIP_ICON, label.c_str(), HOUSE_SENDER, ACT_GROUP_JOIN);
        }
        if (groupSize > 1)
            menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Ungroup", HOUSE_SENDER, ACT_GROUP_LEAVE);

        AddSpacer(menu, ACT_PAGE_MAIN);

        // "Pick up", not "Delete", because for anything that came out of a bag
        // that is literally what happens -- the item goes back to the player.
        // The menu said "Picked up the ..." in its answer long before it could
        // be true; the button now agrees with the answer.
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Pick up", HOUSE_SENDER, ACT_DELETE);

        AddSpacer(menu, ACT_PAGE_MAIN);

        // THE TWO EDIT MODES END DIFFERENTLY, so the bottom of the menu is not
        // the same menu. See HouseEdit in HouseMgr.h for what they are.
        //
        // GLOBAL: the mode owns the room, so there is nothing this menu can
        // finish with on its own -- "Done" closes the window, and the way out
        // of the mode is its own row beneath it. Two rows, one group, no
        // spacer, ordered narrow then wide so the bigger one is never where a
        // hand lands first.
        //
        // INDIVIDUAL: this object IS the session, so "Done editing" is the way
        // out and there is no wider mode to offer. One row.
        //
        // The wording follows from that. "Done editing" alone is fine and says
        // what it does; sitting directly above "Exit edit mode" it would read
        // as the same thing said twice, so the global form is the barer "Done"
        // -- which is also all it does.
        if (sHouseMgr.IsEditingAll(player))
        {
            menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Done", HOUSE_SENDER, ACT_CLOSE);
            menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Exit edit mode", HOUSE_SENDER, ACT_EDIT_OFF);
        }
        else
            menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Done editing", HOUSE_SENDER, ACT_CLOSE);

        // The greeting is a fixed npc_text -- SendGossipMenu takes an id, not a
        // string -- so the one thing the player actually needs to know, WHICH
        // object this menu belongs to, goes to chat instead.
        //
        // THE NAME AND THE NUMBER, through the shared ObjectLabel. This read
        // "Handle: 3." for as long as there were gears, on the argument that
        // you had just clicked the object so its name told you nothing. That
        // argument was sound while the menu was the ONLY thing this line came
        // from; it stopped being sound once every other route -- placing,
        // selecting, nudging, the list -- said "Wooden Chair (3)". One object
        // named two ways across two lines of the same chat frame is worse than
        // a word of redundancy, and the redundancy is what lets you scroll back
        // and match a line to a piece.
        //
        // The number is house_object.slot, which is what makes this short
        // enough to print at all: it was "Handle: 8000042." until objects were
        // numbered per house.
        //
        // Selecting says so HERE rather than printing a second line of its own:
        // this greeting is redrawn by the toggle anyway, so a separate
        // announcement was two lines describing one state.
        //
        // A grouped piece says how many pieces its buttons will move.
        std::ostringstream group;
        if (groupSize > 1)
            group << ", group of " << groupSize;
        ChatHandler(player).PSendSysMessage("Editing %s%s.%s",
                                            HouseMgr::ObjectLabel(obj->guid).c_str(),
                                            group.str().c_str(),
                                            note ? note : (selected ? " Selected." : ""));

        player->PlayerTalkClass->SendGossipMenu(HOUSE_TEXT_MAIN, handle);
    }

    void SendMovePage(Player* player, ObjectGuid handle)
    {
        GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
        player->PlayerTalkClass->ClearMenus();

        AddBackRow(menu, ACT_PAGE_MOVE);
        AddStepRow(menu, player, HOUSE_STEP_MOVE);

        // Directions are relative to the way the PLAYER is facing, not to the
        // world axes `.house move` uses. Pointing at a chair and saying "left"
        // is the whole reason to click a menu instead of typing coordinates.
        //
        // THE FLAT FOUR, THEN THE VERTICAL PAIR. Up and down are a different
        // gesture from shoving something around the floor, and they are the two
        // reached for far less often; grouped apart they stop being mistaken
        // for the next compass direction along.
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Away",    HOUSE_SENDER, ACT_AWAY);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Towards", HOUSE_SENDER, ACT_TOWARDS);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Left",    HOUSE_SENDER, ACT_LEFT);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Right",   HOUSE_SENDER, ACT_RIGHT);

        AddSpacer(menu, ACT_PAGE_MOVE);

        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Up",   HOUSE_SENDER, ACT_UP);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Down", HOUSE_SENDER, ACT_DOWN);

        AddSpacer(menu, ACT_PAGE_MOVE);

        // THIRTEEN ROWS, WHICH IS THE LONGEST OF THE FOUR PAGES. Two limits sit
        // above it and neither is close: SendGossipMenu asserts at
        // GOSSIP_MAX_MENU_ITEMS = 32 (GossipDef.cpp:44), and the client stops
        // parsing options somewhere past ~490 bytes of label plus 7 per row --
        // measured for the database-driven menus, and this page spends about
        // 175. The comment beside the packet's own count field says "max count
        // 15", which is neither of those and is not evidenced here; it is the
        // number to check first if a page ever truncates.
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Bring it to me", HOUSE_SENDER, ACT_DRAG);
        player->PlayerTalkClass->SendGossipMenu(HOUSE_TEXT_MOVE, handle);
    }

    void SendSizePage(Player* player, ObjectGuid handle, HouseObject const* obj)
    {
        GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
        player->PlayerTalkClass->ClearMenus();

        AddBackRow(menu, ACT_PAGE_SIZE);
        AddStepRow(menu, player, HOUSE_STEP_SIZE);

        // Percentages rather than multipliers, because "+10%" is what somebody
        // resizing a chair is thinking.
        //
        // GROW AND SHRINK ARE EXACT INVERSES, which they were not before: grow
        // multiplies by (1 + p), shrink DIVIDES by it, so the two cancel and a
        // misclick costs nothing. Multiplying by (1 - p) instead left +10% then
        // -10% at 0.99, and would have made the new 100% step multiply by zero.
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Grow",   HOUSE_SENDER, ACT_BIGGER);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Shrink", HOUSE_SENDER, ACT_SMALLER);

        AddSpacer(menu, ACT_PAGE_SIZE);

        // Kept as fixed jumps even though the step can reach 50: this is the
        // one wanted when the step is set fine for nudging and the object is
        // still half the size it should be.
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Grow 50%",   HOUSE_SENDER, ACT_BIGGER_LOTS);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Shrink 50%", HOUSE_SENDER, ACT_SMALLER_LOTS);

        AddSpacer(menu, ACT_PAGE_SIZE);

        // CODED. The last field of AddMenuItem makes the client pop a text box
        // and send what is typed back through pGOGossipSelectWithCode. This
        // build DOES transmit that flag -- SMSG_GOSSIP_MESSAGE writes index,
        // icon, coded and text per option (GossipDef.cpp:181-188) -- which is
        // notable, because the BoxMessage argument sitting right beside it is
        // NOT sent and there are no confirmation popups here at all.
        std::ostringstream setLabel;
        setLabel << "Set size";
        if (obj && obj->scale > 0.0f)
            setLabel << "  (now x" << obj->scale << ")";
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, setLabel.str().c_str(), HOUSE_SENDER, ACT_SIZE_SET,
                         "Size, where 1 is normal:", true);

        // The way back. Undoing a dozen multiplications by eye is not something
        // anybody should have to do. Directly under Set size, because the two
        // are the same question -- an exact size, or the exact size it began at.
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Reset size", HOUSE_SENDER, ACT_SIZE_RESET);

        player->PlayerTalkClass->SendGossipMenu(HOUSE_TEXT_SIZE, handle);
    }

    void SendTurnPage(Player* player, ObjectGuid handle)
    {
        GossipMenu& menu = player->PlayerTalkClass->GetGossipMenu();
        player->PlayerTalkClass->ClearMenus();

        AddBackRow(menu, ACT_PAGE_TURN);
        AddStepRow(menu, player, HOUSE_STEP_TURN);

        // The bare pair takes the step; the labelled pair is a quarter turn
        // whatever the step says. A square table wants 90 and a chair wants 5,
        // and having to change the step between them is the annoyance the
        // fixed buttons exist to avoid.
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Left",  HOUSE_SENDER, ACT_TURN_L);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Right", HOUSE_SENDER, ACT_TURN_R);

        AddSpacer(menu, ACT_PAGE_TURN);

        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Left 90",  HOUSE_SENDER, ACT_TURN_L90);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Right 90", HOUSE_SENDER, ACT_TURN_R90);

        AddSpacer(menu, ACT_PAGE_TURN);

        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Face Player",           HOUSE_SENDER, ACT_FACE_ME);
        menu.AddMenuItem(HOUSE_GOSSIP_ICON, "Face Away from Player", HOUSE_SENDER, ACT_FACE_AWAY);

        player->PlayerTalkClass->SendGossipMenu(HOUSE_TEXT_TURN, handle);
    }

}

//== edit sessions ===========================================================

HouseObject const* HouseMgr::GetObject(uint32 guid) const
{
    auto itr = m_objects.find(guid);
    return itr == m_objects.end() ? nullptr : &itr->second;
}

bool HouseMgr::IsEditing(Player* player) const
{
    return player && m_editing.find(player->GetGUIDLow()) != m_editing.end();
}

// "On for everything" as opposed to "on for the things just placed" -- the
// per-object toggle refuses in the first state, because the reconciler would
// re-add whatever it dropped on the next pass.
bool HouseMgr::IsEditingAll(Player* player) const
{
    auto itr = m_editing.find(player->GetGUIDLow());
    return itr != m_editing.end() && itr->second.all;
}

// THE WANTED SET, NOT THE MARKED ONE. `marked` lags by a Reconcile pass, and
// this is the question BuildValuesUpdate asks on every update block -- so
// answering from `marked` would make the type override wait for the very
// Reconcile that is trying to deliver it.
bool HouseMgr::InEditSession(Player* player, uint32 objectGuid) const
{
    auto itr = m_editing.find(player->GetGUIDLow());
    return itr != m_editing.end() && itr->second.wanted.count(objectGuid) > 0;
}

// Answering for a player with no session at all is not a hypothetical: the
// menu is drawn from GossipHello, and a per-object session can be dropped by
// the Done button while the page is still on screen. The fallbacks are the
// struct's own defaults, so the number shown is the number that would be used.
float HouseMgr::GetEditStep(Player* player, HouseStepKind kind) const
{
    auto itr = m_editing.find(player->GetGUIDLow());
    if (itr == m_editing.end())
        return kind == HOUSE_STEP_TURN ? 15.0f
             : kind == HOUSE_STEP_SIZE ? 10.0f : 1.0f;

    switch (kind)
    {
        case HOUSE_STEP_TURN: return itr->second.stepTurn;
        case HOUSE_STEP_SIZE: return itr->second.stepSize;
        default:              return itr->second.stepMove;
    }
}

// Falling off the end of the list goes back to the front, and a step that is
// not IN the list -- which nothing produces today, but a changed list would --
// lands on the first entry rather than sticking.
void HouseMgr::CycleEditStep(Player* player, HouseStepKind kind)
{
    auto itr = m_editing.find(player->GetGUIDLow());
    if (itr == m_editing.end())
        return;

    float* slot = kind == HOUSE_STEP_TURN ? &itr->second.stepTurn
                : kind == HOUSE_STEP_SIZE ? &itr->second.stepSize
                                          : &itr->second.stepMove;

    StepKindInfo const& info = STEP_KINDS[kind];
    for (uint32 i = 0; i < info.count; ++i)
        if (info.values[i] == *slot)
        {
            *slot = info.values[(i + 1) % info.count];
            return;
        }
    *slot = info.values[0];
}

// Bring what the CLIENT has been told into line with what the session wants.
// Everything that changes a session ends here, so there is exactly one place
// that decides what is clickable.
//
// It used to reconcile live GEARS instead, against the session AND the player's
// distance -- with two thresholds rather than one, because a single one makes a
// player standing on the boundary summon and despawn a gameobject twice a
// second. None of that survives: what is reconciled now is a field the client
// was told, and the client does its own interact-distance check.
void HouseMgr::Reconcile(Player* player, HouseEdit& session)
{
    // .house edit on means "all of them", so it has to keep meaning that as
    // the house changes -- otherwise the set is a snapshot of the moment it was
    // switched on, which is the bug this whole mechanism replaced.
    if (session.all)
    {
        session.wanted.clear();
        for (HouseObject const* o : GetObjects(session.houseId))
            if (!IsHouseFabric(*o))
                session.wanted.insert(o->guid);
    }

    // NOTHING SUMMONS A GEAR ANY MORE. The furniture itself is hoverable and
    // clickable while it is in the session -- Object::BuildValuesUpdate tells
    // this player, and only this player, that it is a GOOBER -- so a second
    // object floating beside it is clutter. The loop that used to summon them
    // lived here; what replaces it is telling the client which objects changed
    // side.
    //
    // NO DISTANCE CHECK, unlike the gears this replaced. A gear had to come and go with
    // the player because it was a real object in the room; a type is just what
    // the client was told, and the client does its own interact-distance check
    // before it will send a click. Marking on approach and unmarking on walking
    // away would be a despawn and respawn of every object every few steps.
    for (auto m = session.marked.begin(); m != session.marked.end(); )
    {
        if (session.wanted.count(*m))
        {
            ++m;
            continue;
        }
        FlashObject(player, *m);            // back to a chair, for this player
        session.marked.erase(m++);
    }

    for (uint32 objectGuid : session.wanted)
    {
        if (session.marked.count(objectGuid) || !GetObject(objectGuid))
            continue;

        session.marked.insert(objectGuid);

        // THE BLINK IS THE PRICE AND IT IS NOT AVOIDABLE. The 1.12 client builds
        // a gameobject from its creation block and never re-reads it, so a
        // changed type only lands by taking the object out and putting it back.
        // FlashObject is that, and the blink is what it costs.
        //
        // TRIED AND MEASURED, 2026-09-03: pushing GAMEOBJECT_TYPE_ID as a plain
        // field update instead -- ForceValuesUpdateAtIndex, the same mechanism
        // the core uses for GAMEOBJECT_DYN_FLAGS on a quest chest -- removes the
        // blink completely AND the objects stop being hoverable. So the client
        // caches interactivity at creation exactly as it caches the model.
        //
        // That is worth stating because the standing rule was only ever
        // evidenced for RENDERING, and it was reasonable to hope interactivity
        // was decided later. It is not. The rule is about the creation block
        // itself, not about what the field happens to control.
        //
        // The only way to change a type without a blink would be to spawn a
        // second, GOOBER copy and despawn the original -- two guids for one
        // object, when house_object.id IS the guid. Not worth it for a flash
        // that happens twice per editing session and reads as the mode changing.
        FlashObject(player, objectGuid);
    }
}

// Called from instance_player_house::Update. One pass per player editing in
// this house -- which is at most a handful, and usually one.
// KEEP THE SESSION IN STEP WITH THE HOUSE. Both used to summon and despawn a
// gear; what they do now is add and remove an object from the set the client is
// told is clickable, and let Reconcile deliver the difference.
//
// Reconciling immediately rather than waiting for the next scheduled pass:
// placing something and having it be clickable half a second later is the kind
// of lag that reads as a bug, and picking something up should stop it being
// clickable before the menu closes.
//
// IT STARTS A SESSION IF THERE IS NOT ONE, which is the whole reason a freshly
// placed object is arrangeable without turning edit mode on first. It used to
// return here when the player had no session, so `.house object add` with edit
// off placed something that could not be clicked -- and `.house object edit <n>`
// answered "right-click it to arrange" having done nothing at all.
//
// The session it starts is a per-object one (`all` stays false), so it holds
// exactly what was added to it and the menu's Done button can end it. A global
// session is never created here: `.house edit on` means "everything in this
// house, as the house changes", and Reconcile rebuilds `wanted` from the house
// on every pass -- so a session that claimed to be global would swallow the
// room on its next tick.
void HouseMgr::AddToEdit(Player* player, uint32 objectGuid)
{
    if (!player)
        return;

    // The house comes off the object rather than from where the player is
    // standing, so this cannot open a session pointed at the wrong house.
    HouseObject const* o = GetObject(objectGuid);
    if (!o)
        return;

    HouseEdit& session = m_editing[player->GetGUIDLow()];
    if (!session.houseId)                   // default-constructed a moment ago
        session.houseId = o->houseId;

    session.wanted.insert(objectGuid);
    Reconcile(player, session);
}

// Safe to call for an object that has just been deleted, which is the main
// caller: Reconcile's unmark pass goes through FlashObject, and that returns
// quietly when the row is already gone.
void HouseMgr::DropFromEdit(Player* player, uint32 objectGuid)
{
    auto itr = m_editing.find(player->GetGUIDLow());
    if (itr == m_editing.end())
        return;

    // ONLY MEANINGFUL FOR AN INDIVIDUAL SESSION. Under global edit Reconcile is
    // about to rebuild `wanted` from every object in the house, so this erase is
    // undone before the next tick -- which is why nothing calls it in that state
    // and why `.house object edit <n>` refuses there rather than flickering.
    itr->second.wanted.erase(objectGuid);
    Reconcile(player, itr->second);

    // AN EMPTIED PER-OBJECT SESSION IS NOT EDIT MODE, and leaving one standing
    // is not harmless: it answers IsEditing, which is what the bare
    // `.house edit` toggle reads to decide which way to go -- so placing one
    // chair and pressing Done would leave the next `.house edit` turning
    // something off. Reconcile has just unmarked the last object, so there is
    // nothing left to hand back to the client.
    //
    // A GLOBAL session is left alone even when empty: `.house edit on` in a
    // room you then clear out is a state somebody asked for, and Reconcile
    // refills it the moment anything is placed.
    if (!itr->second.all && itr->second.wanted.empty())
        m_editing.erase(itr);
}

void HouseMgr::UpdateEditSessions(Map* map)
{
    if (!map || m_editing.empty())
        return;

    for (const auto& ref : map->GetPlayers())
    {
        Player* player = ref.getSource();
        if (!player)
            continue;

        auto itr = m_editing.find(player->GetGUIDLow());
        if (itr != m_editing.end())
            Reconcile(player, itr->second);
    }
}

bool HouseMgr::SetEditMode(Player* player, bool on, std::string& error)
{
    House* house = GetHouseAt(player);
    if (!house)
    {
        error = "You are not in a house.";
        return false;
    }
    // The same answer CheckOwner gives -- one function, so a developer
    // authoring a template can click the furniture here exactly as they can
    // use the commands there.
    if (!CanEditHouse(player, *house))
    {
        error = "This is not your house.";
        return false;
    }

    // Always tear down first, so turning edit mode on twice cannot mark the
    // same objects twice.
    ClearEdit(player);
    if (!on)
    {
        // TURNING EDIT OFF IS A "DONE" GESTURE, so let go of the object too.
        // Strictly the selection is independent of edit mode -- you can select
        // and nudge with nothing clickable at all, and that still works. But
        // nobody turns edit mode off meaning "keep aiming at the chair", and a
        // glow left burning on a board you have just put away is state the
        // player has stopped thinking about.
        //
        // Only on the way OFF. ClearEdit above also runs when turning edit ON,
        // as the re-entrant teardown, and clearing there would throw away a
        // selection made a moment earlier by the very command about to make
        // that object clickable.
        ClearSelection(player);
        return true;
    }

    // The fabric is filtered out here as well as in Reconcile, only so the
    // "nothing to edit" answer can tell the two cases apart: an empty house and
    // a fully furnished one where none of it is yours read very differently to
    // somebody who just typed .house edit and found nothing would light up.
    std::vector<HouseObject const*> objects = GetObjects(house->id);
    std::vector<HouseObject const*> mine;
    for (HouseObject const* o : objects)
        if (!IsHouseFabric(*o))
            mine.push_back(o);

    if (mine.empty())
    {
        error = objects.empty()
            ? "There is nothing here to edit."
            : "Everything here came with the house. Place something of your own first.";
        return false;
    }

    HouseEdit session;
    session.houseId = house->id;
    session.all = true;
    for (HouseObject const* o : mine)
        session.wanted.insert(o->guid);

    HouseEdit& stored = m_editing[player->GetGUIDLow()];
    stored = session;

    // Marked here rather than left to the next scheduled pass, because
    // `.house edit on` should light the room up now -- but through Reconcile,
    // which is the same decision it will be making every half second from now
    // on. Doing it in two places is how the two drift apart.
    Reconcile(player, stored);
    return true;
}

void HouseMgr::ClearEdit(Player* player)
{
    if (!player)
        return;

    auto itr = m_editing.find(player->GetGUIDLow());
    if (itr == m_editing.end())
        return;

    // Hand every marked object back to the client as its real type. This has to
    // happen while the session is still standing, because the override in
    // BuildValuesUpdate asks InEditSession -- so flashing after the erase below
    // is what sends the true type, and flashing before it would send GOOBER
    // again. Order is load-bearing: copy, erase, then flash.
    std::set<uint32> const marked = itr->second.marked;

    m_editing.erase(itr);

    for (uint32 objectGuid : marked)
        FlashObject(player, objectGuid);
}

//== the gossip script =======================================================

// THE MENU IS PARAMETERISED ON THE GOSSIP SENDER, and the two arguments are
// genuinely different things: senderGuid is only ever handed back to the client
// so the next click returns here, and objectGuid is the thing being moved.
//
// They are the same object today -- you right-click the furniture and the
// furniture is the sender. They were not when a gear stood beside each piece
// and sent its own guid, which is why the split exists; it costs nothing and is
// what would let anything else open this menu on an object's behalf.
static bool HouseObjectMenuHello(Player* player, ObjectGuid senderGuid, uint32 objectGuid)
{
    HouseObject const* obj = objectGuid ? sHouseMgr.GetObject(objectGuid) : nullptr;
    if (!obj)
    {
        // Not one of this player's, or one that has already been torn down.
        player->PlayerTalkClass->CloseGossip();
        return true;
    }

    SendMain(player, senderGuid, obj);
    return true;
}

// The coded option arrives here instead, with whatever was typed. Everything
// else about the page is identical, so it converts the text to an action and
// hands over rather than duplicating the dispatch.
static bool HouseObjectMenuSelect(Player* player, ObjectGuid senderGuid, uint32 objectGuid, uint32 sender, uint32 action);

static bool HouseObjectMenuSelectCode(Player* player, ObjectGuid senderGuid, uint32 objectGuid,
                                     uint32 sender, uint32 action, const char* code)
{
    if (sender != HOUSE_SENDER || action != ACT_SIZE_SET)
        return false;

    if (!objectGuid)
    {
        player->PlayerTalkClass->CloseGossip();
        return true;
    }

    // atof rather than a strict parse: "1.5x" and " 1.5 " should both work, and
    // anything genuinely unreadable comes back 0 and is rejected below.
    float const wanted = code ? float(atof(code)) : 0.0f;
    if (wanted <= 0.0f)
    {
        ChatHandler(player).SendSysMessage("Type a number bigger than zero. 1 is normal size.");
        SendSizePage(player, senderGuid, sHouseMgr.GetObject(objectGuid));
        return true;
    }

    std::string error;
    if (!sHouseMgr.ScaleObject(player, objectGuid, wanted, false, error))
        ChatHandler(player).SendSysMessage(error.c_str());

    SendSizePage(player, senderGuid, sHouseMgr.GetObject(objectGuid));
    return true;
}

static bool HouseObjectMenuSelect(Player* player, ObjectGuid senderGuid, uint32 objectGuid,
                                  uint32 sender, uint32 action)
{
    if (sender != HOUSE_SENDER)
        return false;

    HouseObject const* obj = objectGuid ? sHouseMgr.GetObject(objectGuid) : nullptr;
    if (!obj)
    {
        player->PlayerTalkClass->CloseGossip();
        return true;
    }

    float const step  = sHouseMgr.GetEditStep(player, HOUSE_STEP_MOVE);
    float const turn  = sHouseMgr.GetEditStep(player, HOUSE_STEP_TURN);

    // As a multiplier, once, so grow and shrink cannot drift apart: one is the
    // factor and the other is its reciprocal.
    float const grow  = 1.0f + sHouseMgr.GetEditStep(player, HOUSE_STEP_SIZE) / 100.0f;
    float const face  = player->GetOrientation();
    std::string error;
    bool moved = false;

    switch (action)
    {
        case ACT_PAGE_MAIN:
            SendMain(player, senderGuid, obj);
            return true;
        case ACT_PAGE_MOVE:
            SendMovePage(player, senderGuid);
            return true;
        case ACT_PAGE_TURN:
            SendTurnPage(player, senderGuid);
            return true;
        case ACT_PAGE_SIZE:
            SendSizePage(player, senderGuid, obj);
            return true;

        case ACT_BIGGER:
            moved = sHouseMgr.ScaleObject(player, objectGuid, grow, true, error);
            break;
        case ACT_SMALLER:
            moved = sHouseMgr.ScaleObject(player, objectGuid, 1.0f / grow, true, error);
            break;
        case ACT_BIGGER_LOTS:
            moved = sHouseMgr.ScaleObject(player, objectGuid, 1.5f, true, error);
            break;
        case ACT_SMALLER_LOTS:
            moved = sHouseMgr.ScaleObject(player, objectGuid, 1.0f / 1.5f, true, error);
            break;
        case ACT_SIZE_RESET:
            moved = sHouseMgr.ScaleObject(player, objectGuid, 1.0f, false, error);
            break;

        // Each redraws its own page, which is what makes the number under the
        // cursor change -- the only feedback a gossip menu can give.
        case ACT_STEP_MOVE:
            sHouseMgr.CycleEditStep(player, HOUSE_STEP_MOVE);
            SendMovePage(player, senderGuid);
            return true;
        case ACT_STEP_TURN:
            sHouseMgr.CycleEditStep(player, HOUSE_STEP_TURN);
            SendTurnPage(player, senderGuid);
            return true;
        case ACT_STEP_SIZE:
            sHouseMgr.CycleEditStep(player, HOUSE_STEP_SIZE);
            SendSizePage(player, senderGuid, obj);
            return true;

        // Away is the way the player is looking; left and right are a quarter
        // turn off it. cos/sin of the orientation is the same convention the
        // core uses everywhere for "in front of me".
        case ACT_AWAY:
            moved = sHouseMgr.OffsetObject(player, objectGuid, cos(face) * step, sin(face) * step, 0.0f, error);
            break;
        case ACT_TOWARDS:
            moved = sHouseMgr.OffsetObject(player, objectGuid, -cos(face) * step, -sin(face) * step, 0.0f, error);
            break;
        case ACT_LEFT:
            moved = sHouseMgr.OffsetObject(player, objectGuid, cos(face + M_PI_F / 2) * step, sin(face + M_PI_F / 2) * step, 0.0f, error);
            break;
        case ACT_RIGHT:
            moved = sHouseMgr.OffsetObject(player, objectGuid, cos(face - M_PI_F / 2) * step, sin(face - M_PI_F / 2) * step, 0.0f, error);
            break;
        case ACT_UP:
            moved = sHouseMgr.OffsetObject(player, objectGuid, 0.0f, 0.0f, step, error);
            break;
        case ACT_DOWN:
            moved = sHouseMgr.OffsetObject(player, objectGuid, 0.0f, 0.0f, -step, error);
            break;

        case ACT_TURN_L:
            moved = sHouseMgr.TurnObject(player, objectGuid, turn, false, error);
            break;
        case ACT_TURN_R:
            moved = sHouseMgr.TurnObject(player, objectGuid, -turn, false, error);
            break;
        case ACT_TURN_L90:
            moved = sHouseMgr.TurnObject(player, objectGuid, 90.0f, false, error);
            break;
        case ACT_TURN_R90:
            moved = sHouseMgr.TurnObject(player, objectGuid, -90.0f, false, error);
            break;
        case ACT_FACE_ME:
            moved = sHouseMgr.TurnObject(player, objectGuid, 0.0f, true, error);
            break;
        // Which way a model "faces" is the artist's choice, not a rule -- plenty
        // of doodads have their front on the far side -- so both are needed.
        case ACT_FACE_AWAY:
            moved = sHouseMgr.TurnObject(player, objectGuid, 180.0f, true, error);
            break;

        case ACT_DRAG:
            moved = sHouseMgr.DragObject(player, objectGuid, error);
            break;

        // A toggle, and it redraws the main page so the label flips under the
        // cursor -- the only feedback a gossip menu can give that a switch has
        // actually moved.
        case ACT_SELECT:
        {
            char const* note = nullptr;
            if (sHouseMgr.GetSelectedObject(player) == objectGuid)
            {
                sHouseMgr.ClearSelection(player);
                note = " Unselected.";
            }
            else if (!sHouseMgr.SelectObject(player, objectGuid, error))
                ChatHandler(player).SendSysMessage(error.c_str());

            SendMain(player, senderGuid, obj, note);
            return true;
        }

        // Both redraw the main page, so the group row and the size in the
        // greeting change under the cursor. The selection is left where it
        // is: the next chair groups with the same table.
        case ACT_GROUP_JOIN:
        {
            std::string note;
            uint32 const withGuid = sHouseMgr.GetSelectedObject(player);
            if (!withGuid)
                ChatHandler(player).SendSysMessage("Select the piece to group with first.");
            else if (!sHouseMgr.JoinGroup(player, objectGuid, withGuid, error))
                ChatHandler(player).SendSysMessage(error.c_str());
            else
                note = " Grouped with " + HouseMgr::ObjectLabel(withGuid) + ".";

            SendMain(player, senderGuid, obj, note.empty() ? nullptr : note.c_str());
            return true;
        }
        case ACT_GROUP_LEAVE:
        {
            char const* note = nullptr;
            if (!sHouseMgr.LeaveGroup(player, objectGuid, error))
                ChatHandler(player).SendSysMessage(error.c_str());
            else
                note = " Ungrouped.";

            SendMain(player, senderGuid, obj, note);
            return true;
        }

        // No confirmation, deliberately. Putting something down is one click,
        // so taking it back should be too -- and once furniture is an inventory
        // item this hands it back rather than destroying it, which makes a
        // "cannot be undone" warning a lie as well as a nuisance. There is no
        // box to put one in either: this build's SMSG_GOSSIP_MESSAGE carries
        // only index, icon, coded and text, so AddMenuItem's BoxMessage is
        // never transmitted.
        case ACT_DELETE:
        {
            std::string name = HouseMgr::PieceName(*obj);
            uint32 returned = 0;
            if (!sHouseMgr.RemoveObject(player, objectGuid, error, &returned))
                ChatHandler(player).SendSysMessage(error.c_str());
            else if (returned)
                // Same wording as the command, and for the same reason: this is
                // where repositioning gets taught, because there is no move
                // verb -- you pick it up and place it again at the reticle.
                ChatHandler(player).PSendSysMessage("Picked up the %s. Right-click to place it again.", name.c_str());
            else
                // Nothing came back, so do not say it did. This is the GM case
                // -- .house object add places rows with no item behind them.
                ChatHandler(player).PSendSysMessage("Removed the %s.", name.c_str());

            player->PlayerTalkClass->CloseGossip();
            return true;
        }

        // Finished, rather than merely closing a window. Both halves are about
        // what the NEXT thing you do means: an object left clickable still
        // lights up under the cursor and invites another click, and a selection
        // left set quietly aims every bare command at something you have walked
        // away from.
        case ACT_CLOSE:
        {
            // UNDER GLOBAL EDIT THIS CLOSES THE WINDOW AND NOTHING ELSE, which
            // is the whole of what "Done" claims there. The mode owns every
            // object in the house, so there is no session of this object's own
            // to end -- and the selection is not edit state either, so it is
            // left exactly where it was. Leaving the mode is the row below.
            if (sHouseMgr.IsEditingAll(player))
            {
                player->PlayerTalkClass->CloseGossip();
                return true;
            }

            // INDIVIDUAL: this object is what the session holds, so finishing
            // with it ends the session's interest in it. Both halves are about
            // what the NEXT thing you do means -- an object left clickable
            // invites another click, and a selection left set quietly aims
            // every bare command at something you have walked away from.
            //
            // Only when the selection IS this object. Clearing it because you
            // closed some other object's menu would undo a choice nobody asked
            // to undo.
            if (sHouseMgr.GetSelectedObject(player) == objectGuid)
                sHouseMgr.ClearSelection(player);

            sHouseMgr.DropFromEdit(player, objectGuid);

            player->PlayerTalkClass->CloseGossip();
            return true;
        }

        // THE WHOLE ROOM, rather than the object in front of you, and drawn
        // only while there is a room-wide mode to leave. SetEditMode tears the
        // session down and clears the selection on its own, on the standing
        // rule that turning edit off is a done gesture -- so this is exactly
        // `.house edit off`, reachable without closing the menu and typing.
        case ACT_EDIT_OFF:
        {
            std::string editError;
            if (!sHouseMgr.SetEditMode(player, false, editError))
                ChatHandler(player).SendSysMessage(editError.c_str());
            else
                ChatHandler(player).SendSysMessage("Edit mode off.");

            player->PlayerTalkClass->CloseGossip();
            return true;
        }

        default:
            player->PlayerTalkClass->CloseGossip();
            return true;
    }

    if (!moved)
    {
        ChatHandler(player).SendSysMessage(error.c_str());
        player->PlayerTalkClass->CloseGossip();
        return true;
    }

    // NOTHING HAS TO CHASE THE OBJECT ANY MORE. A gear stood beside each piece
    // and had to be moved with it, and the rule was "whatever moves a piece of
    // furniture moves its handle" -- which had to live in the mutators, not
    // here, or the gear followed an object nudged from this menu and stayed put
    // when the same object was nudged by `.house object move`. The furniture
    // carries its own menu now, so it takes it along for free.
    //
    // Straight back to the page that was just used, so nudging is one click
    // rather than four.
    if (action >= ACT_TURN_L && action <= ACT_FACE_AWAY)
        SendTurnPage(player, senderGuid);
    else if ((action >= ACT_BIGGER && action <= ACT_SMALLER_LOTS) || action == ACT_SIZE_RESET)
        // Re-fetched rather than reusing obj, so the label shows the size that
        // was just applied rather than the one before it.
        SendSizePage(player, senderGuid, sHouseMgr.GetObject(objectGuid));
    else
        SendMovePage(player, senderGuid);

    return true;
}

//== clicking the furniture itself ===========================================
//
// THE SAME MENU, REACHED WITHOUT A GEAR. While an object is in your edit
// session the client is told it is a GOOBER -- per viewer, in
// Object::BuildValuesUpdate -- so it highlights under the cursor and can be
// clicked. What it cannot do is find a script: housing furniture uses 15,550
// stock gameobject_template rows we do not own, so `script_name` is not
// available and pGOHello / pGOGossipSelect will never fire for one.
//
// So the core calls these two directly instead, from the two places a
// gameobject click can arrive: GameObject::Use, and the IsGameObject() branch
// of HandleGossipSelectOptionOpcode. Both are guarded by the housing guid
// range before they get here, and both are `extern` free functions because
// that is the seam this repo already uses to reach a module from the core.
//
// GUARDED BY THE EDIT SESSION, NOT BY OWNERSHIP. InEditSession is the same
// question the type override asks, so a click can only ever reach a menu on an
// object the player was already being shown as clickable. Everything the menu
// then does still goes through the mutators, which check ownership themselves.
// A gameobject guid carries its ENTRY as well as its low guid, and the client
// checks both -- send a mismatched entry and the select comes back for an
// object the client has never heard of. house_object.go_entry is where it
// lives.
static bool HouseSenderGuid(uint32 goGuidLow, ObjectGuid& out)
{
    HouseObject const* o = sHouseMgr.GetObject(goGuidLow);
    if (!o)
        return false;
    out = ObjectGuid(HIGHGUID_GAMEOBJECT, o->goEntry, goGuidLow);
    return true;
}

bool HouseGameObjectUse(Player* player, uint32 goGuidLow)
{
    ObjectGuid senderGuid;
    if (!player || !sHouseMgr.InEditSession(player, goGuidLow) || !HouseSenderGuid(goGuidLow, senderGuid))
        return false;

    return HouseObjectMenuHello(player, senderGuid, goGuidLow);
}

bool HouseGameObjectGossipSelect(Player* player, uint32 goGuidLow, uint32 sender, uint32 action, const char* code)
{
    ObjectGuid senderGuid;
    if (!player || !sHouseMgr.InEditSession(player, goGuidLow) || !HouseSenderGuid(goGuidLow, senderGuid))
        return false;

    if (code)
        return HouseObjectMenuSelectCode(player, senderGuid, goGuidLow, sender, action, code);
    return HouseObjectMenuSelect(player, senderGuid, goGuidLow, sender, action);
}

