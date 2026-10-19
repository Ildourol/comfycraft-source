
#include "playerbot/playerbot.h"
#include "CombatStrategy.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/PlayerbotAIConfig.h"

using namespace ai;

void CombatStrategy::InitCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "invalid target",
        NextAction::array(0, new NextAction("select new target", 89.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "mounted",
        NextAction::array(0, new NextAction("check mount state", 88.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "combat stuck",
        NextAction::array(0, new NextAction("unstuck", 0.7f), NULL)));

    triggers.push_back(new TriggerNode(
        "combat long stuck",
        NextAction::array(0, new NextAction("unstuck", 0.9f), NULL)));

    triggers.push_back(new TriggerNode(
        "often",
        NextAction::array(0, new NextAction("use trinket", 50.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "very often",
        NextAction::array(0, new NextAction("use lightwell", 80.0f), NULL)));
}

// How long a healer stays out of melee after being caught in area damage.
// Long enough that it does not simply walk back into the same puddle, short
// enough that it rejoins a fight that has moved on.
static const uint32 HEALER_BACKOFF_SECONDS = 12;

void HealerCautionStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new HealerCautionMultiplier(ai));
    multipliers.push_back(new HealBeforeFleeMultiplier(ai));
}

float HealerCautionMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    // "has area debuff" is the same signal AvoidAoeStrategy flees on - an
    // actual damaging area aura on the bot, not a guess about positioning.
    if (AI_VALUE2(bool, "has area debuff", "self target"))
        m_backOffUntil = time(nullptr) + HEALER_BACKOFF_SECONDS;

    if (time(nullptr) >= m_backOffUntil)
        return 1.0f;

    // Only the actions that walk it back into range. Heals, flee, follow,
    // movement and everything else keep their relevance - a healer that stops
    // healing because it stood in a fire is worse than one that melees.
    const std::string name = action->getName();
    if (name == "dps assist" || name == "melee" || name == "reach melee" ||
        name == "attack anything" || name == "attack" || name == "tank assist")
        return 0.0f;

    return 1.0f;
}

// Heal before running, if there is someone in reach who needs it.
//
// FleeStrategy fires "flee" at ACTION_EMERGENCY + 9 from its `panic` and
// `outnumbered` triggers, which outranks every heal in the book. A healer
// retreating with the party therefore stops healing ENTIRELY for as long as
// the condition holds, which is exactly when the party most needs it - and the
// same ranking is why an `attack` order typed during a flee appears to be
// ignored until it is typed a second time.
//
// So: while somebody is in range and hurt, drop flee far enough for the heal to
// win. It still RUNS - it just gets a cast off on the way, which is what a
// person healing a retreat actually does.
//
// UNLESS THE HEALER IS THE ONE DYING. Below the critical threshold the panic
// flee is correct and keeps its relevance: a dead healer heals nobody, and
// overriding self-preservation to top somebody else up is how you lose both.
float HealBeforeFleeMultiplier::GetValue(Action* action)
{
    if (!action || action->getName() != "flee")
        return 1.0f;

    if (bot->GetHealthPercent() <= (float)sPlayerbotAIConfig.criticalHealth)
        return 1.0f;

    // Somebody really hurt. "party member to heal" always holds the tank, at full
    // health too, so a priest behind a tanking player never stepped back from a
    // mob beside it (2026-10-18).
    Unit* patient = AI_VALUE(Unit*, "party member to heal");
    if (!patient || patient->GetHealthPercent() >= (float)sPlayerbotAIConfig.almostFullHealth)
        return 1.0f;

    return 0.01f;
}

float AvoidAoeStrategyMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    std::string name = action->getName();
    if (name == "follow" || name == "co" || name == "nc" || name == "react" || name == "select new target" || name == "flee")
        return 1.0f;

    uint32 spellId = AI_VALUE2(uint32, "spell id", name);
    const SpellEntry* const pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
    if (!pSpellInfo) return 1.0f;

    if (spellId && pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        return 1.0f;
    else if (spellId && pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
        return 1.0f;

    uint32 CastingTime = !IsChanneledSpell(pSpellInfo) ? GetSpellCastTime(pSpellInfo, bot) : GetSpellDuration(pSpellInfo);

    if (AI_VALUE2(bool, "has area debuff", "self target") && spellId && CastingTime > 0)
    {
        return 0.0f;
    }

    return 1.0f;
}

void AvoidAoeStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "has area debuff",
        NextAction::array(0, new NextAction("flee", ACTION_EMERGENCY + 5), NULL)));
}

void AvoidAoeStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    InitCombatTriggers(triggers);
}

void AvoidAoeStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new AvoidAoeStrategyMultiplier(ai));
}

void AvoidAoeStrategy::InitReactionMultipliers(std::list<Multiplier*>& multipliers)
{
    InitCombatMultipliers(multipliers);
}

void WaitForAttackStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "wait for attack safe distance",
        NextAction::array(0, new NextAction("wait for attack keep safe distance", 60.0f), NULL)));
}

void WaitForAttackStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new WaitForAttackMultiplier(ai));
}

bool WaitForAttackStrategy::ShouldWait(PlayerbotAI* ai)
{
    // Only check if the bot has the strategy enabled
    if (ai->HasStrategy("wait for attack", BotState::BOT_STATE_COMBAT))
    {
        // Only check if bot is in a group with a real player
        Player* bot = ai->GetBot();
        AiObjectContext* context = ai->GetAiObjectContext();
        if (bot->GetGroup() && ai->HasRealPlayerMaster())
        {
            // Don't wait if the current target is an enemy player
            bool enemyPlayer = false;
            Unit* target = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
            if (target)
            {
                Player* player = dynamic_cast<Player*>(target);
                if (player)
                {
                    enemyPlayer = !sServerFacade.IsFriendlyTo(target, player);
                }
            }

            if (!enemyPlayer)
            {
                // Check if bot is currently in combat
                const time_t combatStartTime = AI_VALUE(time_t, "combat start time");
                if (combatStartTime > 0)
                {
                    // Check the amount of time elapsed from the combat start
                    const time_t elapsedTime = time(0) - combatStartTime;
                    return elapsedTime < GetWaitTime(ai);
                }
            }
        }
    }

    return false;
}

uint8 WaitForAttackStrategy::GetWaitTime(PlayerbotAI* ai)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    return AI_VALUE(uint8, "wait for attack time");
}

float WaitForAttackMultiplier::GetValue(Action* action)
{
    // Allow some movement and targeting actions
    const std::string& actionName = action->getName();
    if ((actionName != "wait for attack keep safe distance") && 
        (actionName != "dps assist") && 
        (actionName != "set facing") &&
        (actionName != "pull my target") &&
        (actionName != "pull rti target") &&
        (actionName != "pull start") &&
        (actionName != "pull action") &&
        (actionName != "pull end"))
    {
        return WaitForAttackStrategy::ShouldWait(ai) ? 0.0f : 1.0f;
    }

    return 1.0f;
}

void HealInterruptStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "heal target full health",
        NextAction::array(0, new NextAction("interrupt current spell", ACTION_EMERGENCY), NULL)));
}

void HealInterruptStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    InitCombatTriggers(triggers);
}
