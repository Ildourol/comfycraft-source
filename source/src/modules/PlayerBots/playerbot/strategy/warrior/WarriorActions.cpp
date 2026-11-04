
#include "playerbot/playerbot.h"
#include "WarriorActions.h"

using namespace ai;

namespace
{
    // Every rank of Battle Shout, 1.12 to 3.3.5. A party member's shout counts too.
    const uint32 battleShoutIds[] = { 6673, 5242, 6192, 11549, 11550, 11551, 25289, 2048, 47436 };

    bool LastsLongEnough(SpellAuraHolder const* holder)
    {
        return holder && (holder->IsPermanent() || holder->GetAuraDuration() > BATTLE_SHOUT_REFRESH_MS);
    }
}

bool ai::BattleShoutWanted(PlayerbotAI* ai)
{
    Player* bot = ai->GetBot();

    // The bot's own rank first. While that shout has time left, this is the
    // only aura lookup of the tick.
    const uint32 ownId = ai->GetAiObjectContext()->GetValue<uint32>("spell id", "battle shout")->Get();
    if (ownId && LastsLongEnough(bot->GetSpellAuraHolder(ownId)))
        return false;

    for (uint32 id : battleShoutIds)
    {
        if (id != ownId && LastsLongEnough(bot->GetSpellAuraHolder(id)))
            return false;
    }

    return true;
}
