/*
 * InvasionMgr -- see InvasionMgr.h for what this is and why only one side
 * ever gets teleported.
 *
 * THE FILTERS ARE THE FEATURE. Finding an opponent is a loop over everyone
 * online; what makes it a good fight rather than a griefing tool is what the
 * loop throws away. Each rejection below is a rule somebody would otherwise
 * discover the hard way, and the order they are written in is cheapest-first,
 * because this runs over every session.
 *
 * EVERY CHECK RUNS TWICE, at search time and again on arrival. Five seconds
 * pass between the two and a player can spend them walking into a city, taking
 * a boat, dying, or logging out. A check that only runs at search time is a
 * check that can be walked around.
 */

#include "InvasionMgr.h"

#include "Chat.h"
#include "Log.h"
#include "Database/DatabaseEnv.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"
#include "Util.h"

InvasionManager sInvasionMgr;

namespace
{
    // Passed to GetClosePoint as the arriving player's bounding radius. The
    // real value lives on the invader, who is on another map at the time --
    // see Arrive() for why asking them is the wrong move.
    float const ARRIVAL_BOUNDING_RADIUS = 1.0f;

    inline uint32 SecondsConfig(eConfigUInt32Values index)
    {
        return sWorld.getConfig(index) * IN_MILLISECONDS;
    }

    inline void Tell(Player* player, char const* text)
    {
        if (!player || !player->GetSession())
            return;

        ChatHandler(player).SendSysMessage(text);

        // Every refusal path messages the player, so mirroring it to the log
        // makes a test that "did nothing" readable afterwards rather than
        // needing to be run again with somebody watching the chat frame.
        sLog.outBasic("Invasion: -> %s: %s", player->GetName(), text);
    }

    inline void TellFmt(Player* player, char const* fmt, char const* arg)
    {
        if (player && player->GetSession())
            ChatHandler(player).PSendSysMessage(fmt, arg);
    }

    // A player nobody should be able to reach, from either end of the match.
    // This is the shared half of both gates; the asymmetric parts (a cooldown
    // for the invader, a level bracket for the defender) live at the call
    // sites.
    // NOT Player const*. The bot test below is Player::AI(), which is a
    // non-const accessor -- so a const pointer cannot ask the one question
    // that keeps companions out of the pool.
    bool IsAvailableForInvasion(Player* player)
    {
        if (!player || !player->IsInWorld() || !player->GetSession())
            return false;

        if (!player->IsAlive() || player->IsBeingTeleported() || player->IsTaxiFlying())
            return false;

        // A GM is invisible half the time and immune the rest of it; a bot is
        // somebody's companion standing next to them, which is not a fight.
        if (player->IsGameMaster() || player->AI())
            return false;

        // THE HARDCORE TRAP. ResurrectPlayer refuses outright for a hardcore
        // character (Player.cpp, `if (IsHardcore() && !forceHc) return;`), so
        // the res at the end of a match would silently do nothing and
        // KillPlayer would take the character permanently instead -- disconnect,
        // gravestone and all. The core already decided this: IsValidAttackTarget
        // carries a custom guard against hardcore characters flagging by
        // accident. Barring them here is the same decision, made earlier.
        if (player->IsHardcore())
            return false;

        if (player->InBattleGround() || !player->GetMap() || player->GetMap()->Instanceable())
            return false;

        // NO SANCTUARY CHECK HERE. It is a DEFENDER-side rule and lives in
        // IsValidDefender -- see the comment there for why the two ends are not
        // symmetric.
        return true;
    }
}

bool InvasionManager::IsInvolved(ObjectGuid guid) const
{
    for (auto const& inv : m_invasions)
        if (inv.invader == guid || inv.defender == guid)
            return true;

    return false;
}

bool InvasionManager::CanInvadeFrom(Player* invader) const
{
    if (!sWorld.getConfig(CONFIG_BOOL_INVASION_ENABLE))
    {
        Tell(invader, "The sigil is cold. Invasions are disabled.");
        return false;
    }

    if (invader->GetLevel() < sWorld.getConfig(CONFIG_UINT32_INVASION_MIN_LEVEL))
    {
        ChatHandler(invader).PSendSysMessage("You must be level %u to invade.",
            sWorld.getConfig(CONFIG_UINT32_INVASION_MIN_LEVEL));
        return false;
    }

    if (IsInvolved(invader->GetObjectGuid()))
    {
        Tell(invader, "You are already in an invasion.");
        return false;
    }

    if (invader->IsInCombat())
    {
        Tell(invader, "Not while you are fighting.");
        return false;
    }

    // YOU CAN SET OUT FROM A SANCTUARY, and this used to refuse. Sanctuary
    // governs where a fight can HAPPEN, not where a hunt is planned -- and the
    // invader leaves town the instant the sigil fires, so there was never a
    // fight in the city to prevent. Making them walk outside to press a button
    // that teleports them somewhere else was ceremony, and it is the first
    // thing the feature was caught doing in game.
    if (invader->IsHardcore())
    {
        Tell(invader, "Hardcore characters cannot invade. Losing would be permanent.");
        return false;
    }

    if (!IsAvailableForInvasion(invader))
    {
        Tell(invader, "You cannot invade from here.");
        return false;
    }

    return true;
}

bool InvasionManager::IsValidDefender(Player* invader, Player* target) const
{
    if (!target || target == invader)
        return false;

    // THE FLAG IS THE CONSENT, and it is the only consent there is -- nobody
    // is asked, so nobody unflagged is ever found.
    if (!target->IsPvP())
        return false;

    // SANCTUARY IS A DEFENDER-SIDE RULE, and it is not politeness -- it is a
    // broken match. GetReactionTo returns FRIENDLY when both players carry
    // PLAYER_FLAGS_SANCTUARY, so an invasion that LANDS in a city or town
    // cannot be fought at all: the two would simply stand there until the
    // clock ran out. The invader's own sanctuary does not matter, because they
    // are leaving it. The flag is maintained by the factionless sanctuary code
    // in Player::UpdateArea, which covers capitals and towns both.
    if (target->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_SANCTUARY))
        return false;

    uint32 const range = sWorld.getConfig(CONFIG_UINT32_INVASION_LEVEL_RANGE);
    uint32 const mine = invader->GetLevel();
    uint32 const theirs = target->GetLevel();
    if (theirs + range < mine || mine + range < theirs)
        return false;

    // Two people in the same group or raid resolve FRIENDLY in GetReactionTo
    // before any flag is looked at, so the fight would be unwinnable and the
    // match would never end.
    if (invader->GetGroup() && invader->GetGroup() == target->GetGroup())
        return false;

    // NOTE THE ABSENCE OF AN IsInvolved CHECK. It belongs in PickDefender, not
    // here, because this same function is re-run on arrival against the defender
    // of the invasion doing the re-running -- who is, necessarily, involved. Put
    // it here and every single invasion calls itself off five seconds in.
    if (invader->GetSession() && target->GetSession() &&
        invader->GetSession()->GetAccountId() == target->GetSession()->GetAccountId())
        return false;

    return IsAvailableForInvasion(target);
}

void InvasionManager::GrantRespite(ObjectGuid defender)
{
    uint32 const seconds = sWorld.getConfig(CONFIG_UINT32_INVASION_RESPITE_SECONDS);
    if (!seconds)
        return;

    m_respite[defender] = time(nullptr) + seconds;
}

bool InvasionManager::HasRespite(ObjectGuid defender) const
{
    auto const itr = m_respite.find(defender);
    return itr != m_respite.end() && itr->second > time(nullptr);
}

Player* InvasionManager::PickDefender(Player* invader) const
{
    std::vector<Player*> candidates;

    {
        HashMapHolder<Player>::ReadGuard guard(HashMapHolder<Player>::GetLock());
        for (auto const& itr : sObjectAccessor.GetPlayers())
            // THE RESPITE CHECK LIVES HERE, beside IsInvolved and for the same
            // reason. IsValidDefender is re-run on arrival against the defender
            // of the invasion doing the re-running -- who has just been granted
            // one -- so putting it there would call off every invasion the
            // instant it landed.
            if (IsValidDefender(invader, itr.second) &&
                !IsInvolved(itr.second->GetObjectGuid()) &&
                !HasRespite(itr.second->GetObjectGuid()))
                candidates.push_back(itr.second);
    }

    if (candidates.empty())
        return nullptr;

    // At random rather than nearest or lowest. Nearest would make the sigil a
    // way to find the person you already know is over the hill, which is the
    // opposite of the point.
    return candidates[urand(0, candidates.size() - 1)];
}

bool InvasionManager::CanBeginInvasion(Player* invader)
{
    if (!invader)
        return false;

    // Prune here rather than on a timer -- the map only grows when somebody
    // invades, so this is the only place it can need it. It is the respite map,
    // not a cooldown on the caller: THE PERSON HOLDING THE SIGIL IS NEVER PUT ON
    // ONE. Hunting again immediately is a choice they are making; being hunted
    // again immediately is not, which is the whole asymmetry.
    time_t const now = time(nullptr);
    for (auto itr = m_respite.begin(); itr != m_respite.end();)
        itr = (itr->second <= now) ? m_respite.erase(itr) : ++itr;

    if (!CanInvadeFrom(invader))
        return false;

    // DELIBERATELY NOT "is there anybody to hunt". That question is asked at the
    // END of the cast, in BeginInvasion, because answering it here would leak
    // whether the world holds a target before the trance has even begun -- and
    // because somebody can flag up while the bar fills.
    return true;
}

bool InvasionManager::BeginInvasion(Player* invader)
{
    if (!CanBeginInvasion(invader))
        return false;

    Player* defender = PickDefender(invader);
    if (!defender)
    {
        Tell(invader, "You find no one worth hunting.");
        return false;
    }

    // FLAG THE INVADER RATHER THAN REFUSING ONE WHO IS NOT. Right-clicking an
    // item called the Invader's Sigil is not an ambiguous act, and sending
    // somebody away to type /pvp first would be bureaucracy in front of the
    // one thing the item does. It is said out loud either way.
    if (!invader->IsPvP())
    {
        invader->UpdatePvP(true);
        Tell(invader, "You are flagged for war.");
    }

    Invasion inv;
    inv.invader = invader->GetObjectGuid();
    inv.defender = defender->GetObjectGuid();
    inv.origin = WorldLocation(invader->GetMapId(), invader->GetPositionX(),
                               invader->GetPositionY(), invader->GetPositionZ(),
                               invader->GetOrientation());
    inv.phase = PHASE_STALKING;
    inv.timer = SecondsConfig(CONFIG_UINT32_INVASION_WARN_SECONDS);
    m_invasions.push_back(inv);

    // NO RESPITE IS STAMPED HERE, only on arrival. Being warned is not being
    // invaded: a hunt that goes cold in the next five seconds -- because they
    // stepped into a town, or the invader was killed at the door -- must leave
    // them exactly as findable as it found them, or "shake off a hunter" would
    // become the cheapest way to buy five minutes of peace.
    //
    // They are unfindable in the meantime regardless: PickDefender skips
    // anybody already IsInvolved, and they are, from this line onward.

    // The defender is told a hunter is coming and not who, which is the whole
    // value of the warning: it buys them the seconds to get somewhere they
    // would rather be fought, without handing them a name to log out from.
    Tell(defender, "You are being hunted. Something is coming for you.");
    TellFmt(invader, "You take the trail of %s...", defender->GetName());

    sLog.outBasic("Invasion: %s (level %u) is hunting %s (level %u).",
        invader->GetName(), invader->GetLevel(), defender->GetName(), defender->GetLevel());

    return true;
}

void InvasionManager::Arrive(Invasion& inv, Player* invader, Player* defender)
{
    // Ten yards BEHIND them: GetClosePoint works off the object's own
    // orientation, so M_PI_F is directly astern. Landing in front would put
    // the invader in the defender's face with no moment of arrival at all.
    float x, y, z;
    defender->GetClosePoint(x, y, z, ARRIVAL_BOUNDING_RADIUS,
        float(sWorld.getConfig(CONFIG_UINT32_INVASION_ARRIVE_DISTANCE)), M_PI_F);

    // THE SEARCHER ARGUMENT IS DELIBERATELY nullptr. GetNearPoint would use it
    // to correct Z against the searcher's own map, and the invader is still
    // standing on a different one -- so the ground height would be read from
    // the wrong continent. Passing nothing makes the defender's map do it,
    // which is the map the point is actually on.
    invader->TeleportTo(defender->GetMapId(), x, y, z, defender->GetOrientation());

    inv.phase = PHASE_FIGHTING;
    inv.timer = SecondsConfig(CONFIG_UINT32_INVASION_DURATION_SECONDS);

    // THIS IS THE MOMENT THEY COUNT AS INVADED, so this is where the respite is
    // stamped. Conclude stamps it again when the fight ends, which is what makes
    // the clock run from the end rather than the start -- but stamping it here
    // as well means an invasion that ends untidily (the invader crashing out
    // mid-fight, say, which Update handles by simply dropping the row) still
    // leaves the defender covered.
    GrantRespite(inv.defender);

    TellFmt(invader, "You have invaded %s.", defender->GetName());
    TellFmt(defender, "%s has invaded you!", invader->GetName());
}

void InvasionManager::SendHome(Invasion const& inv, Player* invader)
{
    if (!invader || !invader->IsInWorld())
        return;

    // NO MESSAGE HERE. Walking home a winner and being carried home a corpse are
    // different enough to deserve different words, and the callers know which
    // one this is.
    invader->TeleportTo(inv.origin.mapId, inv.origin.x, inv.origin.y, inv.origin.z, inv.origin.o);
}

void InvasionManager::RecordMatch(Invasion const& inv, ObjectGuid winner) const
{
    if (inv.phase != PHASE_FIGHTING)
        return;

    // One statement with an AUTO_INCREMENT key, so there is no duplicate to
    // collide with and no partner write for the unordered PExecute to race.
    CharacterDatabase.PExecute("INSERT INTO character_invasion_match (invader, defender, winner) VALUES ('%u', '%u', '%u')",
                               inv.invader.GetCounter(), inv.defender.GetCounter(), winner.GetCounter());
}

void InvasionManager::Conclude(Invasion& inv, char const* invaderLine, char const* defenderLine,
                               uint32 delayMs, ObjectGuid winner)
{
    // Before the phase changes below: only a match that came to blows counts.
    RecordMatch(inv, winner);

    // RESTAMPED AT THE END, AND THE ORDER MATTERS -- inv.phase is still the old
    // one on this line. A fight that ran its full Invasion.DurationSeconds would
    // otherwise use up most of the respite while it was still going on, and the
    // defender would come out of a five-minute brawl with seconds of cover. Only
    // a real fight counts: concluding out of PHASE_STALKING means they died to a
    // wolf before anybody arrived.
    if (inv.phase == PHASE_FIGHTING)
        GrantRespite(inv.defender);

    inv.phase = PHASE_LEAVING;
    inv.timer = delayMs ? delayMs : SecondsConfig(CONFIG_UINT32_INVASION_RETURN_SECONDS);

    if (invaderLine)
        Tell(sObjectAccessor.FindPlayer(inv.invader), invaderLine);
    if (defenderLine)
        Tell(sObjectAccessor.FindPlayer(inv.defender), defenderLine);
}

void InvasionManager::OnPlayerDeath(Player* victim, Unit* killer)
{
    if (!victim)
        return;

    for (auto& inv : m_invasions)
    {
        bool const victimIsInvader = (inv.invader == victim->GetObjectGuid());
        if (!victimIsInvader && inv.defender != victim->GetObjectGuid())
            continue;

        // A death during the warning window or after the match has already
        // been decided ends it, but nobody is resurrected for it -- they died
        // of something else.
        if (inv.phase != PHASE_FIGHTING)
        {
            if (inv.phase == PHASE_STALKING)
                Conclude(inv, "Your quarry fell to something else.", nullptr);
            return;
        }

        // THE TWO SIDES DIE DIFFERENTLY, AND THAT IS THE POINT.
        //
        // A DEFENDER DIES LIKE ANYBODY ELSE -- corpse, release, graveyard, run
        // back. Nothing here touches them. Standing straight back up was the
        // first version and it was wrong: losing a fight on your own ground
        // should cost what losing a fight always costs, or the whole thing is
        // consequence-free for the person who did not choose it AND for the one
        // who did. Their durability is safe either way, because Unit::Kill
        // already skips the 10% hit whenever a player landed the blow.
        //
        // AN INVADER IS RESURRECTED AND SENT HOME, because they are the one
        // standing a continent away from where they started. Making them corpse
        // run out of the Barrens is not a death penalty, it is a half-hour of
        // walking, and avoiding exactly that is what the origin is FOR.
        //
        // A WIN, FOR THE BOARD (sql/custom/110), is the other side surviving
        // and winning: it was THEM who landed the blow, through a pet or a
        // totem as well as by hand, and they are still standing. A defender
        // who falls to a wolf mid-fight has not been beaten by anybody, and a
        // blow that lands after its striker has died is a draw.
        ObjectGuid const opponent = victimIsInvader ? inv.defender : inv.invader;
        Player* const killerPlayer = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        Player* const opponentPlayer = sObjectAccessor.FindPlayer(opponent);
        ObjectGuid const winner = (killerPlayer && killerPlayer->GetObjectGuid() == opponent &&
                                   opponentPlayer && opponentPlayer->IsAlive()) ? opponent : ObjectGuid();

        if (victimIsInvader)
        {
            // AN INVADER STAYS DEAD, AND THAT IS THE WHOLE SEQUENCE. They lie
            // where they fell for the linger, are carried home a corpse, and
            // wake there. The first version resurrected them here, inside
            // Unit::Kill, which worked exactly as designed and read as a bug:
            // you popped upright on the winner's doorstep and then stood about
            // for five seconds waiting to be teleported.
            //
            // SO NOTHING IS DONE TO THE BODY HERE. KillPlayer() runs on the next
            // Player::Update as it would for any death -- release prompt, the
            // six-minute timer, no Corpse object until they actually release
            // (BuildPlayerRepop is what creates one). Waking them up is
            // PHASE_REVIVING's job, once they are home.
            //
            // THE RELEASE BUTTON IS LEFT ALONE ON PURPOSE. Hiding it would make
            // the ride home unskippable and look better, and it would also mean
            // that anything going wrong between here and PHASE_REVIVING leaves
            // somebody a corpse with no way out. Releasing early just sends them
            // to a graveyard; they are collected from there and woken all the
            // same. The message says to sit still, which is cheaper than taking
            // the choice away.
            Conclude(inv, "You have fallen. Lie still -- you will be carried back.",
                          "You have driven off your attacker!",
                     SecondsConfig(CONFIG_UINT32_INVASION_DEATH_LINGER_SECONDS), winner);
        }
        else
        {
            Conclude(inv, "Your quarry has fallen.", "You have been slain by your invader.", 0, winner);
        }

        return;
    }
}

bool InvasionManager::OnPlayerLogout(Player* player)
{
    if (!player)
        return false;

    ObjectGuid const guid = player->GetObjectGuid();

    for (auto itr = m_invasions.begin(); itr != m_invasions.end(); ++itr)
    {
        if (itr->invader != guid && itr->defender != guid)
            continue;

        bool const wasInvader = (itr->invader == guid);

        if (wasInvader)
        {
            // THIS IS WHY NOTHING IS PERSISTED. The invader goes home before
            // the character is saved, so the position that reaches the database
            // is the one they started from. Logging out in the middle of the
            // Barrens and coming back there is the bug this prevents -- and the
            // teleport only counts if the caller flushes it, which is what the
            // return value is for.
            player->TeleportTo(itr->origin.mapId, itr->origin.x, itr->origin.y,
                               itr->origin.z, itr->origin.o);

            Tell(sObjectAccessor.FindPlayer(itr->defender), "Your invader has fled the world.");

            // Erased below without going through Conclude, so written down
            // here. Nobody won: fleeing is not losing to anybody.
            RecordMatch(*itr, ObjectGuid());
        }
        else
        {
            // A DEFENDER LEAVING MUST NOT ERASE THE INVASION, and this used to.
            // Erasing it drops the invader's ride home on the floor and leaves
            // them standing wherever they invaded -- the exact stranding the
            // stored origin exists to prevent, triggered by the single most
            // natural thing a losing defender does. Concluding instead moves it
            // to PHASE_LEAVING, and Update sends the invader home as usual.
            //
            // Unless it is already leaving, in which case the ride is booked and
            // there is nothing to do but let it run.
            if (itr->phase != PHASE_LEAVING)
                Conclude(*itr, "Your quarry has fled the world. The hunt is over.", nullptr);

            return false;
        }

        m_invasions.erase(itr);
        return wasInvader;
    }

    return false;
}

void InvasionManager::Update(uint32 diff)
{
    for (size_t i = 0; i < m_invasions.size();)
    {
        Invasion& inv = m_invasions[i];

        Player* invader = sObjectAccessor.FindPlayer(inv.invader);
        Player* defender = sObjectAccessor.FindPlayer(inv.defender);

        // Either side gone is the end of it. The invader being gone means
        // there is nothing left to send home; the defender being gone is
        // handled in OnPlayerLogout, so reaching this for them means something
        // less orderly happened.
        if (!invader || !invader->IsInWorld())
        {
            m_invasions.erase(m_invasions.begin() + i);
            continue;
        }

        inv.timer = (inv.timer > diff) ? inv.timer - diff : 0;

        switch (inv.phase)
        {
            case PHASE_STALKING:
            {
                if (inv.timer)
                    break;

                // The five seconds of warning are exactly long enough to walk
                // into a city, so the whole gate is re-run rather than trusted.
                if (!defender || !IsValidDefender(invader, defender) ||
                    !IsAvailableForInvasion(invader))
                {
                    Tell(invader, "The trail goes cold.");
                    Tell(defender, "The feeling passes.");

                    // NOTHING IS CHARGED TO EITHER SIDE. The invader may go
                    // straight back out -- they never got a fight. The defender
                    // gets no respite either: walking into a city to shake
                    // somebody is good play and it worked, but it must not also
                    // buy five minutes of being unfindable, or it becomes the
                    // whole game.
                    m_invasions.erase(m_invasions.begin() + i);
                    continue;
                }

                Arrive(inv, invader, defender);
                break;
            }
            case PHASE_FIGHTING:
            {
                // A defender who walks into a sanctuary, zones into an instance
                // or logs out ends the match rather than standing in one place
                // being unkillable.
                if (!defender || !defender->IsInWorld() ||
                    defender->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_SANCTUARY) ||
                    (defender->GetMap() && defender->GetMap()->Instanceable()))
                {
                    Conclude(inv, "Your quarry is beyond reach.", nullptr);
                    break;
                }

                if (!inv.timer)
                    Conclude(inv, "Time is up. The hunt is over.", "Your invader's time is up.");

                break;
            }
            case PHASE_LEAVING:
            {
                if (inv.timer)
                    break;

                SendHome(inv, invader);

                if (invader->IsAlive())
                {
                    Tell(invader, "The hunt is over. You return.");
                    m_invasions.erase(m_invasions.begin() + i);
                    continue;
                }

                // Carried home dead. Waking happens a beat later so that
                // arriving and standing up are two things the player sees
                // rather than one they miss.
                inv.phase = PHASE_REVIVING;
                inv.timer = SecondsConfig(CONFIG_UINT32_INVASION_REVIVE_SECONDS);
                break;
            }
            case PHASE_REVIVING:
            {
                // WAIT OUT THE TELEPORT AS WELL AS THE TIMER. A far teleport is
                // asynchronous, so resurrecting on the timer alone could stand
                // them up before they have arrived -- back where they died, and
                // then dragged home alive, which is the bug this whole sequence
                // exists to avoid. The timer is already at 0 and stays there, so
                // this just re-checks each tick until the map hands them over.
                if (inv.timer || invader->IsBeingTeleported())
                    break;

                invader->ResurrectPlayer(1.0f, false);

                // If they released during the linger there is a corpse of theirs
                // still lying in the defender's zone. This is what every other
                // resurrect path in the core does about that, and it is a no-op
                // when they never released.
                invader->SpawnCorpseBones();

                Tell(invader, "You wake where you set out.");
                m_invasions.erase(m_invasions.begin() + i);
                continue;
            }
        }

        ++i;
    }
}
