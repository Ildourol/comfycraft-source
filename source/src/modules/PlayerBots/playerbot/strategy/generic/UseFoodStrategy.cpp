
#include "playerbot/playerbot.h"
#include "UseFoodStrategy.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/actions/ChatShortcutActions.h"
#include "playerbot/strategy/actions/GenericSpellActions.h"
#include "playerbot/strategy/actions/MovementActions.h"
#include "playerbot/strategy/actions/ReachTargetActions.h"

using namespace ai;

void UseFoodStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    triggers.push_back(new TriggerNode(
        "needs food",
        NextAction::array(0, new NextAction("food", 6.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "high mana",
        NextAction::array(0, new NextAction("drink", 6.0f), NULL)));
}

// MANA IS THE HEALER'S JOB, SO THE HEALER FINISHES ITS DRINK.
//
// A fight used to stand every bot up at once: the pull order stripped the
// drink, and in the combat engine anything that casts or moves stands the bot
// first. A healer has nothing to do until somebody is hurt, so it got up to
// wand or buff and started the fight short of mana.
//
// Each release below is a reason the meal must end anyway:
//   - being hit: sitting takes crits and the drink does not outheal a mob;
//   - full mana: nothing left to gain, and the healer may as well be useful;
//   - the master past the leash: the same leash ShouldDrinkValue uses out of
//     combat, so a group that runs off does not leave its healer seated.
//
// Standing is cheapest, so it goes first: this runs on every combat tick of
// every bot, and almost all of them are standing.
static bool SeatedInLeash(PlayerbotAI* ai)
{
    Player* bot = ai->GetBot();
    if (!bot || bot->IsStandState())
        return false;

    if (!bot->getAttackers().empty())
        return false;

    Player* master = ai->GetMaster();
    if (master && master != bot)
    {
        const float leash = sPlayerbotAIConfig.EatDrinkMinDistance +
            ((bot->GetGroup() && bot->GetGroup()->IsRaidGroup()) ? sPlayerbotAIConfig.raidFollowDistance
                                                                 : sPlayerbotAIConfig.followDistance);

        if (bot->GetMapId() != master->GetMapId() || !bot->IsWithinDist(master, leash))
            return false;
    }

    return true;
}

// The same test DrinkAction::isUseful uses: the cheat drink, or a real item's
// periodic mana while sitting. Food is the same with health.
static bool DrinkingForMana(Player* bot)
{
    return (bot->HasAura(24355) || bot->HasAuraType(SPELL_AURA_OBS_MOD_MANA)) &&
        bot->GetPower(POWER_MANA) < bot->GetMaxPower(POWER_MANA);
}

static bool EatingForHealth(Player* bot)
{
    return (bot->HasAura(24005) || bot->HasAuraType(SPELL_AURA_OBS_MOD_HEALTH)) &&
        bot->GetHealth() < bot->GetMaxHealth();
}

bool ai::HealerKeepsDrinking(PlayerbotAI* ai)
{
    if (!ai || !SeatedInLeash(ai))
        return false;

    Player* bot = ai->GetBot();
    return PlayerbotAI::IsHeal(bot) && DrinkingForMana(bot);
}

// Out of combat the meal is protected only while the AI sleeps for it. The AI
// wakes early (a fight starts or ends, an order), and then `update follow`
// moved the bot and the meal ended (2026-10-18). Every class, the same releases.
bool ai::MealKeepsBotSeated(PlayerbotAI* ai)
{
    if (!ai || !SeatedInLeash(ai))
        return false;

    Player* bot = ai->GetBot();
    return DrinkingForMana(bot) || EatingForHealth(bot);
}

// A DENY LIST, NOT AN ALLOW LIST. What stands a bot up is a spell cast
// (PlayerbotAI::CastSpell stands it first) and a move, so those are what is
// held back. Orders are ChatCommandActions and pass: `follow` is the one order
// that is also a MovementAction, and a player who says follow means get up.
//
// The heal and the walk to the patient pass, and the heal is what ends the
// meal: CastSpell stands the bot, and standing removes the drink
// (AURA_INTERRUPT_FLAG_NOT_SEATED).
bool ai::AllowedWhileHealerDrinks(Action* action)
{
    if (!action)
        return true;

    if (dynamic_cast<CastHealingSpellAction*>(action))
        return true;

    if (dynamic_cast<ReachPartyMemberToHealAction*>(action))
        return true;

    if (dynamic_cast<FollowChatShortcutAction*>(action))
        return true;

    if (dynamic_cast<CastSpellAction*>(action) || dynamic_cast<MovementAction*>(action))
        return false;

    return true;
}
