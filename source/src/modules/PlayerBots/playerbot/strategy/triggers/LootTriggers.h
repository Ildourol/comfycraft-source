#pragma once
#include "playerbot/strategy/Trigger.h"
#include "playerbot/strategy/values/LastMovementValue.h"

namespace ai
{
    // The bot, its master or a group member in sight range has the game's combat flag.
    bool GroupInCombat(PlayerbotAI* ai, Player* bot);

    class LootAvailableTrigger : public Trigger
    {
    public:
        LootAvailableTrigger(PlayerbotAI* ai) : Trigger(ai, "loot available") {}

        virtual bool IsActive() override;
    };

    class FarFromCurrentLootTrigger : public Trigger
    {
    public:
        FarFromCurrentLootTrigger(PlayerbotAI* ai) : Trigger(ai, "far from current loot") {}

        virtual bool IsActive() override;
    };

    class CanLootTrigger : public Trigger
    {
    public:
        CanLootTrigger(PlayerbotAI* ai) : Trigger(ai, "can loot") {}

        virtual bool IsActive() override;
    };
}
