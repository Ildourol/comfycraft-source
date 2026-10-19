
#include "playerbot/playerbot.h"
#include "ChatShortcutActions.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/values/PositionValue.h"
#include "playerbot/strategy/values/Formations.h"
#include "playerbot/strategy/generic/PullStrategy.h"

using namespace ai;

void ReturnPositionResetAction::ResetPosition(std::string posName)
{
    ai::PositionMap& posMap = context->GetValue<ai::PositionMap&>("position")->Get();
    ai::PositionEntry pos = posMap[posName];
    pos.Reset();
    posMap[posName] = pos;
}

void ReturnPositionResetAction::SetPosition(WorldPosition wPos, std::string posName)
{
    ai::PositionMap& posMap = context->GetValue<ai::PositionMap&>("position")->Get();
    ai::PositionEntry pos = posMap[posName];
    pos.Set(wPos);
    posMap[posName] = pos;
}

void ReturnPositionResetAction::PrintStrategies(PlayerbotAI* ai, Event& event)
{
    if (event.getParam() == "?")
    {
        Player* requester = event.getOwner() ? event.getOwner() : ai->GetMaster();
        ai->PrintStrategies(requester, BotState::BOT_STATE_ALL);
    }
}

// Follow and flee move the bot only. A defensive pet stays on its target until the mob
// dies, so stop it and send it the pet bar's Follow. Its react state is kept: it still
// fights back when hit.
static void RecallPet(Player* bot)
{
    Pet* pet = bot->GetPet();
    if (!pet || !pet->AI())
        return;

    const uint32 command = (uint32(ACT_COMMAND) << 24) | COMMAND_FOLLOW;
    WorldPacket data(CMSG_PET_ACTION);
    data << pet->GetObjectGuid();
    data << command;
    data << ObjectGuid();
    bot->GetSession()->HandlePetAction(data);
}

bool FollowChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    // FOLLOW IS "HEEL", BRIEFLY - come back to me, hold fire for five seconds,
    // then carry on as normal.
    //
    // It used to mean "come with me and keep fighting", which left `flee` as a
    // second command differing by exactly one bit - flee is the same +follow
    // movement (GetDefaultMovementStrategy returns "follow" whenever there is a
    // player master) plus +passive. Merged on 2026-09-07.
    //
    // Then passive stayed on as a MODE until attack, pull, stay or guard lifted
    // it. That made a following companion useless for anything else: passive
    // zeroes looting, eating, buffs and fighting back, and the owner used
    // follow as the everyday "come here", not as "stand down for good". So
    // since 2026-09-17 follow's passive fades after five seconds
    // (PlayerbotAI::FadePassiveAfter, checked in UpdateAI), long enough to break
    // off and turn round. `flee` keeps the standing mode and cancels the fade,
    // which is what makes the two words mean different things again.
    //
    // An earlier leash lifted passive ON ARRIVAL and was removed because it
    // re-armed the bot at the moment you had asked it to stop. A timer from the
    // order is the owner's choice over that: predictable, and a bot still
    // running back from far away may start fighting on the way.
    //
    // Both engines, like flee does. Adding +follow to the non-combat engine
    // only is what made the old follow useless as a retreat: the moment
    // anything swung at the bot it switched engines, lost the follow strategy,
    // and stood there fighting instead of coming back.
    ai->Reset();
    ai->ChangeStrategy("+follow,+passive,-stay,-wander,", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+follow,+passive,-stay,-guard,-wander", BotState::BOT_STATE_COMBAT);
    ai->FadePassiveAfter(5000);
    ai->SetTightFollow(true);
    RecallPet(bot);

    // Coming back to the master abandons a pull, and it has to be said out
    // loud. Passive and PullMultiplier would otherwise gate each other into a
    // deadlock: the pull's actions are the only ones PullMultiplier allows, and
    // passive zeroes every one of them, so the bot would stand still holding a
    // pull nothing could advance until the 15s failsafe expired.
    //
    // Routed through "pull end" for the same reason FleeAction routes through
    // it - the pet's react state and the anchor need the real cleanup.
    if (PullStrategy* pull = PullStrategy::Get(ai))
    {
        if (pull->HasPullStarted())
        {
            ai->DoSpecificAction("pull end", event, true);
        }
    }

    ai::PositionMap& posMap = context->GetValue<ai::PositionMap&>("position")->Get();
    ai::PositionEntry pos = posMap["return"];
    pos.Reset();
    posMap["return"] = pos;

    ReturnPositionResetAction::PrintStrategies(ai, event);

    Formation* formation = AI_VALUE(Formation*, "formation");
    MEM_AI_VALUE(WorldPosition, "master position")->Reset();

    if (formation->getName() == "custom") //If in custom formation set relative position to current position.
    {
        ai::PositionEntry pos = posMap["follow"];

        WorldPosition relPos(bot);

        if (!ai->IsSafe(requester) || sServerFacade.GetDistance2d(bot, requester) > sPlayerbotAIConfig.reactDistance) //Use default formation location.
        {
            relPos = WorldPosition(bot->GetMapId(), cos(GetFollowAngle()) * ai->GetRange("follow"), sin(GetFollowAngle()) * ai->GetRange("follow"), 0);
        }
        else //Use relative location.
        {
            relPos -= WorldPosition(ai->GetMaster());
            relPos.rotateXY(-1 * ai->GetMaster()->GetOrientation());
        }

        pos.Set(relPos.getX(), relPos.getY(), relPos.getZ(), relPos.getMapId());
        posMap["follow"] = pos;
    }

    if (sServerFacade.IsInCombat(bot))
    {     
        WorldLocation loc = formation->GetLocation();
        if (Formation::IsNullLocation(loc) || loc.mapid == -1)
            return false;

        if (MoveTo(loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z, false, false))
        {
            ai->TellPlayerNoFacing(requester, BOT_TEXT("following"));
            return true;
        }
    }

    ai->TellPlayerNoFacing(requester, BOT_TEXT("following"));
    return true;
}

bool StayChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    // STAY IS "TAKE YOUR FORMATION SPOT AROUND ME, AND HOLD IT" (2026-10-19, the
    // owner's request). The spot is worked out once, from where the requester
    // stands and faces now, and stamped; `return to stay position` walks the bot
    // to it and back to it. Before, the stamp was wherever the bot stood.
    WorldPosition spot(bot);
    Formation* formation = AI_VALUE(Formation*, "formation");
    if (formation)
    {
        WorldLocation loc = formation->GetLocation();
        if (!Formation::IsNullLocation(loc) && loc.mapid == bot->GetMapId())
            spot = WorldPosition(loc);
    }

    ai->Reset();
    ai->ChangeStrategy("+stay,-follow,-wander,-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+stay,-follow,-wander,-passive", BotState::BOT_STATE_COMBAT);

    SetPosition(spot);
    SetPosition(spot, "stay");
    MEM_AI_VALUE(WorldPosition, "master position")->Reset();

    PrintStrategies(ai, event);

    ai->TellPlayerNoFacing(requester, BOT_TEXT("staying"));
    return true;
}

bool GuardChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+guard,-follow,-wander,-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+guard,-follow,-wander,-passive", BotState::BOT_STATE_COMBAT);

    SetPosition(bot);
    SetPosition(bot, "guard");
    MEM_AI_VALUE(WorldPosition, "master position")->Reset();  

    PrintStrategies(ai, event);

    ai->TellPlayerNoFacing(requester, BOT_TEXT("guarding"));
    return true;
}

bool FreeChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+free,-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+free,-passive", BotState::BOT_STATE_COMBAT);

    PrintStrategies(ai, event);

    ai->TellPlayerNoFacing(requester, BOT_TEXT("free_moving"));
    return true;
}

bool WanderChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+wander,-follow,-guard,-stay,-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("-follow,-guard,-stay,-passive", BotState::BOT_STATE_COMBAT);

    PrintStrategies(ai, event);

    ai->TellPlayerNoFacing(requester, BOT_TEXT("wandering"));
    return true;
}

bool FleeChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    // `flee` is follow that STAYS passive - see FollowChatShortcutAction, whose
    // passive fades after five seconds since 2026-09-17. Flee cancels that fade:
    // it holds fire until attack, pull, stay or guard lifts it.
    ai->Reset();
    std::string movement = ai->GetDefaultMovementStrategy();
    ai->ChangeStrategy("+" + movement + ",+passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+" + movement + ",+passive", BotState::BOT_STATE_COMBAT);
    ai->CancelPassiveFade(); // flee is the one that stays passive
    RecallPet(bot);
    ResetPosition();

    // Same deadlock as follow: passive and PullMultiplier gate each other, so
    // a pull left running would sit there until the failsafe.
    if (PullStrategy* pull = PullStrategy::Get(ai))
    {
        if (pull->HasPullStarted())
        {
            ai->DoSpecificAction("pull end", event, true);
        }
    }

    PrintStrategies(ai, event);

    if (bot->GetMapId() != requester->GetMapId() || sServerFacade.GetDistance2d(bot, requester) > sPlayerbotAIConfig.sightDistance)
    {
        ai->TellPlayerNoFacing(requester, BOT_TEXT("fleeing_far"));
        return true;
    }
    ai->TellPlayerNoFacing(requester, BOT_TEXT("fleeing"));
    return true;
}

bool GoawayChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+runaway", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("+runaway", BotState::BOT_STATE_COMBAT);
    ResetPosition();

    PrintStrategies(ai, event);

    ai->TellPlayerNoFacing(requester, "Running away");
    return true;
}

bool GrindChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    ai->Reset();
    ai->ChangeStrategy("+grind,-passive", BotState::BOT_STATE_NON_COMBAT);
    ResetPosition();
    ai->TellPlayerNoFacing(requester, BOT_TEXT("grinding"));
    return true;
}

bool TankAttackChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    if (!ai->IsTank(bot))
        return false;

    ai->Reset();
    ai->ChangeStrategy("-passive", BotState::BOT_STATE_NON_COMBAT);
    ai->ChangeStrategy("-passive", BotState::BOT_STATE_COMBAT);
    ResetPosition();
    ai->TellPlayerNoFacing(requester, BOT_TEXT("attacking"));
    return true;
}

bool MaxDpsChatShortcutAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    if (!requester)
        return false;

    if (!ai->ContainsStrategy(STRATEGY_TYPE_DPS))
        return false;

    ai->Reset();
    ai->ChangeStrategy("-threat,-conserve mana,-cast time,+dps debuff,+boost", BotState::BOT_STATE_COMBAT);
    ai->TellPlayerNoFacing(requester, "Max DPS!");
    return true;
}
