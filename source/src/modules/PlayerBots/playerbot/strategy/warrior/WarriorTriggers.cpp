
#include "playerbot/playerbot.h"
#include "WarriorTriggers.h"
#include "WarriorActions.h"

using namespace ai;

bool BloodrageBuffTrigger::IsActive()
{
    return AI_VALUE2(uint8, "health", "self target") >= sPlayerbotAIConfig.mediumHealth &&
        AI_VALUE2(uint8, "rage", "self target") < 20;
}

bool SunderArmorDebuffTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (!target)
        return false;

    if (ai->IsTank(bot) && !target->IsPlayer())
        return true;

    return !ai->HasAura("sunder armor", target, true) && !HasMaxDebuffs();
}

// Battle Shout when it is missing or near its end (BattleShoutWanted), and
// only with a fight near. The combat engine stays on while every mob near the
// group is held, the same case as Shield Block and Bloodrage (WarriorActions.h).
// At the start of a pull the pull target is the current target.
bool BattleShoutTrigger::IsActive()
{
    if (!BattleShoutWanted(ai))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (target && sServerFacade.IsAlive(target) && bot->IsValidAttackTarget(target))
        return true;

    for (Unit* attacker : bot->getAttackers())
    {
        if (attacker && sServerFacade.IsAlive(attacker))
            return true;
    }

    return false;
}

bool CommandingShoutTrigger::IsActive()
{
    uint32 comShout = AI_VALUE2(uint32, "spell id", "commanding shout");
    uint32 batShout = AI_VALUE2(uint32, "spell id", "battle shout");
    if (!(comShout || batShout))
        return false;

    if (bot->HasSpell(comShout))
        return !ai->HasAura("commanding shout", bot);

    if (bot->HasSpell(batShout))
        return !ai->HasAura("battle shout", bot);

    return false;
}
