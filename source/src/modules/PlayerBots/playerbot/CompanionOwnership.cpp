#include "playerbot/playerbot.h"
#include "CompanionOwnership.h"

#include "AccountMgr.h"
#include "Chat/Chat.h"
#include "Chat/ComfyCommands.h"
#include "Database/DatabaseEnv.h"
#include "Group/Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Log.h"
#include "playerbot/PlayerbotAIConfig.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <vector>

namespace CompanionOwnership
{
    const char*  ACCOUNT_PREFIX  = "COMPANION";
    const uint32 MAX_PER_PLAYER  = 2;

    namespace
    {
        // companion guid -> owner guid. Small (bounded by players * 2), read on
        // every group XP reward and every delete, so it stays in memory and the
        // table is only ever written through.
        std::map<uint32, uint32> s_owner;

        // Account ids that belong to the pool, resolved once at startup and
        // added to as the pool grows. Cheaper than a LoginDatabase query on
        // every ownership question.
        std::set<uint32> s_accounts;

        // Companions asked to go, not yet gone. See QueueDismiss.
        std::set<uint32> s_pendingDismiss;

        // companion guid -> the pool account it lives on. Needed to log one
        // back in after its owner returns; the player cache has it too, but
        // this is the copy Register was handed and cannot be stale.
        std::map<uint32, uint32> s_account;

        // companion guid -> when its owner logged out. Absent = out with them.
        std::map<uint32, time_t> s_parked;

        // Companions on their way back from a park. See MarkReturning.
        std::set<uint32> s_returning;

        // Players whose companions are on aggro. See SetAggro.
        std::set<uint32> s_aggro;

        // Guards every container above. Owners on different maps call in from
        // their own map threads at once: restores and dismissal drains after a
        // restart, hires, and IsCompanion on every companion tick. Reads take
        // it shared. A write releases it before its synchronous DB write, so a
        // slow write does not stall the reads.
        std::shared_mutex s_lock;
        typedef std::shared_lock<std::shared_mutex> ReadLock;
        typedef std::unique_lock<std::shared_mutex> WriteLock;

        // Callers hold s_lock.
        uint32 OwnerOfLocked(uint32 companionGuid)
        {
            auto it = s_owner.find(companionGuid);
            return it == s_owner.end() ? 0 : it->second;
        }
    }

    bool IsCompanionAccount(uint32 accountId)
    {
        if (!accountId)
            return false;
        ReadLock lock(s_lock);
        return s_accounts.find(accountId) != s_accounts.end();
    }

    void NoteAccount(uint32 accountId)
    {
        if (!accountId)
            return;
        WriteLock lock(s_lock);
        s_accounts.insert(accountId);
    }

    std::string RandomPassword()
    {
        // Letters and digits only, and 16 of them: AccountMgr uppercases the
        // password and refuses anything longer than MAX_ACCOUNT_STR.
        static const char CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::string password;
        for (int i = 0; i < 16; ++i)
            password += CHARS[urand(0, sizeof(CHARS) - 2)];
        return password;
    }

    void PurgeAtStartup()
    {
        // Learn the pool first: the account list is what tells a stale
        // companion from an ordinary character if a row ever goes missing.
        //
        // A pool account made before 2026-09-25 has its own name as its
        // password. Re-key those here, so every server that already has a pool
        // is closed at its next start without anybody running a command.
        //
        // The exact pattern GetOrCreateAccount makes, never a LIKE on the
        // prefix: the second sweep below deletes every character on a pool
        // account, so a player named COMPANIONFAN must not count as one.
        {
            auto result = LoginDatabase.PQuery(
                "SELECT id, username FROM account WHERE username REGEXP '^%s[0-9]+$'", ACCOUNT_PREFIX);
            if (result)
            {
                uint32 rekeyed = 0;
                do
                {
                    Field* fields = result->Fetch();
                    const uint32 accountId = fields[0].GetUInt32();
                    const std::string name = fields[1].GetCppString();
                    NoteAccount(accountId);

                    if (sAccountMgr.CheckPassword(accountId, name, name) &&
                        sAccountMgr.ChangePassword(accountId, RandomPassword(), name) == AOR_OK)
                        ++rekeyed;
                }
                while (result->NextRow());

                if (rekeyed)
                    sLog.outString("[Companions] Gave %u pool account(s) a random password in place of their name.", rekeyed);
            }
        }

        {
            WriteLock lock(s_lock);
            s_owner.clear();
            s_account.clear();
            s_parked.clear();
            s_pendingDismiss.clear();
        }

        // SINCE 2026-09-09 A RESTART IS A LOGOUT, NOT A PURGE. A row whose
        // owner still exists is kept, parked, and the owner's next login
        // brings the companion back inside the window or deletes it past it
        // - the same rule as logging out, because from the player's side a
        // restart IS a logout they did not ask for. The window counts from
        // the parked stamp when there is one (a clean shutdown logs every
        // player out, and ParkAll runs for each), or from now when there is
        // not (the server died with the companion out, and "now" is the
        // first moment anyone could come back for it).
        //
        // Everything else is still deleted: an owner who no longer exists,
        // a stamp already outside the window, a row whose character is gone.
        //
        // Asked of the table rather than sObjectMgr's player cache on
        // purpose: this runs from InitPlayerbotsAtStartup and the cache's
        // load order relative to that is not worth depending on.
        const time_t now    = time(nullptr);
        const time_t window = time_t(sPlayerbotAIConfig.companionParkMinutes) * MINUTE;

        std::set<uint32> kept;
        uint32 deleted = 0, missing = 0, orphaned = 0;

        auto result = CharacterDatabase.Query(
            "SELECT companion_guid, owner_guid, account_id, parked_time FROM companion_owner");
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                const uint32 companionGuid = fields[0].GetUInt32();
                const uint32 ownerGuid     = fields[1].GetUInt32();
                const uint32 accountId     = fields[2].GetUInt32();
                const uint32 parkedTime    = fields[3].GetUInt32();

                auto exists = CharacterDatabase.PQuery(
                    "SELECT 1 FROM characters WHERE guid = '%u'", companionGuid);
                if (!exists)
                {
                    ++missing;
                    continue;
                }

                auto ownerExists = CharacterDatabase.PQuery(
                    "SELECT 1 FROM characters WHERE guid = '%u'", ownerGuid);

                const time_t since = parkedTime ? time_t(parkedTime) : now;

                if (!ownerExists || now - since > window)
                {
                    Player::DeleteFromDB(ObjectGuid(HIGHGUID_PLAYER, companionGuid), accountId, true, true);
                    if (!ownerExists)
                        ++orphaned;
                    else
                        ++deleted;
                    continue;
                }

                {
                    WriteLock lock(s_lock);
                    s_owner[companionGuid]   = ownerGuid;
                    s_account[companionGuid] = accountId;
                    s_parked[companionGuid]  = since;
                }
                NoteAccount(accountId);
                kept.insert(companionGuid);

                if (!parkedTime)
                    CharacterDatabase.DirectPExecute(
                        "UPDATE companion_owner SET parked_time = '%u' WHERE companion_guid = '%u'",
                        uint32(now), companionGuid);
            }
            while (result->NextRow());
        }

        // Drop every row not kept in one statement, rather than one delete
        // per row above: the kept set is the source of truth from here on.
        if (kept.empty())
            CharacterDatabase.DirectExecute("TRUNCATE TABLE companion_owner");
        else
        {
            std::ostringstream keep;
            for (uint32 guid : kept)
                keep << (keep.tellp() ? "," : "") << guid;
            CharacterDatabase.DirectPExecute(
                "DELETE FROM companion_owner WHERE companion_guid NOT IN (%s)", keep.str().c_str());
        }

        // SECOND SWEEP, and it is the one that actually holds. The table is not
        // a reliable census of what is on the pool: a dismiss unregisters the
        // companion before deleting it, so anything that fails in between
        // leaves a character with NO row pointing at it and the sweep above
        // cannot see it. That is not hypothetical - it happened on 2026-09-06,
        // when a companion that never logged out was re-saved by its own live
        // Player after DeleteFromDB had removed it, and survived as an orphan.
        //
        // The pool accounts are ours, so the rule needs no bookkeeping:
        // nothing should be on a COMPANION account at startup that the table
        // does not vouch for. Anything else is stale.
        std::set<uint32> accounts;
        {
            ReadLock lock(s_lock);
            accounts = s_accounts;
        }

        uint32 orphans = 0;
        for (uint32 accountId : accounts)
        {
            auto stale = CharacterDatabase.PQuery(
                "SELECT guid FROM characters WHERE account = '%u'", accountId);
            if (!stale)
                continue;

            do
            {
                const uint32 guid = stale->Fetch()[0].GetUInt32();
                if (kept.count(guid))
                    continue;
                Player::DeleteFromDB(ObjectGuid(HIGHGUID_PLAYER, guid), accountId, true, true);
                ++orphans;
            }
            while (stale->NextRow());
        }

        if (orphans)
            sLog.outString("[Companions] Startup purge: %u orphan(s) swept off the pool accounts with no ownership row.", orphans);

        if (!kept.empty() || deleted || missing || orphaned)
            sLog.outString("[Companions] Startup purge: %u kept for owners to come back to, %u past the %u-minute window deleted, "
                           "%u with no owner deleted, %u row(s) already gone. %u pool account(s) known.",
                           uint32(kept.size()), deleted, sPlayerbotAIConfig.companionParkMinutes, orphaned, missing,
                           uint32(accounts.size()));
        else
            sLog.outDetail("[Companions] Startup purge: nothing to clean. %u pool account(s) known.",
                           uint32(accounts.size()));
    }

    void Register(uint32 ownerGuid, uint32 companionGuid, uint32 accountId)
    {
        if (!ownerGuid || !companionGuid)
            return;

        {
            WriteLock lock(s_lock);
            s_owner[companionGuid] = ownerGuid;
            s_account[companionGuid] = accountId;
            s_parked.erase(companionGuid);
        }
        NoteAccount(accountId);

        // DirectPExecute, not PExecute. The row must be on disk before the
        // character it describes can be dismissed, and CharacterDatabase runs
        // four unordered worker threads - the same asynchrony that let a
        // logout save land after a delete and resurrect the character this
        // morning. A hire is rare enough to afford the synchronous write.
        CharacterDatabase.DirectPExecute(
            "REPLACE INTO companion_owner (companion_guid, owner_guid, account_id, created_time) "
            "VALUES ('%u', '%u', '%u', '%u')",
            companionGuid, ownerGuid, accountId, uint32(time(nullptr)));
    }

    void Unregister(uint32 companionGuid)
    {
        if (!companionGuid)
            return;

        {
            WriteLock lock(s_lock);
            s_owner.erase(companionGuid);
            s_account.erase(companionGuid);
            s_parked.erase(companionGuid);
            s_pendingDismiss.erase(companionGuid);
            s_returning.erase(companionGuid);
        }
        CharacterDatabase.DirectPExecute(
            "DELETE FROM companion_owner WHERE companion_guid = '%u'", companionGuid);
    }

    uint32 GetOwner(uint32 companionGuid)
    {
        ReadLock lock(s_lock);
        return OwnerOfLocked(companionGuid);
    }

    bool IsCompanion(uint32 companionGuid)
    {
        return GetOwner(companionGuid) != 0;
    }

    void QueueDismiss(uint32 companionGuid)
    {
        // Parked is the one case where leaving the party is not a dismissal:
        // the core evicts every bot from the party as their owner logs out,
        // through the same Group::RemoveMember hook, and ParkAll has already
        // run by then. Queue it here and the owner's next login would delete
        // the companions it was about to bring back.
        if (!companionGuid)
            return;
        WriteLock lock(s_lock);
        if (OwnerOfLocked(companionGuid) && s_parked.find(companionGuid) == s_parked.end())
            s_pendingDismiss.insert(companionGuid);
    }

    void ParkAll(uint32 ownerGuid)
    {
        if (!ownerGuid)
            return;

        const time_t now = time(nullptr);
        bool any = false;
        {
            WriteLock lock(s_lock);
            for (const auto& entry : s_owner)
            {
                if (entry.second != ownerGuid)
                    continue;
                s_parked[entry.first] = now;
                any = true;
            }
        }

        if (!any)
            return;

        // DirectPExecute for the same reason Register uses it: the stamp must
        // be on disk before anything later in this logout can act on it, and
        // a logout is rare enough to pay for the synchronous write.
        CharacterDatabase.DirectPExecute(
            "UPDATE companion_owner SET parked_time = '%u' WHERE owner_guid = '%u'",
            uint32(now), ownerGuid);
    }

    bool IsParked(uint32 companionGuid)
    {
        ReadLock lock(s_lock);
        return s_parked.find(companionGuid) != s_parked.end();
    }

    void Unpark(uint32 companionGuid)
    {
        {
            WriteLock lock(s_lock);
            if (!s_parked.erase(companionGuid))
                return;
        }

        CharacterDatabase.DirectPExecute(
            "UPDATE companion_owner SET parked_time = 0 WHERE companion_guid = '%u'", companionGuid);
    }

    void MarkReturning(uint32 companionGuid)
    {
        if (!companionGuid)
            return;
        WriteLock lock(s_lock);
        s_returning.insert(companionGuid);
    }

    bool TakeReturning(uint32 companionGuid)
    {
        WriteLock lock(s_lock);
        return s_returning.erase(companionGuid) > 0;
    }

    std::vector<Parked> ParkedOf(uint32 ownerGuid)
    {
        std::vector<Parked> out;
        ReadLock lock(s_lock);
        for (const auto& entry : s_parked)
        {
            if (OwnerOfLocked(entry.first) != ownerGuid)
                continue;

            const auto acc = s_account.find(entry.first);
            out.push_back(Parked{ entry.first, acc == s_account.end() ? 0u : acc->second, entry.second });
        }

        std::sort(out.begin(), out.end(),
                  [](const Parked& a, const Parked& b) { return a.since < b.since; });
        return out;
    }

    void SetAggro(uint32 playerGuid, bool on)
    {
        WriteLock lock(s_lock);
        if (on)
            s_aggro.insert(playerGuid);
        else
            s_aggro.erase(playerGuid);
    }

    bool IsAggro(uint32 playerGuid)
    {
        ReadLock lock(s_lock);
        return s_aggro.count(playerGuid) != 0;
    }

    std::vector<uint32> TakeQueuedDismissals(uint32 ownerGuid)
    {
        std::vector<uint32> out;
        WriteLock lock(s_lock);
        for (auto it = s_pendingDismiss.begin(); it != s_pendingDismiss.end(); )
        {
            if (OwnerOfLocked(*it) == ownerGuid)
            {
                out.push_back(*it);
                it = s_pendingDismiss.erase(it);
            }
            else if (!OwnerOfLocked(*it))
            {
                // Already gone by another route - dismissed from the gossip, or
                // deleted while the queue was waiting. Drop it silently rather
                // than hand back a guid whose character no longer exists.
                it = s_pendingDismiss.erase(it);
            }
            else
                ++it;
        }
        return out;
    }

    std::vector<uint32> CompanionsOf(uint32 ownerGuid)
    {
        std::vector<uint32> out;
        ReadLock lock(s_lock);
        for (const auto& entry : s_owner)
            if (entry.second == ownerGuid)
                out.push_back(entry.first);
        return out;
    }

    uint32 CountFor(uint32 ownerGuid)
    {
        uint32 n = 0;
        ReadLock lock(s_lock);
        for (const auto& entry : s_owner)
            if (entry.second == ownerGuid)
                ++n;
        return n;
    }
}

// `.companions` names your companions; `.companions sync` sends the same names
// to ComfyUI and prints nothing.
//
// The addon needs this because nothing about a companion reaches the client: it
// is an ordinary Player on a hidden pool account, so the bar cannot tell one
// from a friend who joined you. The addon asks on every party change, which
// covers hire, dismiss, relog and a parked companion coming back without a hook
// in any of them, and it survives a /reload that a push would not.
//
// The reply is `CCOMP\t<name>;<name>`, empty after the tab when there are none.
// Names rather than guids, because names are what the addon can compare against
// its party. MAX_PER_PLAYER keeps it far under a chat packet's 255 bytes.
bool ComfyCommands::HandleCompanionsCommand(ChatHandler* handler, char* args)
{
    Player* me = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!me)
    {
        handler->SendSysMessage("In game only.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::vector<std::string> names;
    for (uint32 guid : CompanionOwnership::CompanionsOf(me->GetGUIDLow()))
    {
        std::string name;
        if (sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, guid), name))
            names.push_back(name);
    }

    std::string list;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i)
            list += ";";
        list += names[i];
    }

    if (args && strcmp(args, "sync") == 0)
    {
        me->SendAddonMessage("CCOMP", list);
        me->SendAddonMessage("CCMODE", CompanionOwnership::IsAggro(me->GetGUIDLow()) ? "aggro" : "defend");
        return true;
    }

    if (names.empty())
    {
        handler->SendSysMessage("No companions out.");
        return true;
    }

    std::string shown;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i)
            shown += ", ";
        shown += names[i];
    }
    handler->PSendSysMessage("Companions: %s.", shown.c_str());
    return true;
}

// `.ccdebug [name] on | off | quiet`: a bot's debug lines, which also go to
// server\companion-logs\<name>.log (PlayerbotAI::TellPlayerNoFacing). `quiet`
// is on with the engine's lines kept out of chat, which dropped the owner's
// frame rate (2026-10-20).
//
// The whisper `nc +debug` that ComfyUI used is a strategy toggle, and a
// companion takes those from a GM only (PlayerbotAI::RunCommand), so on any
// other account /ccdebug did nothing at all. This command is the way in from
// the console too, where there is nobody to whisper. The console and a GM may
// use it on any bot; an owner on its own companion when
// AiPlayerbot.CompanionDebugForOwner is on. The change is queued to the bot,
// which runs it on its own update.
bool ComfyCommands::HandleCompanionDebugCommand(ChatHandler* handler, char* args)
{
    std::vector<std::string> words;
    {
        std::istringstream in(args ? args : "");
        std::string word;
        while (in >> word)
            words.push_back(word);
    }

    int turn = -1; // -1 toggle, 0 off, 1 on
    bool quiet = false;
    if (!words.empty() && (words.back() == "on" || words.back() == "off" || words.back() == "quiet"))
    {
        quiet = words.back() == "quiet";
        turn = words.back() == "off" ? 0 : 1;
        words.pop_back();
    }

    Player* me = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    Player* bot = nullptr;
    if (!words.empty())
        bot = sObjectAccessor.FindPlayerByName(words.front().c_str());
    else if (me)
        bot = handler->GetSelectedPlayer();

    PlayerbotAI* ai = bot ? GetBotAI(bot) : nullptr;
    if (!ai)
    {
        handler->SendSysMessage(words.empty() ? "Target a companion, or name one." : "No bot online by that name.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    bool gm = !handler->m_session || handler->m_session->GetSecurity() >= SEC_DEVELOPER;
    if (!gm)
    {
        bool own = me && CompanionOwnership::GetOwner(bot->GetGUIDLow()) == me->GetGUIDLow();
        if (!own || !sPlayerbotAIConfig.companionDebugForOwner)
        {
            handler->SendSysMessage(own ? "Only a GM may turn on debug here." : "That is not your companion.");
            handler->SetSentErrorMessage(true);
            return false;
        }
    }

    bool on = turn < 0 ? !ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT) : turn == 1;
    ai->SetDebugQuiet(on && quiet);
    ai->QueueOwnCommand(on ? "nc -silent,+debug,+debug action" : "nc +silent,-debug,-debug action");
    if (on)
        handler->PSendSysMessage("Debug on for %s%s: server\\companion-logs\\%s.log", bot->GetName(),
            quiet ? ", file only" : "", bot->GetName());
    else
        handler->PSendSysMessage("Debug off for %s.", bot->GetName());
    return true;
}

// `.cc <order>`: a companion order with nothing in chat (2026-10-21), for the
// bar and for a macro. A party line reached only the companions led by the
// player who said it. This goes to every companion in the group, through the
// same gate in PlayerbotAI::RunCommand, and the replies are still whispers.
// Solo, it goes to the target if that is a companion.
//
// `.cc aggro`, `.cc defend` and `.cc mode` (which swaps the two) set the
// caller's companion mode instead (CompanionOwnership::SetAggro). Each answers
// with one line and on the CCMODE addon prefix, for the bar's button.
bool ComfyCommands::HandleCompanionOrderCommand(ChatHandler* handler, char* args)
{
    Player* me = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!me)
    {
        handler->SendSysMessage("In game only.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string order = args ? args : "";
    order.erase(0, order.find_first_not_of(" \t"));
    order.erase(order.find_last_not_of(" \t") + 1);
    if (order.empty())
    {
        handler->SendSysMessage("Syntax: .cc attack | pull | follow | stay | flee | summon | aggro | defend | mode");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string word = order;
    std::transform(word.begin(), word.end(), word.begin(), ::tolower);
    if (word == "aggro" || word == "defend" || word == "mode")
    {
        const bool aggro = word == "mode" ? !CompanionOwnership::IsAggro(me->GetGUIDLow()) : word == "aggro";
        CompanionOwnership::SetAggro(me->GetGUIDLow(), aggro);
        me->SendAddonMessage("CCMODE", aggro ? "aggro" : "defend");
        handler->SendSysMessage(aggro ? "Aggro: companions attack when you do." :
            "Defend: companions wait until a mob reaches the group.");
        return true;
    }

    std::vector<Player*> to;
    Group* group = me->GetGroup();
    if (group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (member && member != me && GetBotAI(member) && CompanionOwnership::IsCompanion(member->GetGUIDLow()))
                to.push_back(member);
        }
    }
    else if (Player* target = handler->GetSelectedPlayer())
    {
        if (target != me && GetBotAI(target) && CompanionOwnership::IsCompanion(target->GetGUIDLow()))
            to.push_back(target);
    }

    if (to.empty())
    {
        handler->SendSysMessage(group ? "No companion in your group." : "Target a companion, or group with one.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    for (Player* bot : to)
        GetBotAI(bot)->HandleCommand(group ? CHAT_MSG_PARTY : CHAT_MSG_WHISPER, order, *me, LANG_UNIVERSAL);
    return true;
}
