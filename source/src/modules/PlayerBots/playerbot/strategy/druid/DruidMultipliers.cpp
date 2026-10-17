
#include "playerbot/playerbot.h"
#include "DruidMultipliers.h"
#include "DruidActions.h"

using namespace ai;

// Below this mana a resto druid stops casting its offdps spells.
static const uint8 OFFDPS_MANA_FLOOR = 80;

// Only a resto druid has offdps. Its spells had no mana floor, so it cast
// them between heals until it had no mana left for the heals (bug report,
// 2026-10-17). A druid has no wand to fall back on, so below the floor it
// stands and heals. Heals and everything else keep their relevance.
float OffdpsManaFloorMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    const std::string name = action->getName();
    if (name != "wrath" && name != "moonfire" && name != "insect swarm" && name != "faerie fire")
        return 1.0f;

    if (AI_VALUE2(uint8, "mana", "self target") >= OFFDPS_MANA_FLOOR)
        return 1.0f;

    return 0.0f;
}
