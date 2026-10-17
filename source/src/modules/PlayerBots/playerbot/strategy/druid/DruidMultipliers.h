#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    // A resto druid's offdps spells only while its mana is high. See the
    // implementation for why.
    class OffdpsManaFloorMultiplier : public Multiplier
    {
    public:
        OffdpsManaFloorMultiplier(PlayerbotAI* ai) : Multiplier(ai, "offdps mana floor") {}
        float GetValue(Action* action) override;
    };
}
