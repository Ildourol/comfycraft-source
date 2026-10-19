#pragma once

namespace ai
{
    class UseFoodStrategy : public Strategy
    {
    public:
        UseFoodStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "food"; }

    private:
        void InitNonCombatTriggers(std::list<TriggerNode*> &triggers) override;
    };

    // A healer that sat down to drink stays down until somebody needs a heal.
    // True while it is seated with a drink, has mana to gain, is not being hit
    // and its master is inside the meal leash.
    bool HealerKeepsDrinking(PlayerbotAI* ai);

    // Any bot out of combat: seated with food or drink, with health or mana to
    // gain, not being hit and inside the meal leash. The non-combat engine then
    // runs no movement, so follow does not stand it up when its AI wakes.
    bool MealKeepsBotSeated(PlayerbotAI* ai);

    // What an engine may still run while HealerKeepsDrinking or MealKeepsBotSeated
    // is true.
    bool AllowedWhileHealerDrinks(Action* action);
}
