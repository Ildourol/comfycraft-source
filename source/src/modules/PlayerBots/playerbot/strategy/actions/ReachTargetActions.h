#pragma once

#include "playerbot/strategy/Action.h"
#include "MovementActions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "playerbot/strategy/NamedObjectContext.h"
#include "playerbot/strategy/values/MoveStyleValue.h"
#include "GenericSpellActions.h"
#include "playerbot/PlayerbotFactory.h"

namespace ai
{
    class ReachTargetAction : public MovementAction, public Qualified
    {
    public:
        ReachTargetAction(PlayerbotAI* ai, std::string name, float range = 0.0f) : MovementAction(ai, name), Qualified(), range(range), spellName("") {}

        virtual void Qualify(const std::string& qualifier) override
        {
            Qualified::Qualify(qualifier);

            // Get the distance from the qualified spell given
            if (!qualifier.empty())
            {
                spellName = Qualified::getMultiQualifierStr(qualifier, 0, "::");

                float maxSpellRange;
                if (ai->GetSpellRange(spellName, &maxSpellRange))
                {
                    range = maxSpellRange;
                }
            }
        }

        virtual bool Execute(Event& event) override
		{
            Unit* target = GetTarget();
            if (target)
            {
                UpdateMovementState();

                // Ignore movement if too far
                const float distanceToTarget = bot->GetDistance(target, false, DIST_CALC_COMBAT_REACH);
                float chaseDist = range;
                const bool inLos = bot->IsWithinLOSInMap(target, true);
                const bool isFriend = sServerFacade.IsFriendlyTo(bot, target);

                if (range > 0.0f)
                {
                    // Move to 75% of max range so we land comfortably inside the
                    // range envelope rather than at its edge. This prevents constant
                    // fidgeting when the target drifts slightly out of max range, and
                    // stops bots from walking all the way out to the range limit before
                    // they cast. The 0.75 factor keeps a buffer for target movement
                    // while still being well within casting distance.
                    const float preferredDist = GetChaseDistance(range);

                    // NO LINE OF SIGHT MEANS CLOSE IN, WHOEVER THE TARGET IS.
                    //
                    // The step-closer rule was already here and applied to
                    // friends only, which left the common case broken: a caster
                    // already INSIDE its range with a pillar in the way asked to
                    // chase to 75% of that range, was closer than that already,
                    // and so moved nowhere at all. ChaseTo returns true having
                    // done nothing, the action sets no duration and keeps the
                    // Action default of 100ms, and the cast fails on line of
                    // sight a tenth of a second later. Forever.
                    //
                    // Measured 2026-09-07: "fireball 0 / reach spell 1
                    // (duration: 0.10s)" alternating, with the relevance bled
                    // down to -198 by the number of times round it had been.
                    //
                    // Ten percent closer per attempt converges on the target
                    // until the corner is cleared, and cannot overshoot into it
                    // because preferredDist still caps the approach whenever
                    // line of sight is fine. Being in range was never the
                    // question - a wall does not care how close you are, only
                    // where you are standing.
                    // HALF, not a tenth. Ten percent of the way is under three
                    // yards for a caster at range - too small to clear anything
                    // worth calling a corner, and it turned the loop into a
                    // crawl rather than ending it.
                    chaseDist = inLos ? preferredDist : std::min(distanceToTarget * 0.5f, preferredDist);
                    chaseDist = std::max(chaseDist - sPlayerbotAIConfig.contactDistance, 0.0f);
                }

                if (MoveStyleValue::WaitForEnemy(ai) && target->m_movementInfo.HasMovementFlag(movementFlagsMask) &&
                        sServerFacade.IsInFront(target, bot, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT) &&
                        sServerFacade.IsDistanceGreaterThan(distanceToTarget, sPlayerbotAIConfig.tooCloseDistance))
                {
                    ExplainReach(target, distanceToTarget, "waits for it to come");
                    return true;
                }

                if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
                {
                    std::ostringstream out;
                    out << "Moving to reach " << ChatHelper::formatWorldobject(target);
                    ai->TellPlayerNoFacing(GetMaster(), out);
                }
                
                const bool walkNear = inLos && isFriend && (range <= ai->GetRange("follow"));
                const bool moved = walkNear
                    ? MoveNear(target, chaseDist)
                    : ChaseTo(target, chaseDist, bot->GetAngle(target));
                ExplainReach(target, distanceToTarget, walkNear ? "move near" : (chaseHow.empty() ? "no chase" : chaseHow) +
                    (inLos ? "" : ", no line of sight"));

                // COMMIT TO THE MOVE WHEN THE PROBLEM IS A WALL.
                //
                // Every attempt at a spell with a cast time calls StopMoving on
                // its way past - PlayerbotAI::CastSpell does it unconditionally
                // for a moving caster - so the cast that is failing on line of
                // sight was also halting the walk meant to fix it. The bot
                // hopped a third of a second, was stopped, hopped again, and
                // the trace read "fireball 0 / reach spell 1" forever.
                //
                // The duration is what closes that, because SetActionDuration
                // pauses the whole bot: for as long as it lasts nothing tries
                // to cast, so the move actually completes. Only when out of
                // line of sight - a bot with a clear shot has no reason to stop
                // reacting for two seconds.
                //
                // AFTER the move, never before. MoveTo and ChaseTo both end in
                // WaitForReach, which calls SetDuration with the travel time -
                // anything set beforehand is overwritten and lost. That cost a
                // day on the tank's pull; it is not being paid twice.
                if (moved && !inLos)
                {
                    const uint32 losCommit = 2000;
                    if (GetDuration() < losCommit)
                    {
                        SetDuration(losCommit);
                    }
                }

                return moved;
            }

            return false;
        }

        // True for reach actions whose target is always meant to be attacked
        // (melee, pull) - false for "reach spell", which is also used to close
        // distance on friendly targets (e.g. Blessing of Protection/Freedom).
        virtual bool RequiresAttackableTarget() const { return false; }

        // How far in to actually walk, given the range the action needs. The
        // default keeps a 25% buffer so a drifting target does not push the bot
        // straight back out of range. ReachPullAction overrides it: a pull pays
        // for that buffer twice, once walking in and once walking back.
        virtual float GetChaseDistance(float chaseRange) const { return chaseRange * 0.75f; }

        virtual bool isUseful() override
		{
            // The pull's movement gate lives in MovementAction::isUseful, and
            // this override was skipping straight past it - so "reach melee",
            // "reach spell" and "reach pull" were the ONE family of movers
            // exempt from it, which is precisely the family that drags a tank
            // out of position.
            //
            // Measured 2026-09-07: mid-pull, "> reach melee @0.03" while the
            // state was Firing, walking the bot from 30 yards to 9 to 0 until
            // the opener could not be cast at all. It printed once because the
            // narration de-duplicates identical lines; it was running on every
            // tick.
            notKnown = false;
            if (!MovementAction::isUseful())
                return false;

            // Do not move if stay strategy is set
            if (!ai->HasStrategy("stay", ai->GetState()))
            {
                Unit* target = GetTarget();
                if (target)
                {
                    // A target that can never legally be attacked (friendly, wrong
                    // phase, etc.) isn't worth closing distance to - this is what let
                    // bots walk up to and cluster around friendly NPCs once a stale
                    // "current target"/"pull target" pointed at one (same check used
                    // in PossibleAttackTargetsValue::IsPossibleTarget).
                    if (RequiresAttackableTarget() && !bot->IsValidAttackTarget(target))
                    {
                        return false;
                    }

                    // Do not move while casting
                    if (!bot->IsNonMeleeSpellCasted(true, false, true))
                    {
                        // Check if the spell for which the reach action is used for can be casted
                        SpellCastResult result = SPELL_CAST_OK;
                        if (!spellName.empty() && !ai->CanCastSpell(spellName, target, true, nullptr, true, true, true, &result))
                        {
                            // A spell the bot does not know, or a target immune
                            // to it: no debug line, see CastSpellAction::isPossible.
                            notKnown = result == SPELL_FAILED_NOT_KNOWN || result == SPELL_FAILED_IMMUNE;
                            return false;
                        }

                        // Force move if not in los
                        if (bot->IsWithinLOSInMap(target, true))
                        {
                            // Check if the bot is already on the range required
                            return bot->GetDistance(target, true, DIST_CALC_COMBAT_REACH) > range;
                        }

                        return true;
                    }
                }
            }

            return false;
        }

        virtual std::string GetTargetName() override { return "current target"; }
        std::string GetSpellName() const { return spellName; }
        bool RefusedQuietly() const override { return notKnown; }

        virtual Unit* GetTarget() override
        {
            // Get the target from the qualifiers
            if (!qualifier.empty())
            {
                std::string targetQualifier;
                const std::string targetName = Qualified::getMultiQualifierStr(qualifier, 1, "::");
                if (targetName != "current target")
                {
                    targetQualifier = Qualified::getMultiQualifierStr(qualifier, 2, "::");
                }

                return targetQualifier.empty() ? AI_VALUE(Unit*, targetName) : AI_VALUE2(Unit*, targetName, targetQualifier);
            }
            else
            {
                return AI_VALUE(Unit*, GetTargetName());
            }
        }

    protected:
        // A REACH SAYS WHERE IT WENT (2026-10-28). "reach melee 1" means only
        // that a move started. A tank ran at a Defias Evoker for 8 s, never got
        // to it, and the log could not say what it ran at or how far it got.
        // With `debug` on, each reach writes its target, the gap to it, how far
        // the bot and the target moved since the last reach, and how ChaseTo
        // moved the bot.
        void ExplainReach(Unit* target, float gap, const std::string& how)
        {
            Player* master = GetMaster();
            if (!master || !ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
                return;

            const uint32 now = WorldTimer::getMSTime();
            const WorldPosition botNow(bot);
            const WorldPosition targetNow(target);

            char text[256];
            int used = snprintf(text, sizeof(text), "reach: %s %s gap %.1f yd", getName().c_str(), target->GetName(), gap);
            if (lastReachMs && lastReachMap == bot->GetMapId() && used > 0 && used < (int)sizeof(text))
            {
                used += snprintf(text + used, sizeof(text) - used, ", in %.1f s it ran %.1f yd",
                    WorldTimer::getMSTimeDiff(lastReachMs, now) / 1000.0f, botNow.distance(lastReachBot));
                if (lastReachTarget == target->GetObjectGuid() && used > 0 && used < (int)sizeof(text))
                    used += snprintf(text + used, sizeof(text) - used, ", the target %.1f yd", targetNow.distance(lastReachTargetPos));
            }

            ai->TellPlayerNoFacing(master, std::string(text) + ", " + how,
                PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);

            lastReachMs = now ? now : 1;
            lastReachMap = bot->GetMapId();
            lastReachBot = botNow;
            lastReachTarget = target->GetObjectGuid();
            lastReachTargetPos = targetNow;
        }

        float range;
        std::string spellName;
        bool notKnown = false;
        uint32 lastReachMs = 0;
        uint32 lastReachMap = 0;
        WorldPosition lastReachBot;
        ObjectGuid lastReachTarget;
        WorldPosition lastReachTargetPos;
    };

    class CastReachTargetSpellAction : public CastSpellAction
    {
    public:
        CastReachTargetSpellAction(PlayerbotAI* ai, std::string spell, float distance) : CastSpellAction(ai, spell), distance(distance) {}

		virtual bool isUseful() override
		{
            // Do not move if stay strategy is set
            if (ai->HasStrategy("stay", ai->GetState()))
                return false;

			return sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "current target"), (distance + sPlayerbotAIConfig.contactDistance));
		}

    protected:
        float distance;
    };

    class ReachMeleeAction : public ReachTargetAction
	{
    public:
        ReachMeleeAction(PlayerbotAI* ai) : ReachTargetAction(ai, "reach melee") {}
        bool RequiresAttackableTarget() const override { return true; }
    };

    class ReachSpellAction : public ReachTargetAction
	{
    public:
        ReachSpellAction(PlayerbotAI* ai) : ReachTargetAction(ai, "reach spell", ai->GetRange("spell")) {}
    };

    class ReachPullAction : public ReachTargetAction
    {
    public:
        ReachPullAction(PlayerbotAI* ai) : ReachTargetAction(ai, "reach pull")
        {
            PullStrategy* strategy = PullStrategy::Get(ai);
            if (strategy)
            {
                range = strategy->GetRange();
            }
        }

        void Qualify(const std::string& qualifier) override
        {
            ReachTargetAction::Qualify(qualifier);

            // Reduce the range slightly for a more accurate pull (moving targets can get out of reach).
            // Taken from the strategy so PullAction stops the approach at exactly
            // the distance this action stops walking to - see GetApproachRange.
            if (PullStrategy* strategy = PullStrategy::Get(ai))
            {
                range = strategy->GetApproachRange();
            }
        }

        // Stop at the range we need, not at 75% of it.
        //
        // The base class closes to range * 0.75 to keep a buffer against a
        // drifting target. That is right for a chase and wrong for a pull:
        // every yard closed here is a yard the tank has to walk back with the
        // mob on it, and the -5 above is already the buffer. For a 30 yd shot
        // the base behaviour walked to ~18 yd, so the pull travelled 12 yards
        // further in than it needed to and then had to undo all of it.
        float GetChaseDistance(float chaseRange) const override { return chaseRange; }

        std::string GetTargetName() override { return "pull target"; }
        bool RequiresAttackableTarget() const override { return true; }
    };

    class ReachPartyMemberToHealAction : public ReachTargetAction
    {
    public:
        ReachPartyMemberToHealAction(PlayerbotAI* ai) : ReachTargetAction(ai, "reach party member to heal", ai->GetRange("heal")) {}
        virtual std::string GetTargetName() override { return "party member to heal"; }
    };

    class ReachPartyMemberForTotemAction : public ReachTargetAction
    {
    public:
        ReachPartyMemberForTotemAction(PlayerbotAI* ai)
            : ReachTargetAction(ai, "reach party member for totem", 20.0f) {}

        void Qualify(const std::string& qualifier) override
        {
            ReachTargetAction::Qualify(qualifier);
            range = 20.0f;
        }

        Unit* GetTarget() override
        {
            std::string totemSpell = spellName;
            if (totemSpell.empty() && !qualifier.empty())
                totemSpell = Qualified::getMultiQualifierStr(qualifier, 0, "::");

            Group* group = bot->GetGroup();
            if (!group || totemSpell.empty())
                return AI_VALUE(Unit*, "master target");

            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (!member || !sServerFacade.IsAlive(member))
                    continue;

                if (member == bot)
                    continue;

                if (!member->IsInWorld() || member->GetMapId() != bot->GetMapId())
                    continue;

                if (!bot->IsWithinDistInMap(member, 100.0f, false))
                    continue;

                if (!ai->HasAura(totemSpell, member, false, true))
                    return member;
            }

            return nullptr;
        }
    };
}
