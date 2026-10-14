#ifndef _COMPANION_OWNERSHIP_H
#define _COMPANION_OWNERSHIP_H

#include "Common.h"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  Who owns a companion.
//
//  Companions are playerbots created to order on hire and deleted on dismiss,
//  living on a server-owned COMPANION0..N account pool rather than the hiring
//  player's own account (which caps at 9 characters shared with their real
//  ones). That move breaks the module's idea of ownership, which is:
//
//      it is on a random-pool account, OR it is on your account
//
//  A companion is neither, so `.bot delete` would answer "Not your bot". This
//  is the missing third notion: companion guid -> owning character guid.
//
//  EPHEMERAL, WITH A GRACE PERIOD. A companion is not a character anybody
//  keeps; it is an implementation detail with the lifetime of one hire. But
//  since 2026-09-09 the hire survives its owner going away for a while: the
//  owner's logout PARKS it (ParkAll) rather than dismissing it, and the
//  owner's next login brings it back if that was within
//  AiPlayerbot.CompanionParkMinutes, or deletes it if not. A dropped
//  connection and a server restart are the same case (PurgeAtStartup keeps
//  a parked companion whose owner still exists and is inside the window),
//  so relogging for an addon or a rebuild does not cost a dungeon party.
//  Everything the table cannot vouch for is still deleted at startup, which
//  is what keeps a crash from leaking characters into the pool for ever.
//
//  Storage is an in-memory map written through to `tw_char.companion_owner`
//  (sql/custom/066, parked_time from 071). Reads are all in-memory: they
//  happen on the XP path and on every delete, and neither wants a query.
// ---------------------------------------------------------------------------
namespace CompanionOwnership
{
    struct Parked
    {
        uint32 companionGuid;
        uint32 accountId;
        time_t since;
    };
    // The account-name prefix for the companion pool. Deliberately NOT the
    // RNDBOT prefix: that list is how RandomPlayerbotMgr decides what it may
    // recycle, and a companion swept into it would be re-rolled or logged out
    // from under its owner.
    extern const char* ACCOUNT_PREFIX;

    // How many companions one player may have out at once. The party cap of 5
    // bounds a group on its own, so this exists only to stop one person in a
    // duo taking three of the five seats.
    extern const uint32 MAX_PER_PLAYER;

    // Read the table once at startup. A row whose owner still exists and
    // whose parked stamp is inside the window is kept - loaded, parked, and
    // brought back by the owner's next login exactly as after a logout.
    // Every other row, and every character on a pool account the table does
    // not vouch for, is deleted: that is what keeps a crash from leaking
    // characters into the pool for ever.
    void PurgeAtStartup();

    void     Register(uint32 ownerGuid, uint32 companionGuid, uint32 accountId);
    void     Unregister(uint32 companionGuid);

    // 0 when this guid is not a companion at all.
    uint32   GetOwner(uint32 companionGuid);
    bool     IsCompanion(uint32 companionGuid);
    uint32   CountFor(uint32 ownerGuid);

    // Every companion guid this player currently has out, for dismiss-all.
    std::vector<uint32> CompanionsOf(uint32 ownerGuid);

    // Dismissals asked for from somewhere it is not safe to delete a Player -
    // chiefly Group::RemoveMember, which is mid-way through mutating the group
    // when the hook fires. Queue there, drain from the owner's PlayerbotMgr on
    // its next update. A parked companion is never queued: the eviction the
    // core runs at logout goes through the same hook and is not a dismissal.
    void     QueueDismiss(uint32 companionGuid);
    std::vector<uint32> TakeQueuedDismissals(uint32 ownerGuid);

    // The owner is logging out. Stamp every companion they have out so the
    // logout's party eviction is not read as "send them home", and so the
    // next login knows how long they waited. Runs from the module's
    // OnBeforeLogout, which the core fires before it touches the group.
    void     ParkAll(uint32 ownerGuid);
    bool     IsParked(uint32 companionGuid);
    void     Unpark(uint32 companionGuid);

    // A companion the restore has sent back to its owner. Its join runs the
    // hire path (AcceptInvitationAction), and the hire's "Whisper me: ..."
    // hint is for a new hire only. TakeReturning answers once and forgets.
    // In memory only: a restart parks the owner's companions again.
    void     MarkReturning(uint32 companionGuid);
    bool     TakeReturning(uint32 companionGuid);

    // Everything parked for this owner, oldest first. The caller decides who
    // comes back and who has waited too long; Unregister (via delete) and
    // Unpark both clear an entry.
    std::vector<Parked> ParkedOf(uint32 ownerGuid);

    // True if the account is one of ours, used to keep the pool out of the
    // random-bot machinery and to spot a stale character at startup.
    bool     IsCompanionAccount(uint32 accountId);

    // Remember a pool account discovered or created after the startup scan.
    void     NoteAccount(uint32 accountId);

    // A password for a pool account. Nobody ever types it: a companion logs in
    // through the bot holder, never through realmd, so its only job is to be
    // one that nobody can guess. The pool used the account name until
    // 2026-09-25, which let anyone sign in as COMPANION0 from a client.
    std::string RandomPassword();
}

#endif
