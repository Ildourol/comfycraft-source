
#include "playerbot/playerbot.h"
#include "TankTargetValue.h"
#include "PossibleAttackTargetsValue.h"
#include "AttackersValue.h"

using namespace ai;

class FindTargetForTankStrategy : public FindNonCcTargetStrategy
{
public:
    FindTargetForTankStrategy(PlayerbotAI* ai, Unit* held, Player* master) : FindNonCcTargetStrategy(ai), held(held), master(master)
    {
        minThreat = 0;
    }

public:
    virtual void CheckAttacker(Unit* creature, ThreatManager* threatManager) override
    {
        Player* bot = ai->GetBot();
        AiObjectContext* context = ai->GetAiObjectContext();

        // A mob the player has only started on has no threat from the tank, so
        // the least-threat pick took it at once, then dropped it when the cast
        // bar ended and took it again when the spell landed. While the tank has
        // a fight, it waits for the mob to be in combat.
        if (held && creature != held && AttackersValue::IsOnlyStartedOn(creature, master)) return;

        if (IsCcTarget(creature)) return;

        if (!PossibleAttackTargetsValue::IsValid(creature, bot))
        {
            std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
            if (std::find(attackers.begin(), attackers.end(), creature->GetObjectGuid()) == attackers.end())
                return;
        }

        float threat = threatManager->getThreat(bot);
        if (!result || (minThreat - threat) > 0.1f)
        {
            minThreat = threat;
            result = creature;
        }
    }

protected:
    float minThreat;
    Unit* held;
    Player* master;
};


Unit* TankTargetValue::Calculate()
{
    Unit* rti = RtiTargetValue::Calculate();
    if (rti) return rti;

    FindTargetForTankStrategy strategy(ai, AttackersValue::HeldTarget(ai), ai->GetMaster());
    return FindTarget(&strategy);
}
