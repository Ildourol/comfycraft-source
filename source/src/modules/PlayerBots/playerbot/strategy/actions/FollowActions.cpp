
#include "playerbot/playerbot.h"
#include "FollowActions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/Formations.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/FleeManager.h"
#include "playerbot/CompanionOwnership.h"
#include "Maps/PathFinder.h"
#include <iomanip>
#include <sstream>

using namespace ai;

// LOOSE FOLLOW IN A DUNGEON (2026-10-19, the owner's request).
//
// Formation follow kept a companion about 3 yd from its owner and chased that
// spot on every step, which put it in the owner's pulls. In a dungeon a
// companion now stands still while its owner is within LooseFollowDistance (10,
// `.bot reload` after a change), at the same height and in sight, and walks
// toward the owner only when it is more than 4 yd past that, more than 4 yd
// above or below, or out of sight. The Follow order is formation follow until
// the companion arrives; then it is loose again.
static bool LooseFollow(PlayerbotAI* ai, Unit* followTarget)
{
    Player* bot = ai->GetBot();
    return followTarget && followTarget == ai->GetMaster() && !ai->IsTightFollow() &&
        bot->GetMap()->IsDungeon() && CompanionOwnership::IsCompanion(bot->GetGUIDLow());
}

static bool LooseTooFar(Player* bot, Unit* followTarget)
{
    return sServerFacade.GetDistance2d(bot, followTarget) > sPlayerbotAIConfig.looseFollowDistance + 4.0f ||
        std::fabs(bot->GetPositionZ() - followTarget->GetPositionZ()) > 4.0f ||
        !bot->IsWithinLOSInMap(followTarget, true);
}

bool FollowAction::Execute(Event& event)
{
    bool moved = false;
    Unit* followTarget = AI_VALUE(Unit*, "follow target");
    Formation* formation = AI_VALUE(Formation*, "formation");

    if (LooseFollow(ai, followTarget))
    {
        if (!LooseTooFar(bot, followTarget))
            return false;

        // Too far on the flat: stop a little inside the distance. Above, below or out
        // of sight: come right up, or the same spot would be out of reach again.
        const bool flatOnly = std::fabs(bot->GetPositionZ() - followTarget->GetPositionZ()) <= 4.0f &&
            bot->IsWithinLOSInMap(followTarget, true);
        return MoveNear(followTarget, flatOnly ? std::max(sPlayerbotAIConfig.looseFollowDistance - 2.0f, sPlayerbotAIConfig.followDistance)
                                               : sPlayerbotAIConfig.followDistance);
    }

    // The Follow order ends when the companion has arrived.
    if (followTarget && ai->IsTightFollow() && sServerFacade.GetDistance2d(bot, followTarget) <= sPlayerbotAIConfig.followDistance + 3.0f)
        ai->SetTightFollow(false);

    if (ai->IsSafe(followTarget))
    {
        if (formation)
        {
            WorldLocation loc = formation->GetLocation();
            if (!Formation::IsNullLocation(loc) && loc.mapid != -1)
            {
                moved = Follow(followTarget, formation->GetOffset(), formation->GetAngle());
            }
        }
    }
    else
        moved = Follow(followTarget, 0, 0);

    return moved;
}

bool FollowAction::isUseful()
{
    if (!ai->CanMove())
        return false;

    // Loose follow and close enough: no move. A formation follow left running from
    // before (login, the Follow order) is stopped, or it would keep the old spot.
    {
        Unit* followTarget = AI_VALUE(Unit*, "follow target");
        if (LooseFollow(ai, followTarget) && !LooseTooFar(bot, followTarget))
        {
            if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
                ai->StopMoving();

            return false;
        }
    }

    float distance = 0;
    Unit* followTarget = AI_VALUE(Unit*, "follow target");
    Formation* formation = AI_VALUE(Formation*, "formation");

    if (followTarget && followTarget->IsPlayer())
    {
        if (AI_VALUE(GuidPosition, "rpg target") && CanFreeMoveValue::CanFreeMoveTo(ai, AI_VALUE(GuidPosition, "rpg target")))
        {
            return false;
        }
    }

    if (followTarget)
    {
        if (followTarget->IsTaxiFlying() || !CanDeadFollow(followTarget) || followTarget->GetGUIDLow() == bot->GetGUIDLow())
        {
            return false;
        }
    }

    if (followTarget)
    {
        distance = sServerFacade.GetDistance2d(bot, followTarget);
    }
    else
    {
        WorldLocation loc = formation->GetLocation();
        if (Formation::IsNullLocation(loc) || bot->GetMapId() != loc.mapid)
        {
            return false;
        }

        distance = sServerFacade.GetDistance2d(bot, loc.coord_x, loc.coord_y);
    }

    if (sServerFacade.IsDistanceGreaterThan(distance, sPlayerbotAIConfig.sightDistance))
    {
        return true;
    }

    // maybe jump is faster?
    if (ai->HasStrategy("follow jump", BotState::BOT_STATE_NON_COMBAT) && ai->AllowActivity())
    {
        return true;
    }

    // Already following at the formation's spot (MovementAction::IsFollowing).
    if (followTarget && formation && MovementAction::IsFollowing(bot, followTarget, formation->GetOffset(), formation->GetAngle()))
    {
        return false;
    }

    return true;
}

bool FollowAction::CanDeadFollow(Unit* target)
{
    //Move to corpse when dead and player is alive or not a ghost.
    if (!sServerFacade.IsAlive(bot) && (sServerFacade.IsAlive(target) || !target->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST)))
    {
        return false;
    }
    
    return true;
}

bool StopFollowAction::isUseful()
{
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        return false;

    if (sServerFacade.GetChaseTarget(bot) && !sServerFacade.GetChaseTarget(bot)->IsPlayer() && sServerFacade.GetChaseTarget(bot)->IsMoving())
        return false;

    return true;
}

bool FleeToMasterAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Unit* fTarget = AI_VALUE(Unit*, "master target");
    bool canFollow = Follow(fTarget);
    if (!canFollow)
    {
        //SetDuration(5000);
        return false;
    }

    WorldPosition targetPos(fTarget);
    WorldPosition bosPos(bot);
    float distance = bosPos.fDist(targetPos);


    uint32 scale = 5;
    if (bot->GetGroup() && bot->GetGroup()->GetMembersCount() > 5)
        scale = bot->GetGroup()->GetMembersCount();
        

    if (distance > sPlayerbotAIConfig.reactDistance && bot->IsInCombat())
    {
        if (!urand(0, scale))
            ai->TellPlayerNoFacing(requester, "I'm heading to your location but I'm in combat", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            //ai->TellPlayer(BOT_TEXT("wait_travel_combat"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }
    else if (distance < sPlayerbotAIConfig.reactDistance * 3)
    {
        if (!urand(0, scale))
            ai->TellPlayerNoFacing(requester, BOT_TEXT("wait_travel_close"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }
    else if (distance < 1000)
    {
        if (!urand(0, scale*4))
            ai->TellPlayerNoFacing(requester, BOT_TEXT("wait_travel_medium"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }
    else
        if (!urand(0, scale*6))
            ai->TellPlayerNoFacing(requester, BOT_TEXT("wait_travel_medium"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
           
    SetDuration(3000U);
    return true;
}

bool FleeToMasterAction::isUseful()
{
    if (!ai->CanMove())
        return false;

    if (!ai->GetGroupMaster())
        return false;

    if (ai->GetGroupMaster() == bot)
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");

    if (target && ai->GetGroupMaster()->HasTarget(target->GetObjectGuid()))
        return false;

    if (!(ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
      ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
        return false;

    Unit* fTarget = AI_VALUE(Unit*, "master target");
    
    if (!CanDeadFollow(fTarget))
        return false;

    if (fTarget && fTarget->IsPlayer())
    {
        if (AI_VALUE(GuidPosition, "rpg target") && CanFreeMoveValue::CanFreeMoveTo(ai, AI_VALUE(GuidPosition, "rpg target")))
            return false;
    }

    LootObject loot = AI_VALUE(LootObject, "loot target");
    if (loot.IsLootPossible(bot))
        return false;

    return true;
}

// AWAY FROM A PATROL, THROUGH THE MASTER (2026-10-19, the owner's sketch).
//
// A flee point or a stand-off spot could lie in the path of a patrol walking in,
// and a priest that fled ahead of the group was picked up by one. A ranged bot
// or a healer takes the nearest spot on the ring around its master that is clear
// of the patrol (FindMasterRingSpot). A melee bot, or one with no spot, runs to
// the master.
bool AvoidPatrolAction::Execute(Event& event)
{
    Player* master = GetMaster();
    if (!master)
        return false;

    const bool explain = ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT);

    if (ai->IsRanged(bot) || ai->IsHeal(bot))
    {
        float rx, ry, rz;
        std::string note;
        if (FindMasterRingSpot(ai, rx, ry, rz, note))
        {
            if (explain)
                ai->TellPlayerNoFacing(master, "patrol: to a spot " + note);

            return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);
        }
    }

    if (explain)
        ai->TellPlayerNoFacing(master, "patrol: to " + std::string(master->GetName()));

    return MoveNear(master, sPlayerbotAIConfig.followDistance);
}
