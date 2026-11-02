
#include "playerbot/playerbot.h"
#include "AttackerWithoutAuraTargetValue.h"
#include "playerbot/PlayerbotAIConfig.h"

using namespace ai;

Unit* AttackerWithoutAuraTargetValue::Calculate()
{
    std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();
    Unit* target = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    // An attacker immune to the spell never gets the aura. Skip it, or it is
    // picked on every tick and no other attacker gets the debuff (bug #128).
    const uint32 spellId = ai->GetAiObjectContext()->GetValue<uint32>("spell id", qualifier)->Get();
    for (std::list<ObjectGuid>::iterator i = attackers.begin(); i != attackers.end(); ++i)
    {
        Unit* unit = ai->GetUnit(*i);
        if (!unit || unit == target)
            continue;

        if (bot->GetDistance(unit) > ai->GetRange("spell"))
            continue;

        if (spellId && ai->IsTargetImmuneToSpell(spellId, unit))
            continue;

        if (!ai->HasAura(qualifier, unit))
            return unit;
    }

    return NULL;
}
