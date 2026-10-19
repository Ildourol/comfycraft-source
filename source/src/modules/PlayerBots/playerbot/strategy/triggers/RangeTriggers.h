#pragma once
#include "playerbot/strategy/Trigger.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/generic/CombatStrategy.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
#include "playerbot/strategy/values/Formations.h"
#include "playerbot/strategy/values/LastMovementValue.h"
#include "playerbot/FleeManager.h"

namespace ai
{
    class EnemyTooCloseForSpellTrigger : public Trigger 
    {
    public:
        EnemyTooCloseForSpellTrigger(PlayerbotAI* ai) : Trigger(ai, "enemy too close for spell") {}
        
        virtual bool IsActive() override
        {
            // A FLEE THAT FOUND NOWHERE TO GO WAITS 5 S (2026-10-19). The trigger
            // fired again every tick at 30 and kept a hunter 5.7 yd from a mob,
            // unable to shoot (8 yd minimum) and unable to switch to melee, and a
            // priest looping the same way 127 times. Now it fights where it stands.
            const time_t noPoint = AI_VALUE(LastMovement&, "last movement").lastNoFleePoint;
            if (noPoint && time(nullptr) - noPoint < 5)
                return false;

            // A bot led by a player with a mob on it stays and fights (MovementAction::
            // Flee): a hunter melees, a healer heals itself. The panic flee is separate.
            if (ai->HasRealPlayerMaster() && !bot->getAttackers().empty())
                return false;

            Unit* target = AI_VALUE(Unit*, "current target");
            if (target)
            {
                if (ai->HasStrategy("follow", BotState::BOT_STATE_COMBAT) ||
                    ai->HasStrategy("guard", BotState::BOT_STATE_COMBAT) ||
                    ai->HasStrategy("wander", BotState::BOT_STATE_COMBAT))
                    if(bot->getClass() != CLASS_HUNTER || sServerFacade.GetDistance2d(bot, target) > 5.0f)
                        return false;                   

                const bool canMove = !PossibleAttackTargetsValue::HasBreakableCC(target, bot) && !PossibleAttackTargetsValue::HasUnBreakableCC(target, bot);

                // Don't move if the target is targeting you and you can't add distance between you and the target
                if (target->GetTarget() == bot && canMove && target->GetSpeedInMotion() > (bot->GetSpeedInMotion() * 0.65))
                {
                    return false;
                }

                float const combatReach = bot->GetCombinedCombatReach(target, false);
                float const minDistance = ai->GetRange("spell") + combatReach;
                float const targetDistance = sServerFacade.GetDistance2d(bot, target) + combatReach;

                // No need to move if the target is rooted and you can shoot
                if (!canMove && (targetDistance > minDistance))
                {
                    return false;
                }

                bool isBoss = false;
                bool isRaid = false;
                bool isVictim = target->GetVictim() && target->GetVictim()->GetObjectGuid() == bot->GetObjectGuid();

                if (target->IsCreature())
                {
                    Creature* creature = ai->GetCreature(target->GetObjectGuid());
                    if (creature)
                    {
                        isBoss = creature->IsWorldBoss();
                    }
                }

                if (bot->GetMap()->IsRaid())
                    isRaid = true;

                // A HUNTER THE SERVER CALLS TOO CLOSE IS TOO CLOSE (2026-10-19). The
                // formula below shrinks the threshold by the target's combat reach, so
                // against Rhahk'Zor it came out near 9 yd while the server refused Auto
                // Shot with TOO_CLOSE (118): the hunter stood 22 s without a shot. The
                // server's own cast check decides.
                if (bot->getClass() == CLASS_HUNTER)
                {
                    SpellCastResult shot = SPELL_CAST_OK;
                    ai->CanCastSpell("auto shot", target, 0, nullptr, true, false, false, &shot);
                    if (shot == SPELL_FAILED_TOO_CLOSE)
                        return true;
                }

                // A HEALER IN A PLAYER'S GROUP KEEPS 10 YD FROM THE FIGHT (2026-10-18).
                // With two elites on the tanking player, the priest stood beside them
                // for the whole fight: the rule below ignores a mob that hits someone
                // else. Any mob on the attack list counts. HealBeforeFleeMultiplier
                // still holds the flee back while somebody is below 90%.
                if (PlayerbotAI::IsHeal(bot) && ai->HasRealPlayerMaster() && bot->getClass() != CLASS_HUNTER)
                {
                    for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "attackers"))
                    {
                        Unit* attacker = ai->GetUnit(guid);
                        if (attacker && sServerFacade.IsAlive(attacker) && sServerFacade.GetDistance2d(bot, attacker) < 10.0f)
                            return true;
                    }

                    return false;
                }

                // Casters have no minimum range — only flee if the mob is actually targeting/attacking this bot.
                // Hunters are excluded: their ranged weapons have a ~8 yd minimum range so they must
                // maintain distance even when the mob is focused on someone else.
                if (bot->getClass() != CLASS_HUNTER && !isVictim && target->GetTarget() != bot)
                    return false;

                //if (isBoss || isRaid)
                //    return sServerFacade.IsDistanceLessThan(targetDistance, (ai->GetRange("spell") + combatReach) / 2);

                float coeff = 0.5f;
                if (target->IsPlayer())
                {
                    if (!isVictim)
                        coeff = 0.4f;
                    else
                        coeff = 0.6f;
                }
                else
                {
                    if (!isVictim)
                        coeff = 0.4f;
                    else
                        coeff = 0.6f;
                }

                if (isRaid)
                    coeff = 0.7f;

                return sServerFacade.IsDistanceLessOrEqualThan(targetDistance, minDistance * coeff);
            }
            return false;
        }
    };

    class EnemyTooCloseForShootTrigger : public Trigger 
    {
    public:
        EnemyTooCloseForShootTrigger(PlayerbotAI* ai) : Trigger(ai, "enemy too close for shoot") {}
        
        virtual bool IsActive() override
        {
            Unit* target = AI_VALUE(Unit*, "current target");
            if (target)
            {
                // Don't move if the target is targeting you and you can't add distance between you and the target
                if (target->GetTarget() == bot && !target->IsRooted() && target->GetSpeedInMotion() > (bot->GetSpeedInMotion() * 0.65))
                {
                    return false;
                }

                float const combatReach = bot->GetCombinedCombatReach(target, false);
                float const minShootDistance = ai->GetRange("shoot") + combatReach;
                float const targetDistance = sServerFacade.GetDistance2d(bot, target) + combatReach;

                // No need to move if the target is rooted and you can shoot
                if (target->IsRooted() && (targetDistance > minShootDistance))
                {
                    return false;
                }

                bool isBoss = false;
                bool isRaid = false;
                bool isVictim = target->GetVictim() && target->GetVictim()->GetObjectGuid() == bot->GetObjectGuid();

                if (target->IsCreature())
                {
                    Creature* creature = ai->GetCreature(target->GetObjectGuid());
                    if (creature)
                    {
                        isBoss = creature->IsWorldBoss();
                    }
                }

                if (bot->GetMap()->IsRaid())
                    isRaid = true;

                //if (isBoss || isRaid)
                //    return sServerFacade.IsDistanceLessThan(targetDistance, (ai->GetRange("spell") + combatReach));

                float coeff = 0.5f;
                if (target->IsPlayer())
                {
                    if (!isVictim)
                        coeff = 0.7f;
                    else
                        coeff = 1.0f;
                }
                else
                {
                    if (!isVictim)
                        coeff = 0.4f;
                    else
                        coeff = 0.6f;
                }

                if (isRaid)
                    coeff = 1.0f;

                return sServerFacade.IsDistanceLessOrEqualThan(targetDistance, minShootDistance * coeff);
            }

            return false;
        }
    };

    class EnemyTooCloseForMeleeTrigger : public Trigger 
    {
    public:
        EnemyTooCloseForMeleeTrigger(PlayerbotAI* ai) : Trigger(ai, "enemy too close for melee", 3) {}
        
        virtual bool IsActive() override
        {
            Unit* target = AI_VALUE(Unit*, "current target");
            if (target && target->IsPlayer())
                return false;

            return target && AI_VALUE2(bool, "inside target", "current target");
        }
    };

    class EnemyInRangeTrigger : public Trigger 
    {
    public:
        EnemyInRangeTrigger(PlayerbotAI* ai, std::string name, float distance, bool enemyMustBePlayer = false, bool enemyTargetsBot = false)
        : Trigger(ai, name)
        , distance(distance)
        , enemyMustBePlayer(enemyMustBePlayer)
        , enemyTargetsBot(enemyTargetsBot) {}
        
        virtual bool IsActive() override
        {
            Unit* target = AI_VALUE(Unit*, "current target");
            if (target)
            {
                if (enemyMustBePlayer && !target->IsPlayer())
                {
                    return false;
                }

                if (enemyTargetsBot && target->GetTarget() != bot)
                {
                    return false;
                }

                return sServerFacade.IsDistanceLessOrEqualThan(AI_VALUE2(float, "distance", "current target"), distance);
            }

            return false;
        }

    protected:
        float distance;
        bool enemyMustBePlayer;
        bool enemyTargetsBot;
    };

    class EnemyIsCloseTrigger : public EnemyInRangeTrigger
    {
    public:
        EnemyIsCloseTrigger(PlayerbotAI* ai) : EnemyInRangeTrigger(ai, "enemy is close", sPlayerbotAIConfig.tooCloseDistance) {}
    };

    class OutOfRangeTrigger : public Trigger 
    {
    public:
        OutOfRangeTrigger(PlayerbotAI* ai, std::string name, float distance) : Trigger(ai, name)
        {
            this->distance = distance;
        }

        virtual bool IsActive() override
        {
            Unit* target = AI_VALUE(Unit*, GetTargetName());
            return target &&
                sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", GetTargetName()), distance);
        }

        virtual std::string GetTargetName() override { return "current target"; }

    protected:
        float distance;
    };

    class EnemyOutOfMeleeTrigger : public OutOfRangeTrigger
    {
    public:
        EnemyOutOfMeleeTrigger(PlayerbotAI* ai) : OutOfRangeTrigger(ai, "enemy out of melee range", sPlayerbotAIConfig.meleeDistance) {}
        
        virtual bool IsActive() override
        {
            Unit* target = AI_VALUE(Unit*, GetTargetName());
            if (!target)
                return false;

            return !bot->CanReachWithMeleeAttack(target) || !bot->IsWithinLOSInMap(target, true);
        }
    };

    class EnemyOutOfSpellRangeTrigger : public OutOfRangeTrigger
    {
    public:
        EnemyOutOfSpellRangeTrigger(PlayerbotAI* ai) : OutOfRangeTrigger(ai, "enemy out of spell range", ai->GetRange("spell")) {}
        
        virtual bool IsActive() override
        {
            Unit* target = AI_VALUE(Unit*, GetTargetName());
            if (!target)
                return false;

            return target && (bot->GetDistance(target, true, DIST_CALC_COMBAT_REACH) > (distance - sPlayerbotAIConfig.contactDistance)) || !bot->IsWithinLOSInMap(target, true);
        }
    };

    class PartyMemberToHealOutOfSpellRangeTrigger : public OutOfRangeTrigger
    {
    public:
        PartyMemberToHealOutOfSpellRangeTrigger(PlayerbotAI* ai) : OutOfRangeTrigger(ai, "party member to heal out of spell range", ai->GetRange("heal")) {}
        virtual std::string GetTargetName() override { return "party member to heal"; }
        
        virtual bool IsActive() override
        {
            Unit* target = AI_VALUE(Unit*, GetTargetName());
            if (!target)
                return false;

            return target && (bot->GetDistance(target, true, DIST_CALC_COMBAT_REACH) > (distance - sPlayerbotAIConfig.contactDistance)) || !bot->IsWithinLOSInMap(target, true);
        }
    };

    class FarFromMasterTrigger : public Trigger 
    {
    public:
        FarFromMasterTrigger(PlayerbotAI* ai, std::string name = "far from master", float distance = 12.0f, int checkInterval = 50) : Trigger(ai, name, checkInterval), distance(distance) {}

        virtual bool IsActive() override
        {
            Unit* master = AI_VALUE(Unit*, "master target");
            if (master && sServerFacade.IsFriendlyTo(bot, master))
            {
                if (master->GetTransport() && master->GetTransport() == bot->GetTransport())
                    return false;

                return sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "master target"), distance);
            }

            return false;
        }

    private:
        float distance;
    };

    class OutOfReactRangeTrigger : public FarFromMasterTrigger
    {
    public:
        OutOfReactRangeTrigger(PlayerbotAI* ai, std::string name = "out of react range", float distance = sPlayerbotAIConfig.reactDistance, int checkInterval = 2) : FarFromMasterTrigger(ai, name, distance, checkInterval) {}
    };

    class NotNearMasterTrigger : public OutOfReactRangeTrigger
    {
    public:
        NotNearMasterTrigger(PlayerbotAI* ai, std::string name = "not near master", int checkInterval = 2) : OutOfReactRangeTrigger(ai, name, 5.0f, checkInterval) {}

        virtual bool IsActive() override
        {
            return FarFromMasterTrigger::IsActive() && !sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "master target"), sPlayerbotAIConfig.reactDistance);
        }
    };

    class UpdateFollowTrigger : public NotNearMasterTrigger
    {
    public:
        UpdateFollowTrigger(PlayerbotAI* ai) : NotNearMasterTrigger(ai, "update follow", 3) {}

        virtual bool IsActive() override
        {
            Unit* followTarget = AI_VALUE(Unit*, "follow target");

            if (!followTarget || !ai->IsSafe(followTarget))
                return false;

            //We need to land or liftoff.
            if (followTarget->IsFlying() != bot->IsFlying() || followTarget->IsTaxiFlying())
                return true;

            Formation* formation = AI_VALUE(Formation*, "formation");

            //Already using proper formation.
            if (sServerFacade.GetChaseTarget(bot) && sServerFacade.GetChaseTarget(bot)->GetObjectGuid() == followTarget->GetObjectGuid() && formation->GetAngle() == sServerFacade.GetChaseAngle(bot) && formation->GetOffset() == sServerFacade.GetChaseOffset(bot))
                return false;

            if (!ai->IsStateActive(BotState::BOT_STATE_COMBAT))
                return true;

            Unit* target = AI_VALUE(Unit*, "current target");

            if (!target)
                return true;

            if (target->GetTarget() == bot) //Try pulling target to follow position
                return true;

            if (!ai->IsRanged(bot)) //Melee bots stay in melee.
                return false;

            WorldPosition formationPosition = AI_VALUE(WorldPosition, "formation position");

            if (formationPosition.sqDistance2d(target) > ai->GetRange("spell")) //Do not move to follow if we can't attack from that position.
                return false;

            return true;
        }
    };

    class StopFollowTrigger : public Trigger
    {
    public:
        StopFollowTrigger(PlayerbotAI* ai, std::string name = "stop follow", int checkInterval = 2) : Trigger(ai, name, checkInterval) {}

        virtual bool IsActive() override
        {
            Unit* followTarget = AI_VALUE(Unit*, "follow target");

            if (!followTarget)
                return true;

            if (bot->GetTransport() != followTarget->GetTransport())
                return true;

            //We need to land or liftoff.
            if (followTarget->IsTaxiFlying())
                return true;

            return false;
        }
    };

    // A hostile mob not in the fight yet, standing or walking, within its aggro
    // radius + 5 yd of the bot. Checked once a second. Not while the bot already
    // stands at its master, or was told to stay or guard.
    class IdleMobNearTrigger : public Trigger
    {
    public:
        IdleMobNearTrigger(PlayerbotAI* ai) : Trigger(ai, "idle mob near", 1) {}

        bool IsActive() override
        {
            Player* master = ai->GetMaster();
            if (!master || master == bot || !master->IsInWorld() || master->GetMapId() != bot->GetMapId() || !sServerFacade.IsAlive(bot))
                return false;

            if (ai->HasStrategy("stay", ai->GetState()) || ai->HasStrategy("guard", ai->GetState()))
                return false;

            if (sServerFacade.GetDistance2d(bot, master) <= sPlayerbotAIConfig.followDistance + 2.0f)
                return false;

            const std::list<Unit*> idle = IdleHostiles(ai);
            return !idle.empty() && InIdleAggro(idle, bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), 5.0f);
        }
    };

    class WanderFarTrigger : public Trigger
    {
    public:
        WanderFarTrigger(PlayerbotAI * ai, std::string name = "wander far", int checkInterval = 2)
            : Trigger(ai, name, checkInterval) {
        }

        bool IsActive() override
        {
            return !AI_VALUE2(bool, "can free move", "wandermax");
        }
    };

    class WanderMediumTrigger : public Trigger
    {
    public:
        WanderMediumTrigger(PlayerbotAI* ai, std::string name = "wander medium", int checkInterval = 2) : Trigger(ai, name, checkInterval) {}

        bool IsActive() override
        {
            return !AI_VALUE2(bool, "can free move", "wandermin") && AI_VALUE2(bool, "can free move", "wandermaz");
        }
    };

    class WanderNearTrigger : public StopFollowTrigger
    {
    public:
        WanderNearTrigger(PlayerbotAI* ai, std::string name = "wander near", int checkInterval = 2) : StopFollowTrigger(ai, name, checkInterval) {}

        bool IsActive() override
        {
            if (StopFollowTrigger::IsActive())
                return true;

            return AI_VALUE2(bool, "can free move", "wandermin");
        }
    };

    class WaitForAttackSafeDistanceTrigger : public Trigger
    {
    public:
        WaitForAttackSafeDistanceTrigger(PlayerbotAI* ai, std::string name = "wait for attack safe distance") : Trigger(ai, name) {}

        virtual bool IsActive() override
        {
            if (WaitForAttackStrategy::ShouldWait(ai))
            {
                // Do not move if stay strategy is set
                if (!ai->HasStrategy("stay", ai->GetState()))
                {
                    // Do not move if currently being targeted
                    const bool isBeingTargeted = !bot->getAttackers().empty();
                    if (!isBeingTargeted)
                    {
                        Unit* target = AI_VALUE(Unit*, "current target");
                        if (target)
                        {
                            const float safeDistance = WaitForAttackStrategy::GetSafeDistance();
                            const float safeDistanceThreshold = WaitForAttackStrategy::GetSafeDistanceThreshold();
                            const float distanceToTarget = sServerFacade.GetDistance2d(bot, target);
                            return (distanceToTarget > (safeDistance + safeDistanceThreshold)) ||
                                   (distanceToTarget < (safeDistance - safeDistanceThreshold));
                        }
                    }
                }
            }

            return false;
        }
    };

    class OutOfFreeMoveRangeTrigger : public Trigger
    {
    public:
        OutOfFreeMoveRangeTrigger(PlayerbotAI* ai, std::string name = "out of free move range") : Trigger(ai, name) {}

        virtual bool IsActive() override
        {
            return !AI_VALUE(bool, "can free move");
        };
    };
}