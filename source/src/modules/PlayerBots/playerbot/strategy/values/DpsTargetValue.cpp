
#include "playerbot/playerbot.h"
#include "DpsTargetValue.h"
#include "LeastHpTargetValue.h"
#include "AttackersValue.h"

using namespace ai;

// A dps leaves the mob it is on only for one with less than this share of its health points.
static const float DPS_SWITCH_HEALTH_RATIO = 0.7f;

// STAY ON THE MOB UNTIL ANOTHER IS MUCH LOWER.
//
// The plain least-health pick ran every two seconds and gave the bot's own mob
// no weight. The player's damage lowers the player's mob, so the bot drifted
// onto it, and two mobs close in health swapped back and forth. Health points,
// not percent, on purpose: a small pet in a pack still dies first.
//
// A mob that only the player has started on is passed over while the bot has a
// fight: it is a reason to join, not to switch.
class FindDpsTargetStrategy : public FindLeastHpTargetStrategy
{
public:
    FindDpsTargetStrategy(PlayerbotAI* ai, Unit* held, Player* master) : FindLeastHpTargetStrategy(ai), held(held), master(master), heldSeen(false) {}

    virtual void CheckAttacker(Unit* attacker, ThreatManager* threatManager) override
    {
        if (held && attacker != held && AttackersValue::IsOnlyStartedOn(attacker, master))
            return;

        if (attacker == held && !attacker->IsPlayer() && !IsCcTarget(attacker))
            heldSeen = true;

        FindLeastHpTargetStrategy::CheckAttacker(attacker, threatManager);
    }

    virtual Unit* GetResult() override
    {
        Unit* least = FindLeastHpTargetStrategy::GetResult();
        if (heldSeen && least && least != held && least->GetHealth() >= held->GetHealth() * DPS_SWITCH_HEALTH_RATIO)
            return held;

        return least;
    }

private:
    Unit* held;
    Player* master;
    bool heldSeen;
};

Unit* DpsTargetValue::Calculate()
{
    Unit* rti = RtiTargetValue::Calculate();
    if (rti) return rti;

    FindDpsTargetStrategy strategy(ai, AttackersValue::HeldTarget(ai), ai->GetMaster());
    return TargetValue::FindTarget(&strategy);
}

class FindMaxHpTargetStrategy : public FindTargetStrategy
{
public:
    FindMaxHpTargetStrategy(PlayerbotAI* ai) : FindTargetStrategy(ai)
    {
        maxHealth = 0;
    }

public:
    virtual void CheckAttacker(Unit* attacker, ThreatManager* threatManager) override
    {
        Group* group = ai->GetBot()->GetGroup();
        if (group)
        {
            uint64 guid = group->GetTargetIcon(4);
            if (guid && attacker->GetObjectGuid() == ObjectGuid(guid))
                return;
        }
        if (!result || result->GetHealth() < attacker->GetHealth())
            result = attacker;
    }

protected:
    float maxHealth;
};

Unit* DpsAoeTargetValue::Calculate()
{
    Unit* rti = RtiTargetValue::Calculate();
    if (rti) return rti;

    FindMaxHpTargetStrategy strategy(ai);
    return TargetValue::FindTarget(&strategy);
}
