#pragma once
#include "DruidStrategy.h"
#include "DruidTriggers.h"
#include "playerbot/strategy/triggers/GenericTriggers.h"

namespace ai
{
    class LevelingDruidPlaceholderStrategy : public SpecPlaceholderStrategy
    {
    public:
        LevelingDruidPlaceholderStrategy(PlayerbotAI* ai) : SpecPlaceholderStrategy(ai) {}
        int GetType() override { return STRATEGY_TYPE_DPS; }
        std::string getName() override { return "leveling"; }
    };

    // Rejuvenation not applied on self AND health not full - matches "cast Rejuvenation if
    // health is not full and Rejuvenation is not applied".
    class LevelingRejuvenationTrigger : public BuffTrigger
    {
    public:
        LevelingRejuvenationTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "rejuvenation") {}
        bool IsActive() override
        {
            return BuffTrigger::IsActive() && AI_VALUE2(uint8, "health", "self target") < 100;
        }
    };

    // Moonfire not applied on target (reuses MoonfireTrigger's existing check) AND mana is
    // still above the reserve threshold - stops recasting Moonfire once mana runs low so
    // there's still enough left for Healing Touch/Rejuvenation when actually needed.
    class LevelingMoonfireTrigger : public MoonfireTrigger
    {
    public:
        LevelingMoonfireTrigger(PlayerbotAI* ai) : MoonfireTrigger(ai) {}
        bool IsActive() override
        {
            return MoonfireTrigger::IsActive() && AI_VALUE2(uint8, "mana", "self target") >= 50;
        }
    };

    // A target exists AND mana is above the heal reserve. Wrath is the main damage at any
    // range; below the reserve the bot melees and keeps its mana for a heal.
    class LevelingWrathTrigger : public Trigger
    {
    public:
        LevelingWrathTrigger(PlayerbotAI* ai) : Trigger(ai, "leveling wrath") {}
        bool IsActive() override
        {
            return AI_VALUE(Unit*, "current target") && AI_VALUE2(uint8, "mana", "self target") >= 30;
        }
    };

    // Pre-level-10 Druid: no shapeshift forms exist yet (Bear at 10, Cat at 20), so this is
    // not a Feral strategy. It casts Wrath at any range while mana is above the heal reserve,
    // keeps Moonfire up above 50% mana, self-heals (Healing Touch/Rejuvenation) as needed,
    // and melees only when mana is below the reserve.
    class LevelingDruidStrategy : public DruidStrategy
    {
    public:
        LevelingDruidStrategy(PlayerbotAI* ai) : DruidStrategy(ai) {}

    protected:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;

        NextAction** GetDefaultCombatActions() override;
    };

    class LevelingDruidPveStrategy : public LevelingDruidStrategy
    {
    public:
        LevelingDruidPveStrategy(PlayerbotAI* ai) : LevelingDruidStrategy(ai) {}
        std::string getName() override { return "leveling pve"; }

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    class LevelingDruidPvpStrategy : public LevelingDruidStrategy
    {
    public:
        LevelingDruidPvpStrategy(PlayerbotAI* ai) : LevelingDruidStrategy(ai) {}
        std::string getName() override { return "leveling pvp"; }

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    class LevelingDruidRaidStrategy : public LevelingDruidStrategy
    {
    public:
        LevelingDruidRaidStrategy(PlayerbotAI* ai) : LevelingDruidStrategy(ai) {}
        std::string getName() override { return "leveling raid"; }

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };
}
