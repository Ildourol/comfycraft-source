
#include "playerbot/playerbot.h"
#include "LootTriggers.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/PlayerbotAIConfig.h"

#include "playerbot/ServerFacade.h"
#include "Group/Group.h"
using namespace ai;

// No loot while the bot, its master or a group member near it is in combat. The
// bot's own engine is not enough: it leaves combat when its attackers list is
// empty, and a mob fighting the player can be off that list. A companion then
// walked to a corpse and pulled the next pack (2026-10-18). Meals use it too.
bool ai::GroupInCombat(PlayerbotAI* ai, Player* bot)
{
    if (sServerFacade.IsInCombat(bot))
        return true;

    Player* master = ai->GetMaster();
    if (master && master != bot && master->IsInWorld() && master->GetMapId() == bot->GetMapId() && sServerFacade.IsInCombat(master))
        return true;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == bot || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            continue;

        if (sServerFacade.IsInCombat(member) && sServerFacade.GetDistance2d(bot, member) < sPlayerbotAIConfig.sightDistance)
            return true;
    }

    return false;
}

bool LootAvailableTrigger::IsActive()
{
    if (GroupInCombat(ai, bot))
        return false;

    return AI_VALUE(bool, "has available loot") &&
            (
                    sServerFacade.IsDistanceLessOrEqualThan(AI_VALUE2(float, "distance", "loot target"), INTERACTION_DISTANCE) ||
                    AI_VALUE(std::list<ObjectGuid>, "all targets").empty()
            ) &&
            !AI_VALUE2(bool, "combat", "self target") &&
            !AI_VALUE2(bool, "mounted", "self target");
}

bool FarFromCurrentLootTrigger::IsActive()
{
    if (GroupInCombat(ai, bot))
        return false;

    LootObject loot = AI_VALUE(LootObject, "loot target");

    if (!loot.IsLootPossible(bot))
        return false;

    // Abandon loot the bot cannot safely reach without breaking its follow leash.
    // "move to loot" runs at priority 7 but "out of free move range" fires "follow" at
    // ACTION_HIGH (20). A corpse is only reachable without oscillation if the master is
    // within followDistance + GetMaxLootDistance of it — otherwise the bot must leave the
    // leash to reach the corpse and follow immediately wins, causing a yo-yo.
    Player* master = ai->GetMaster();
    if (master && master != bot)
    {
        Creature* creature = ai->GetCreature(loot.guid);
        if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
        {
            float safeRange = sPlayerbotAIConfig.followDistance + bot->GetMaxLootDistance(creature);
            if (sServerFacade.GetDistance2d(master, creature) > safeRange)
                return false;
        }
    }

    // Must agree with OpenLootAction::DoLoot's loot-range rule, or the bot deadlocks: for a
    // creature corpse the server validates loot with a 3D distance check (Player::SendLoot),
    // but the plain "distance" value here is 2D and ignores Z. On sloped ground the bot read
    // "close enough" (2D <= 5) and so never fired "move to loot", yet "open loot" failed the
    // 3D gate -> it sat a few yards off a corpse it could not loot. Use the same 3D rule so
    // the bot keeps approaching until it is genuinely on the corpse, then loots.
    if (Creature* creature = GetBotAI(bot)->GetCreature(loot.guid))
    {
        if (sServerFacade.GetDeathState(creature) == CORPSE)
            return !creature->IsWithinDistInMap(bot, bot->GetMaxLootDistance(creature), true, SizeFactor::None);
    }

    return AI_VALUE2(float, "distance", "loot target") > INTERACTION_DISTANCE;
}

bool CanLootTrigger::IsActive()
{
    if (GroupInCombat(ai, bot))
        return false;

    return AI_VALUE(bool, "can loot");
}
