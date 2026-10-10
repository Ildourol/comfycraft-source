// Playerbot_RecruitBotForPlayer -- the module half of SCRIPT_COMMAND_RECRUIT_BOT (93).
//
// The gossip end of companions: click an option on Hall Steward Corwin and a
// companion of the asked-for role joins your party, or every companion you have
// is dismissed.
//
// REWRITTEN 2026-09-06, from FINDING to MAKING.
//
//   The first version searched the live 300-bot random population for somebody
//   free, of the right role, near your level. That is why the old gossip text
//   talks about sending word to whoever is closest and warns that nobody
//   suitable might be free -- at Goldshire levels "nobody is free" was the
//   normal answer, not an edge case, because ~4-5 bots exist per level split
//   across two factions and nine classes, minus everyone already grouped.
//
//   Companions are now CREATED to order at your exact level and dismissed by
//   deleting them, so availability is infinite and the level gap is always
//   zero. The whole search, and IsAvailableForHire with it, is gone.
//
// WHY IT IS SPLIT ACROSS THE MODULE BOUNDARY:
//   No file under src/game includes a playerbot header - the core does not
//   depend on the module. The established seam is a free function declared
//   `extern` at the core call site, implemented here, and stubbed out in
//   src/game/PlayerbotStubs.cpp for BUILD_PLAYERBOTS=OFF builds.

#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotMgr.h"
#include "playerbot/RandomPlayerbotFactory.h"
#include "playerbot/ChatHelper.h"
#include "playerbot/CompanionOwnership.h"
#include "playerbot/strategy/actions/ChangeTalentsAction.h"

#include "ScriptMgr.h"
#include "ObjectMgr.h"
#include "Group/Group.h"
#include "Chat/Chat.h"

namespace
{
    BotRoles RoleFromScriptOption(uint32 option)
    {
        switch (option)
        {
            case SO_RECRUIT_TANK:   return BOT_ROLE_TANK;
            case SO_RECRUIT_HEALER: return BOT_ROLE_HEALER;
            default:                return BOT_ROLE_DPS;
        }
    }

    const char* RoleName(uint32 option)
    {
        switch (option)
        {
            case SO_RECRUIT_TANK:   return "someone to hold the line";
            case SO_RECRUIT_HEALER: return "a healer";
            default:                return "someone who hits hard";
        }
    }

    bool DismissAll(Player* player, ChatHandler& chat)
    {
        PlayerbotMgr* mgr = GetBotMgr(player);
        if (!mgr)
            return false;

        const std::vector<uint32> mine = CompanionOwnership::CompanionsOf(player->GetGUIDLow());
        if (mine.empty())
        {
            chat.SendSysMessage("Nobody is travelling with you.");
            return false;
        }

        // Names first: DeleteBot destroys the Player, so reading the name after
        // it is a use-after-free. Cheap to get wrong and impossible to see.
        std::vector<std::string> names;
        for (uint32 guid : mine)
            if (Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid)))
                names.push_back(bot->GetName());

        for (uint32 guid : mine)
            mgr->DeleteBot(ObjectGuid(HIGHGUID_PLAYER, guid));

        if (names.size() == 1)
            chat.PSendSysMessage("%s takes their leave.", names[0].c_str());
        else
            chat.PSendSysMessage("Your companions take their leave.");

        return true;
    }
}

// Returns true if the click did something.
bool Playerbot_RecruitBotForPlayer(Player* player, uint32 role, uint32 classId)
{
    if (!player || !sPlayerbotAIConfig.enabled)
        return false;

    ChatHandler chat(player);

    if (role == SO_RECRUIT_DISMISS)
        return DismissAll(player, chat);

    // classId is datalong2: a class the player picked from the gossip page, or
    // 0 for whoever fits the role. Until 2026-09-08 this was the level-search
    // window of the old find-a-free-bot search, and every row passed 0.

    // Only the leader of a party with room may hire. Adding to a group the
    // player does not lead would conscript someone else's companions.
    Group* group = player->GetGroup();
    if (group)
    {
        if (group->GetLeaderGuid() != player->GetObjectGuid())
        {
            chat.SendSysMessage("Only the leader of a party can hire companions.");
            return false;
        }

        if (group->IsFull())
        {
            chat.SendSysMessage("Your party is already full.");
            return false;
        }
    }

    const uint32 have = CompanionOwnership::CountFor(player->GetGUIDLow());
    if (have >= CompanionOwnership::MAX_PER_PLAYER)
    {
        chat.PSendSysMessage("You already have %u travelling with you. Send one home first.", have);
        return false;
    }

    PlayerbotMgr* mgr = GetBotMgr(player);
    if (!mgr)
        return false;

    // Pick the class here rather than leaving it to CreateBot: CreateBot's own
    // fallback is `GetRandomClass(race)` with NO role, so asking for a healer
    // and passing no class can hand you a warrior. HandleGroup picks the class
    // for the same reason.
    const BotRoles wanted = RoleFromScriptOption(role);
    RandomPlayerbotFactory factory(0);

    uint8 cls = 0;

    // A named class is checked the same way the roll below is, but the refusal
    // says why. Every race is every class here, so either side may hire any
    // class (the factory picks a race of the player's side for it).
    if (classId)
    {
        const std::string name = ChatHelper::formatClass(uint8(classId));

        if (ChangeTalentsAction::getPremadePaths(uint8(classId), "", wanted).empty())
        {
            chat.PSendSysMessage("A %s is not %s.", name.c_str(), RoleName(role));
            return false;
        }

        cls = uint8(classId);
    }

    for (uint32 tries = 0; tries < 20 && !cls; ++tries)
    {
        const uint8 rolled = factory.GetRandomClass(0, wanted);
        if (!rolled)
            continue;

        // AND THE CLASS MUST BE ABLE TO REACH THE ROLE.
        //
        // GetRandomClass only says the class CAN fill the role in principle.
        // Whether it ends up filling it is decided by talents, and
        // AutoSelectTalents does not give up when no premade spec matches the
        // role - it falls back to every spec of the class and picks one:
        //
        //     paths = getPremadePaths(cls, "", role);
        //     if (paths.empty() && role != BOT_ROLE_NONE)
        //         paths = getPremadePaths(cls, "", BOT_ROLE_NONE);
        //
        // So asking for a tank could hand back a healer, silently, with no
        // error anywhere - which is exactly what "I ask for a tank and do not
        // get one" looks like. The LFT code already guards this way and its
        // comment records a shaman told to tank coming back restoration.
        if (ChangeTalentsAction::getPremadePaths(rolled, "", wanted).empty())
            continue;

        cls = rolled;
    }

    if (!cls)
    {
        chat.PSendSysMessage("I could not find %s for you just now.", RoleName(role));
        return false;
    }

    std::ostringstream param;
    param << "level=" << uint32(player->GetLevel())
          << " class=" << ChatHelper::formatClass(cls)
          << " role="  << ChatHelper::formatRole(wanted)
          << " group=" << player->GetName();

    // Out of the starter kit before its first save, so it arrives in the gear
    // it rolls and never changes clothes in front of anyone
    // (PlayerbotHolder::CreateBot). ONLY ABOVE LEVEL 1: the gear roll is
    // queued only when level > 1, so a level 1 hire's starter kit IS its gear
    // and taking it would send it into a fight with its fists.
    if (player->GetLevel() > 1)
        param << " naked=1";

    // CreateBot on the PLAYER'S holder, never sRandomPlayerbotMgr: that is what
    // makes this a companion rather than population - the COMPANION account
    // pool, the ownership row and the per-player cap all hang off
    // PlayerbotMgr::CreatesCompanions().
    std::list<std::string> messages;
    ObjectGuid guid;
    mgr->CreateBot(player, param.str(), messages, guid);

    // CreateBot reports through `messages` rather than a return value, and says
    // "Bot created: <name>" on success. Anything else is a refusal worth
    // passing on in its own words - "Account has max characters" and friends
    // are more useful than a generic failure.
    std::string created;
    for (const std::string& line : messages)
    {
        const std::string prefix = "Bot created: ";
        if (line.compare(0, prefix.size(), prefix) == 0)
            created = line.substr(prefix.size());
    }

    if (created.empty())
    {
        for (const std::string& line : messages)
            if (line.find("online") == std::string::npos)
                chat.SendSysMessage(line.c_str());

        sLog.outError("RecruitBot: create failed for %s (role %u, level %u).",
                      player->GetName(), role, uint32(player->GetLevel()));
        return false;
    }

    // Deliberately not "joins you". The bot is queued for login and the party
    // invite fires from its own first update a moment later, so promising the
    // join here would be a lie about a third of a second long - and a lie the
    // whole rest of the day if the login fails.
    chat.PSendSysMessage("%s will be along shortly.", created.c_str());
    return true;
}
