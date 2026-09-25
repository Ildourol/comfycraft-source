#include "playerbot/playerbot.h"
#include "CompanionOwnership.h"

#include "AccountMgr.h"
#include "Database/DatabaseEnv.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Log.h"
#include "playerbot/PlayerbotAIConfig.h"

#include <algorithm>
#include <map>
#include <set>
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
    }

    bool IsCompanionAccount(uint32 accountId)
    {
        return accountId && s_accounts.find(accountId) != s_accounts.end();
    }

    void NoteAccount(uint32 accountId)
    {
        if (accountId)
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
                    s_accounts.insert(accountId);

                    if (sAccountMgr.CheckPassword(accountId, name, name) &&
                        sAccountMgr.ChangePassword(accountId, RandomPassword(), name) == AOR_OK)
                        ++rekeyed;
                }
                while (result->NextRow());

                if (rekeyed)
                    sLog.outString("[Companions] Gave %u pool account(s) a random password in place of their name.", rekeyed);
            }
        }

        s_owner.clear();
        s_account.clear();
        s_parked.clear();
        s_pendingDismiss.clear();

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

                s_owner[companionGuid]   = ownerGuid;
                s_account[companionGuid] = accountId;
                s_parked[companionGuid]  = since;
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
        uint32 orphans = 0;
        for (uint32 accountId : s_accounts)
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
                           uint32(s_accounts.size()));
        else
            sLog.outDetail("[Companions] Startup purge: nothing to clean. %u pool account(s) known.",
                           uint32(s_accounts.size()));
    }

    void Register(uint32 ownerGuid, uint32 companionGuid, uint32 accountId)
    {
        if (!ownerGuid || !companionGuid)
            return;

        s_owner[companionGuid] = ownerGuid;
        s_account[companionGuid] = accountId;
        s_parked.erase(companionGuid);
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

        s_owner.erase(companionGuid);
        s_account.erase(companionGuid);
        s_parked.erase(companionGuid);
        s_pendingDismiss.erase(companionGuid);
        CharacterDatabase.DirectPExecute(
            "DELETE FROM companion_owner WHERE companion_guid = '%u'", companionGuid);
    }

    uint32 GetOwner(uint32 companionGuid)
    {
        auto it = s_owner.find(companionGuid);
        return it == s_owner.end() ? 0 : it->second;
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
        if (companionGuid && GetOwner(companionGuid) && !IsParked(companionGuid))
            s_pendingDismiss.insert(companionGuid);
    }

    void ParkAll(uint32 ownerGuid)
    {
        if (!ownerGuid)
            return;

        const time_t now = time(nullptr);
        bool any = false;
        for (const auto& entry : s_owner)
        {
            if (entry.second != ownerGuid)
                continue;
            s_parked[entry.first] = now;
            any = true;
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
        return s_parked.find(companionGuid) != s_parked.end();
    }

    void Unpark(uint32 companionGuid)
    {
        if (!s_parked.erase(companionGuid))
            return;

        CharacterDatabase.DirectPExecute(
            "UPDATE companion_owner SET parked_time = 0 WHERE companion_guid = '%u'", companionGuid);
    }

    std::vector<Parked> ParkedOf(uint32 ownerGuid)
    {
        std::vector<Parked> out;
        for (const auto& entry : s_parked)
        {
            if (GetOwner(entry.first) != ownerGuid)
                continue;

            const auto acc = s_account.find(entry.first);
            out.push_back(Parked{ entry.first, acc == s_account.end() ? 0u : acc->second, entry.second });
        }

        std::sort(out.begin(), out.end(),
                  [](const Parked& a, const Parked& b) { return a.since < b.since; });
        return out;
    }

    std::vector<uint32> TakeQueuedDismissals(uint32 ownerGuid)
    {
        std::vector<uint32> out;
        for (auto it = s_pendingDismiss.begin(); it != s_pendingDismiss.end(); )
        {
            if (GetOwner(*it) == ownerGuid)
            {
                out.push_back(*it);
                it = s_pendingDismiss.erase(it);
            }
            else if (!GetOwner(*it))
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
        for (const auto& entry : s_owner)
            if (entry.second == ownerGuid)
                out.push_back(entry.first);
        return out;
    }

    uint32 CountFor(uint32 ownerGuid)
    {
        uint32 n = 0;
        for (const auto& entry : s_owner)
            if (entry.second == ownerGuid)
                ++n;
        return n;
    }
}
