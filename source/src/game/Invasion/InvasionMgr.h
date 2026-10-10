/*
 * InvasionMgr -- world PvP where only one side signs up.
 *
 * THE SHAPE OF IT. You right-click the Invader's Sigil. The server sweeps
 * everyone online for someone flagged for PvP within a few levels of you,
 * tells THEM they are being hunted, and a few seconds later teleports YOU in
 * behind them. When one of you falls the hunt is over, and the invader is sent
 * home to the spot the sigil was used from.
 *
 * A DEFENDER WHO LOSES DIES NORMALLY -- corpse, graveyard, run back. Only the
 * invader is resurrected, and only because they are the one standing a
 * continent from home; see OnPlayerDeath for why the asymmetry is deliberate
 * rather than an oversight.
 *
 * A FALLEN INVADER IS NOT SNATCHED BACK TO LIFE. They lie dead where they fell
 * for Invasion.DeathLingerSeconds, are carried home STILL DEAD, and wake a
 * moment later at the spot they set out from. Standing up instantly on the
 * winner's doorstep and then loitering there until the return timer fired read
 * as a bug in game even though it was doing exactly what it was told.
 *
 * ONLY THE INVADER IS EVER TELEPORTED, and that is the whole consent story.
 * The defender keeps their ground, their friends and their guards; they are
 * never moved, never asked, and lose nothing but the fight. So there is no
 * SMSG_SUMMON_REQUEST here and no accept/decline dialog: the one person who
 * gets moved is the one who pressed the button. The defender's PvP flag is
 * what makes them findable, and the warning is what makes it fair.
 *
 * THE ONLY LIMIT IS ON BEING INVADED. There is no cooldown on the sigil and no
 * cooldown on the person holding it -- hunt again the moment you are home if you
 * want to. What is rationed is how often the same player can be hunted, because
 * that is the half nobody consented to; see m_respite.
 *
 * NOTHING IS PERSISTED, ON PURPOSE. An invasion lives in this vector and
 * nowhere else, so a logout or a restart ends it -- and ending it always means
 * putting the invader back where they started. That is why OnPlayerLogout
 * teleports before the character is saved rather than after: the alternative is
 * a `characters` column, a migration, and a way to strand somebody in the
 * Barrens if the server dies at the wrong moment.
 *
 * IT LEANS ON FACTIONLESS FOR ONE THING. Two players of the same faction can
 * only fight each other because Factionless.Enable makes the PvP flag, rather
 * than the race, decide hostility (docs/features/factionless.md, piece 1).
 * Without that this would be Alliance-versus-Horde only, which halves the pool.
 * Sanctuary is the other half of that feature and it is a hard blocker rather
 * than a nuisance: two players in a sanctuary resolve FRIENDLY, so an invasion
 * that lands in one cannot be fought at all. Both ends are checked, twice --
 * at search time and again on arrival, because people walk.
 */

#ifndef MANGOSSERVER_INVASIONMGR_H
#define MANGOSSERVER_INVASIONMGR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <unordered_map>
#include <vector>

class Player;

class InvasionManager
{
    public:
        InvasionManager() {}

        void Update(uint32 diff);

        // ASKED BEFORE THE CAST STARTS, so a refusal costs no cast bar, no
        // cooldown and no charge -- the same shape HouseFurnitureShouldCast has
        // next door in housing. Every refusal messages the player itself, so the
        // item script has nothing left to say.
        bool CanBeginInvasion(Player* invader);

        // RUN WHEN THE CAST COMPLETES. Re-runs CanBeginInvasion first: a couple
        // of seconds pass while the bar fills and they are enough to get into
        // combat, die, or be teleported somewhere this is not allowed.
        bool BeginInvasion(Player* invader);

        // End of Unit::Kill, behind the pPlayerVictim null check -- Kill runs on
        // every creature death in the world, so this must never be reached for
        // one. `killer` is Kill's own `this`, and decides whether the other
        // side WON or the victim merely died (sql/custom/110).
        void OnPlayerDeath(Player* victim, Unit* killer);

        // WorldSession::LogoutPlayer, and it must stay BEFORE the character is
        // saved: an invader logging out is teleported home first so they do not
        // come back stranded. Returns true when it moved somebody, which is the
        // caller's cue to flush the teleport -- see the call site.
        bool OnPlayerLogout(Player* player);

        bool IsInvolved(ObjectGuid guid) const;

    private:
        enum InvasionPhase
        {
            PHASE_STALKING,     // the defender has been warned; the invader has not arrived
            PHASE_FIGHTING,     // the invader is there and the clock is running
            PHASE_LEAVING,      // it is over; the invader is on their way home
            PHASE_REVIVING,     // a fallen invader has been carried home and is about to wake
        };

        struct Invasion
        {
            ObjectGuid invader;
            ObjectGuid defender;
            WorldLocation origin;       // where the sigil was used, and where the invader goes back to
            InvasionPhase phase = PHASE_STALKING;
            uint32 timer = 0;           // ms left in the current phase
            WorldLocation arena;        // where the invader landed: the duel flag's job
            uint32 outInvader = 0;      // ms before the invader forfeits; 0 is in bounds
            uint32 outDefender = 0;     // the same for the defender
        };

        // Both message the player on refusal. CanInvadeFrom is about the person
        // holding the sigil; IsValidDefender is about everybody else.
        bool CanInvadeFrom(Player* invader) const;
        bool IsValidDefender(Player* invader, Player* target) const;

        // Somebody has just been invaded; leave them alone for a while. Safe to
        // call twice for the same invasion -- the later call simply wins.
        void GrantRespite(ObjectGuid defender);
        bool HasRespite(ObjectGuid defender) const;

        Player* PickDefender(Player* invader) const;

        void Arrive(Invasion& inv, Player* invader, Player* defender);
        bool ArenaForfeit(Invasion& inv, Player* invader, Player* defender, uint32 diff);
        void SendHome(Invasion const& inv, Player* invader);

        // Moves an invasion into PHASE_LEAVING and says why. Either line may be
        // nullptr for "tell this side nothing". delayMs of 0 means the ordinary
        // Invasion.ReturnSeconds; a fallen invader gets the longer linger
        // instead, because lying dead where you fell is the point of it.
        //
        // `winner` is empty unless somebody survived and won; a match that
        // was FIGHTING is written down either way (RecordMatch).
        void Conclude(Invasion& inv, char const* invaderLine, char const* defenderLine,
                      uint32 delayMs = 0, ObjectGuid winner = ObjectGuid());

        // THE RESULT, for the Invasion heroes board. sql/custom/110. Written
        // once, only for a match that reached PHASE_FIGHTING, and never read
        // back: the no-persistence rule above is about the match, and this
        // is only its outcome.
        void RecordMatch(Invasion const& inv, ObjectGuid winner) const;

        std::vector<Invasion> m_invasions;

        // WHO HAS RECENTLY BEEN INVADED, and until when. The limit is on being
        // hunted, not on hunting: an invader who wants to go straight back out
        // is choosing to, and a second sigil costs them nothing anybody else
        // pays. Being invaded is not chosen at all, so back-to-back arrivals
        // are the one thing that turns the feature from an occasional event
        // into harassment -- and the pool of flagged players near your level is
        // small enough that without this the same person gets found again
        // immediately, by everybody.
        //
        // KEYED BY CHARACTER AND HELD IN MEMORY, so it survives a relog and not
        // a restart, and stamped on ARRIVAL rather than on the search -- a hunt
        // that never reached them costs them nothing.
        std::unordered_map<ObjectGuid, time_t> m_respite;
};

extern InvasionManager sInvasionMgr;

#endif
