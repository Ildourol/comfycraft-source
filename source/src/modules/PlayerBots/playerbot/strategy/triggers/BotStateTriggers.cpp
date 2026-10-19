
#include "playerbot/playerbot.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/CompanionOwnership.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "Group/Group.h"
#include "BotStateTriggers.h"

using namespace ai;

bool CombatStartTrigger::IsActive()
{
    if (!ai->IsStateActive(BotState::BOT_STATE_COMBAT) && !ai->IsStateActive(BotState::BOT_STATE_DEAD))
    {
        // Check if any member of the group (near this bot) is getting attacked
        return AI_VALUE(bool, "has attackers");
    }

    return false;
}

bool CombatEndTrigger::IsActive()
{
    // Check if the bot is currently in combat
    if (ai->IsStateActive(BotState::BOT_STATE_COMBAT))
    {
        // Check if any member of the group (near this bot) is getting attacked
        return !AI_VALUE(bool, "has attackers");
    }

    return false;
}

bool DeathTrigger::IsActive()
{
    return !ai->IsStateActive(BotState::BOT_STATE_DEAD) && !sServerFacade.IsAlive(bot);
}

bool ResurrectTrigger::IsActive()
{
    return ai->IsStateActive(BotState::BOT_STATE_DEAD) && sServerFacade.IsAlive(bot);
}

// A companion that died is not going to walk back. It never releases (see
// AutoReleaseSpiritAction::isUseful), so it has no corpse, which makes every
// piece of the corpse-run machinery inert: find corpse, revive from corpse,
// spirit healer and repop all start by asking for a corpse that will never
// exist. Before this trigger the only ways out were a party member with a
// resurrection spell, the owner casting one, or the owner knowing to whisper
// "release" and then "corpse run". With a warrior/mage/rogue party there was
// no way out at all and the body simply stayed there.
bool CompanionReviveTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.companionReviveDelay)
        return false;

    // This branch is NOT the reset it looks like. Only the dead engine runs this
    // trigger, and a revived bot leaves that engine before it ticks again, so
    // this seldom sees a live bot. The reset that counts is Restart(), called
    // from PlayerbotAI::OnDeath. Without it a death outside combat read the
    // previous death's stamp and revived at once (fixed 2026-10-18).
    if (sServerFacade.IsAlive(bot))
    {
        outOfCombatSince = 0;
        announcedWait = false;
        return false;
    }

    if (!CompanionOwnership::IsCompanion(bot->GetGUIDLow()))
        return false;

    // Nowhere to put it down. The revive is defined as "at the owner's feet",
    // so with no owner in the world there is no revive - not a revive in place.
    Player* master = ai->GetMaster();
    if (!master || !master->IsInWorld() || !sServerFacade.IsAlive(master))
        return false;

    if (PartyIsFighting())
    {
        outOfCombatSince = 0;
        announcedWait = false;
        return false;
    }

    if (!outOfCombatSince)
    {
        outOfCombatSince = time(nullptr);

        // Announced where the clock starts, which is the only moment the number
        // is worth saying: the wait is measured from the end of the fight, so a
        // line at the death itself would promise thirty seconds and then be
        // wrong for as long as the fight ran on. Said once, because the point is
        // "it is coming back", not a countdown - and a corpse that ticks at you
        // every second in a chat frame this narrow is worse than silence.
        //
        // isPrivate false to match the revive line: party chat when grouped, a
        // whisper otherwise. noRepeat false because the announcement is already
        // gated to the transition, and the alternative is losing the one after a
        // resumed fight to the five-second repeat filter.
        if (!announcedWait)
        {
            announcedWait = true;

            uint32 seconds = sPlayerbotAIConfig.companionReviveDelay;
            std::string line = "Resurrecting in " + std::to_string(seconds) +
                (seconds == 1 ? " second." : " seconds.");

            // ignoreSilent: companion state, kept even when the companion
            // runs +silent - the owner needs the number to know how long
            // to hold.
            ai->TellPlayerNoFacing(master, line, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false, false, true);
        }

        return false;
    }

    return time(nullptr) - outOfCombatSince >= (time_t)sPlayerbotAIConfig.companionReviveDelay;
}

// Asks the party, not the bot. A dead bot is never in combat itself, so its own
// flag would say "quiet" through the whole fight that killed it.
bool CompanionReviveTrigger::PartyIsFighting()
{
    Player* master = ai->GetMaster();
    if (master && master->IsInWorld() && master->GetMapId() == bot->GetMapId() &&
        sServerFacade.IsInCombat(master))
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == bot)
            continue;

        if (!member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;

        if (sServerFacade.IsInCombat(member))
            return true;
    }

    return false;
}
