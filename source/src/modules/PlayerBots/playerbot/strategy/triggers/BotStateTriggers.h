#pragma once
#include "playerbot/strategy/Trigger.h"

namespace ai
{
    class CombatStartTrigger : public Trigger
    {
    public:
        CombatStartTrigger(PlayerbotAI* ai) : Trigger(ai, "combat start") {}
        virtual bool IsActive() override;
    };

    class CombatEndTrigger : public Trigger
    {
    public:
        CombatEndTrigger(PlayerbotAI* ai) : Trigger(ai, "combat end") {}
        virtual bool IsActive() override;
    };

    class DeathTrigger : public Trigger
    {
    public:
        DeathTrigger(PlayerbotAI* ai) : Trigger(ai, "death") {}
        virtual bool IsActive() override;
    };

    class ResurrectTrigger : public Trigger
    {
    public:
        ResurrectTrigger(PlayerbotAI* ai) : Trigger(ai, "resurrect") {}
        virtual bool IsActive() override;
    };

    // A dead companion whose party has been out of combat long enough to get
    // back up. Companions only - a random bot keeps the upstream corpse run.
    //
    // The clock is a member rather than a value because it has to be reset by
    // the fight, not by the tick that happens to read it: any party member in
    // combat clears it, so the wait always measures uninterrupted quiet. It
    // survives between ticks because the context caches one trigger per bot,
    // and the worst a rebuild can do is start the thirty seconds again.
    class CompanionReviveTrigger : public Trigger
    {
    public:
        CompanionReviveTrigger(PlayerbotAI* ai) : Trigger(ai, "companion revive") {}
        virtual bool IsActive() override;

        // Called by PlayerbotAI::OnDeath on each new death. The reset in
        // IsActive cannot do this job: only the dead engine runs this trigger,
        // and the reaction engine moves a revived bot out of it before it ticks
        // again. Not Reset(), which the engine calls on every tick.
        void Restart()
        {
            outOfCombatSince = 0;
            announcedWait = false;
        }

    private:
        bool PartyIsFighting();

        time_t outOfCombatSince = 0;

        // Said once per countdown, not once per death. Cleared with the clock,
        // so a fight that starts again announces the full wait afresh when it
        // ends rather than leaving the owner holding a number that stopped
        // being true the moment something else attacked.
        bool announcedWait = false;
    };
}