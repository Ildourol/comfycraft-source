
#include "playerbot/playerbot.h"
#include "CcTargetValue.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/Action.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "Group/Group.h"

using namespace ai;

// A MARK IS PLANNING, NOT AN ORDER TO CAST.
//
// Crowd control was mirrored into the NON-COMBAT engine on 2026-09-07 so a
// marked mob could be controlled during a tank's pull - which is out of combat
// for everyone except the tank, and is the one moment cc is really worth
// anything. That worked, and it also made the mark on its own sufficient: walk
// up to a camp with nothing happening, put moon on one of them, and the mage
// sheeps it immediately. Which pulls the camp, on behalf of a party that had
// not decided to fight yet.
//
// Marking says WHICH one when the fight starts. It does not say that it has.
// So out of combat the cc triggers ask for one more thing, and this is it:
//
//   - somebody else in the group is mid-pull, or
//   - somebody in the group is already fighting, the master included - which
//     covers the master pulling by hand, a real pull that never went through
//     PullStrategy, so there is nothing to ask of it but the combat flag.
//
// Deliberately NOT a check on this bot's own orders. "attack" and "pull my
// target" both put the bot straight into the combat engine, so by the time an
// order could be read here the in-combat branch has already answered.
bool ai::CcFightIsUnderway(PlayerbotAI* ai)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!bot)
        return false;

    // Including this bot's own. IsGroupPullRunning deliberately asks only about
    // OTHER members - it is the group's "hold, the tank is working" - so a
    // paladin holding turn undead while making the pull itself would otherwise
    // be the one bot in the group the gate was shut for.
    if (PullStrategy* pull = PullStrategy::Get(ai))
    {
        if (pull->HasPullStarted())
            return true;
    }

    if (PullStrategy::IsGroupPullRunning(ai))
        return true;

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

void ai::ReportCcIntent(PlayerbotAI* ai, Player* requester)
{
    if (!ai || !requester)
        return;

    Player* bot = ai->GetBot();
    if (!bot)
        return;

    // Every spell that reaches a target through "cc target" - the classes that
    // can actually be told to control the mark. A bot holding none of them is
    // not part of this conversation and stays quiet, which is what keeps a
    // party of five from answering an order aimed at one of them.
    static const char* ccSpells[] =
    {
        "polymorph", "sap", "shackle undead", "banish", "fear",
        "hibernate", "entangling roots", "scare beast", "turn undead",
        "freezing trap",
    };

    std::string spell;
    for (uint32 i = 0; i < sizeof(ccSpells) / sizeof(ccSpells[0]); ++i)
    {
        if (ai->HasSpell(ccSpells[i]))
        {
            spell = ccSpells[i];
            break;
        }
    }

    if (spell.empty())
        return;

    AiObjectContext* context = ai->GetAiObjectContext();

    Unit* marked = AI_VALUE(Unit*, "rti cc target");

    // A druid leaving the mark to a mage is not part of this conversation either.
    if (CcDefersToMage(ai, spell, marked))
        return;

    if (!marked)
    {
        ai->TellPlayerNoFacing(requester, "Nothing marked to " + spell);
        return;
    }

    // Ask the value the bot will actually act on, qualified exactly as the
    // trigger qualifies it.
    Unit* ccTarget = AI_VALUE2(Unit*, "cc target", spell);
    if (ccTarget)
    {
        std::ostringstream out;
        out << "CC target found - " << spell << " on " << ccTarget->GetName();

        // DELIBERATELY NOT QUALIFIED BY CcFightIsUnderway, though it looks like
        // it should be. This report is only ever reached from an attack or a
        // pull order, and PullRequestAction calls it at the TOP of Execute -
        // before the tank has started anything. Every bot in the group runs
        // that in whatever order the group happens to be walked, so the gate is
        // usually still shut at the instant this line is built, and appending
        // "waiting for the pull" would have tagged most ordinary pulls with a
        // caveat that stopped being true a tick later.
        //
        // There is no `cc` command to ask this question outside an order - the
        // addon's CC button sends no chat line at all, only the mark - so there
        // is no moment where the qualification would have been read correctly.

        ai->TellPlayerNoFacing(requester, out.str());
        return;
    }

    // Marked, but the value said no. Say which of the reasons it was, since
    // from the outside "marked and doing nothing" looks the same every time.
    SpellCastResult reason = SPELL_CAST_OK;
    ai->CanCastSpell(spell, marked, 0, nullptr, false, true, false, &reason);

    std::ostringstream out;
    out << "Can't " << spell << " " << marked->GetName();
    if (!marked->IsAlive())
        out << " - it's dead";
    else if (marked->GetMapId() != bot->GetMapId())
        out << " - wrong map";
    else
        out << " (" << (uint32)reason << ")";

    ai->TellPlayerNoFacing(requester, out.str());
}

// THE FIRST CC TO LAND OWNS THE MARK.
//
// Every bot defaults to the same cc icon, so a mage and a balance druid both
// took the moon as their own. CcTargetValue only asked about THIS bot's aura,
// so the druid saw a sheep with no roots on it and rooted it - and roots deal
// nature damage in 1.12, so the first tick broke the sheep.
//
// So a mark that already carries somebody else's holding cc is taken. Root is
// left out on purpose: a frost nova does no damage, so a sheep cast over it
// holds, and roots that do damage are already refused by the dot check in
// CcTargetValue. Stun is left out too, being short: sheep after a kidney shot
// is the chain, not a clash. Knockout is IN, because our Spell.dbc gives sap
// MECHANIC_KNOCKOUT and not MECHANIC_SAPPED.
//
// ...AND SO DOES THE FIRST ONE STARTED.
//
// The aura check alone loses the opening race: when the fight starts nothing
// has landed yet, so the mage and the druid both see a clean mob and both start
// casting. Group members update one after another, so the second bot to tick
// can see the first one's cast in flight and hold off. Root counts here
// although it does not count for auras: roots still being cast will land on
// the sheep and break it.
//
// NOT CACHED, and that is the point of it being a function. CcTargetValue is
// recomputed at most once per wall-clock second (CalculatedValue compares
// time(0)), so two bots can both read "free" from that second and both cast.
// The trigger and the action ask this again at the moment of the cast.
bool ai::CcMarkIsTaken(PlayerbotAI* ai, Unit* marked)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!bot || !marked)
        return false;

    static const uint32 heldMask =
        (1 << (MECHANIC_POLYMORPH - 1)) | (1 << (MECHANIC_SAPPED - 1)) |
        (1 << (MECHANIC_KNOCKOUT - 1))  | (1 << (MECHANIC_SHACKLE - 1)) |
        (1 << (MECHANIC_BANISH - 1))    | (1 << (MECHANIC_SLEEP - 1)) |
        (1 << (MECHANIC_FREEZE - 1))    | (1 << (MECHANIC_FEAR - 1)) |
        (1 << (MECHANIC_TURN - 1));

    for (auto const& entry : marked->GetSpellAuraHolderMap())
    {
        SpellAuraHolder const* holder = entry.second;
        if (holder && holder->GetCasterGuid() != bot->GetObjectGuid() && holder->HasMechanicMask(heldMask))
            return true;
    }

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    static const uint32 castMask = heldMask | (1 << (MECHANIC_ROOT - 1));

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;

        Spell* casting = member->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (casting && casting->m_targets.getUnitTargetGuid() == marked->GetObjectGuid() &&
            (casting->m_spellInfo->GetAllSpellMechanicMask() & castMask))
            return true;
    }

    return false;
}

// A DRUID LEAVES CC TO A MAGE.
//
// Roots deal damage, and the damage can break the roots themselves, so they
// are the worst hold in the list. Hibernate goes with them so the druid has
// one rule, not two. Any living mage in the group who knows polymorph counts,
// bot or player: a player mage sheeps by hand. A dead mage hands cc back to
// the druid.
//
// Only for a mob the sheep can hold. Polymorph's TargetCreatureType is beast,
// humanoid and critter, so the druid still takes a dragonkin (hibernate) and
// everything else (roots). The mask is read from the spell, not written here.
bool ai::CcDefersToMage(PlayerbotAI* ai, std::string const& spell, Unit* marked)
{
    if (spell != "entangling roots" && spell != "hibernate")
        return false;

    Player* bot = ai ? ai->GetBot() : nullptr;
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return false;

    if (marked)
    {
        SpellEntry const* sheep = sSpellMgr.GetSpellEntry(118);
        if (sheep && sheep->TargetCreatureType && !(marked->GetCreatureTypeMask() & sheep->TargetCreatureType))
            return false;
    }

    static const uint32 polymorphs[] = { 118, 12824, 12825, 12826, 28271, 28272 };

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;

        if (member->getClass() != CLASS_MAGE || !member->IsAlive())
            continue;

        for (uint32 id : polymorphs)
        {
            if (member->HasSpell(id))
                return true;
        }
    }

    return false;
}

// Crowd control is opt-in: the only valid target is the one the group has put
// the cc icon on (moon by default, "rti cc <icon>" to change it). With nothing
// marked there is no cc target at all, so the bot does not cc.
//
// It used to treat the mark as a preference and then guess for every unmarked
// mob on the threat list, which made warlocks fear whatever was not being
// tanked - and fear sends it running into the next pull.
Unit* CcTargetValue::Calculate()
{
    std::list<ObjectGuid> possible = AI_VALUE(std::list<ObjectGuid>,"possible targets no los");

    for (std::list<ObjectGuid>::iterator i = possible.begin(); i != possible.end(); ++i)
    {
        ObjectGuid guid = *i;
        Unit* add = ai->GetUnit(guid);
        if (!add)
            continue;

        if (!ai->IsSafe(add))
            continue;

        if (ai->HasMyAura(qualifier, add))
            return NULL;

        if (qualifier == "polymorph")
        {
            if (ai->HasMyAura("polymorph: pig", add))
                return NULL;
            if (ai->HasMyAura("polymorph: turtle", add))
                return NULL;
        }
    }

    // THE MARK IS THE SELECTION. Resolve it directly.
    //
    // This used to filter the mark out of "possible attack targets", which was
    // only ever a way to LOSE it: FindTargetForCcStrategy accepted nothing but
    // the unit equal to "rti cc target" anyway, so the list contributed no
    // choice - only three chances to drop the answer before it was asked.
    //
    // All three now fire in ordinary play:
    //   - the pull hold collapses that list to whatever is hitting this bot,
    //     so nobody could cc during a pull, which is exactly when you want it;
    //   - "focus rti targets" collapses it toward the skull;
    //   - RemoveNonThreating strips anything not currently threatening.
    // Deriving the cc target from an attack list means every narrowing of that
    // list silently turns crowd control off, and each one was added for reasons
    // that have nothing to do with cc.
    //
    // Nothing is loosened by this. Crowd control stays opt-in - no mark, no cc
    // - and the trigger that pushes the spell lives in the COMBAT engine, so a
    // marked mob is not sheeped out of a fight. What changes is that a mob the
    // group has explicitly marked can be controlled even while the same bot is
    // holding off everything else.
    Unit* marked = AI_VALUE(Unit*, "rti cc target");
    if (!marked)
        return NULL;

    if (CcDefersToMage(ai, qualifier, marked))
        return NULL;

    if (!marked->IsInWorld() || marked->GetMapId() != bot->GetMapId())
        return NULL;

    if (!ai->IsSafe(marked) || !marked->IsAlive())
        return NULL;

    // A CC THAT BREAKS ON THE NEXT TICK IS NOT WORTH CASTING.
    //
    // Polymorph, sap and shackle all break on damage, so a mob already carrying
    // a damage-over-time is uncontrollable until that falls off - and the cast
    // does not fail, which is what made this the nastiest of the three. The bot
    // sheeps, the dot ticks, the sheep breaks, the trigger sees no cc aura and
    // sheeps again: a mage locked in place doing nothing else, for as long as
    // the dot lasts.
    //
    // No timer needed to "check back later" - this is recomputed every time the
    // value is asked for, so the target becomes eligible again by itself the
    // moment the dot expires.
    if (!marked->GetAurasByType(SPELL_AURA_PERIODIC_DAMAGE).empty())
        return NULL;

    if (CcMarkIsTaken(ai, marked))
        return NULL;

    // Castability, but IGNORING RANGE - which also forgives line of sight.
    //
    // Being too far away or round a corner is not a reason to give up on a cc
    // target, it is a reason to walk. Refusing to select it here was refusing
    // to start: no cc target meant no trigger, no trigger meant no action, and
    // no action meant nothing ever asked the reach prerequisite to close the
    // gap. Selecting it lets "reach spell" do its job, and everything the cast
    // itself cannot fix is handled where the cast fails.
    if (!ai->CanCastSpell(qualifier, marked, true, nullptr, true, true))
        return NULL;

    return marked;
}
