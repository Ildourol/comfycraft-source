
#include "playerbot/playerbot.h"
#include "AttackAction.h"
#include "Movement/MovementGenerator.h"
#include "AI/CreatureAI.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/BotDiagnostics.h" // SC_LOG for attack-command diagnostic trace
#include "playerbot/strategy/generic/CombatStrategy.h"
#include "playerbot/strategy/values/CcTargetValue.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "playerbot/CompanionOwnership.h"
#include "playerbot/strategy/generic/UseFoodStrategy.h"
#include "playerbot/strategy/values/AttackersValue.h"

using namespace ai;

void ai::LogTargetSwitch(PlayerbotAI* botAi, Unit* oldTarget, Unit* newTarget, const std::string& by)
{
    if (!botAi || oldTarget == newTarget || !botAi->HasRealPlayerMaster() || !sPlayerbotAIConfig.hasLog("target_switch.csv"))
        return;

    Player* master = botAi->GetMaster();
    auto describe = [&](std::ostringstream& out, Unit* u)
    {
        if (!u)
        {
            out << ",,,,";
            return;
        }

        out << ",\"" << u->GetName() << "\"," << u->GetGUIDLow() << "," << u->GetHealth() << ",";
        out << (!u->IsAlive() ? "dead" : AttackersValue::IsOnlyStartedOn(u, master) ? "started" : u->IsInCombat() ? "combat" : "idle");
    };

    // time,ms,bot,by,old name,old guid,old hp,old state,new name,new guid,new hp,new state
    std::ostringstream out;
    out << sPlayerbotAIConfig.GetTimestampStr() << "," << WorldTimer::getMSTime() << ",";
    out << botAi->GetBot()->GetName() << "," << by;
    describe(out, oldTarget);
    describe(out, newTarget);
    sPlayerbotAIConfig.log("target_switch.csv", out.str().c_str());
}

void ai::EngageOnOrder(PlayerbotAI* botAi)
{
    if (!botAi)
        return;

    botAi->ChangeStrategy("-passive", BotState::BOT_STATE_NON_COMBAT);
    botAi->ChangeStrategy("-passive", BotState::BOT_STATE_COMBAT);
    botAi->ChangeStrategy("-follow", BotState::BOT_STATE_COMBAT);

    // EXCEPT A HEALER WITH A DRINK, which keeps it. The healer has nothing to
    // walk to on a pull: the anchor is the player, and the healer is sitting
    // next to the player. It also stays in the non-combat engine, where its
    // meal sleep holds; the `combat start` trigger moves it to the combat
    // engine when the fight reaches the group, and there it gets up to heal.
    if (HealerKeepsDrinking(botAi))
        return;

    // AND UP OFF THE FLOOR.
    //
    // Eating and drinking are out-of-combat behaviours, and out of combat is
    // precisely where a pull gets set up - so the order lands on a group that
    // is sitting down. The aura has to go with the stand state: standing alone
    // leaves the bot fed-but-seated on the next tick, because the drink aura is
    // what the sitting is expressing.
    Player* bot = botAi->GetBot();
    if (bot && !bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    }

    botAi->RemoveAura("drink");
    botAi->RemoveAura("food");

    // AND INTO THE COMBAT ENGINE, which is where fighting is even described.
    //
    // The engine is chosen by state, and a bot that has been told to attack
    // something it is not yet fighting is still in the NON-COMBAT one - where
    // there is no "reach melee", no "reach spell" and no attack rotation,
    // because none of that belongs out of combat. bot->Attack() sets a victim
    // and starts auto-attack, but auto-attack needs to already be in range, so
    // out of range nothing ever closes it and nothing ever puts the bot in
    // combat: a dps with a target, standing still, forever.
    //
    // OnCombatStarted is what calls ChangeEngine(BOT_STATE_COMBAT). The pull
    // order has always done this - it is the reason a pull worked from a
    // standing start and an attack did not - and doing it here gives the other
    // two orders the same footing. It self-guards on IsStateActive, so calling
    // it when already fighting costs nothing.
    botAi->OnCombatStarted();

    // ...and act on it now rather than after whatever delay the previous action
    // left behind, which is the difference between responding to a button and
    // responding a second and a half later.
    botAi->ReactImmediately();
}

bool AttackAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    Unit* target = GetTarget();
    if (target && target->IsInWorld() && target->GetMapId() == bot->GetMapId())
    {
        return Attack(requester, target);
    }

    return false;
}

bool AttackMyTargetAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    SC_LOG("attack-cmd entry bot=%s requester=%s eventOwner=%s",
           bot ? bot->GetName() : "(null)",
           requester ? requester->GetName() : "(null)",
           event.getOwner() ? event.getOwner()->GetName() : "(null)");

    // Same as the pull order: the cc-capable bot is the one whose answer the
    // player cannot otherwise see.
    ReportCcIntent(ai, requester);

    if(requester)
    {
        const ObjectGuid guid = requester->GetSelectionGuid();
        SC_LOG("attack-cmd selection bot=%s requester=%s selGuid=0x%016llx",
               bot ? bot->GetName() : "(null)",
               requester->GetName(),
               (unsigned long long)guid.GetRawValue());

        if (guid)
        {
            Unit* tgt = ai->GetUnit(guid);
            SC_LOG("attack-cmd target bot=%s tgt=%s tgtMap=%d botMap=%u",
                   bot ? bot->GetName() : "(null)",
                   tgt ? tgt->GetName() : "(null-unit)",
                   tgt ? (int)tgt->GetMapId() : -1,
                   bot ? bot->GetMapId() : 0);

            // BEFORE the attack, not after it. See EngageOnOrder.
            EngageOnOrder(ai);

            // ATTACK OVERRIDES A PULL. They are two different instructions, and
            // the newer one wins: "never mind the careful pull, go hit that".
            //
            // It also releases the rest of the group. The hold in AttackersValue
            // is keyed on any group tank having a pull started, so leaving the
            // tank's pull running while ordering an attack would have the dps
            // still waiting for a pull nobody intends to finish.
            if (PullStrategy* pull = PullStrategy::Get(ai))
            {
                if (pull->HasPullStarted())
                {
                    ai->DoSpecificAction("pull end", event, true);
                }
            }

            if (Attack(requester, tgt))
            {
                SET_AI_VALUE(ObjectGuid, "attack target", guid);
                SC_LOG("attack-cmd OK bot=%s tgt=%s",
                       bot ? bot->GetName() : "(null)",
                       tgt ? tgt->GetName() : "(null)");
                return true;
            }
            SC_LOG("attack-cmd FAIL bot=%s — Attack() returned false", bot ? bot->GetName() : "(null)");
        }
        else if (verbose)
        {
            SC_LOG("attack-cmd FAIL bot=%s — requester has no selection", bot ? bot->GetName() : "(null)");
            ai->TellError(requester, "You have no target");
        }
    }
    else
    {
        SC_LOG("attack-cmd FAIL bot=%s — no requester (event has no owner, no master)", bot ? bot->GetName() : "(null)");
    }

    return false;
}

bool AttackRTITargetAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Unit* rtiTarget = AI_VALUE(Unit*, "rti target");

    if (rtiTarget && rtiTarget->IsInWorld() && rtiTarget->GetMapId() == bot->GetMapId())
    {
        EngageOnOrder(ai);

        if (Attack(requester, rtiTarget))
        {
            SET_AI_VALUE(ObjectGuid, "attack target", rtiTarget->GetObjectGuid());
            return true;
        }
    }
    else
    {
        ai->TellError(requester, "I dont see my rti attack target");
    }

    return false;
}

bool AttackMyTargetAction::isUseful()
{
    return !ai->ContainsStrategy(STRATEGY_TYPE_HEAL) || ai->HasStrategy("offdps", BotState::BOT_STATE_COMBAT);
}

bool AttackRTITargetAction::isUseful()
{
    return !ai->ContainsStrategy(STRATEGY_TYPE_HEAL) || ai->HasStrategy("offdps", BotState::BOT_STATE_COMBAT);
}

bool AttackAction::Attack(Player* requester, Unit* target)
{
    MotionMaster &mm = *bot->GetMotionMaster();
	if (mm.GetCurrentMovementGeneratorType() == TAXI_MOTION_TYPE || (bot->IsFlying() && WorldPosition(bot).currentHeight() > 10.0f))
    {
        SC_LOG("attack-cmd FAIL bot=%s — taxi/flying", bot ? bot->GetName() : "(null)");
        if (verbose)
        {
            ai->TellPlayerNoFacing(requester, "I cannot attack in flight");
        }

        return false;
    }

    if (IsTargetValid(requester, target))
    {
        SC_LOG("attack-cmd valid-tgt bot=%s tgt=%s mounted=%d range=%.1f",
               bot ? bot->GetName() : "(null)",
               target ? target->GetName() : "(null)",
               bot ? (int)bot->IsMounted() : -1,
               target ? sServerFacade.GetDistance2d(bot, target) : -1.0f);
        if (bot->IsMounted() && (sServerFacade.GetDistance2d(bot, target) < 40.0f || bot->IsFlying()))
        {
            ai->Unmount();
            
            if (bot->IsFlying())
            {
                return true;
            }
        }

        ObjectGuid guid = target->GetObjectGuid();
        bot->SetSelectionGuid(target->GetObjectGuid());

        Unit* oldTarget = AI_VALUE(Unit*, "current target");
        if(oldTarget)
        {
            SET_AI_VALUE(Unit*, "old target", oldTarget);
        }

        LogTargetSwitch(ai, oldTarget, target, getName());

        SET_AI_VALUE(Unit*, "current target", target);
        AI_VALUE(LootObjectStack*, "available loot")->Add(guid);

        const bool isWaitingForAttack = WaitForAttackStrategy::ShouldWait(ai);
        Pet* pet = bot->GetPet();
        if (pet)
        {
            UnitAI* creatureAI = ((Creature*)pet)->AI();
            if (creatureAI)
            {
                const bool sendPet = !isWaitingForAttack && (!ai->HasStrategy("stay", BotState::BOT_STATE_COMBAT) || AI_VALUE2(float, "distance", "current target") < ai->GetRange("spell"));

                // A companion's pet is always passive (PlayerbotAI::DoNextAction) and
                // goes in only on this order, the pet bar's Attack. A defensive pet
                // took the player's pull on its way in (2026-10-18).
                if (sendPet && CompanionOwnership::IsCompanion(bot->GetGUIDLow()))
                {
                    WorldPacket data(CMSG_PET_ACTION);
                    data << pet->GetObjectGuid();
                    data << uint32((uint32(ACT_COMMAND) << 24) | COMMAND_ATTACK);
                    data << target->GetObjectGuid();
                    bot->GetSession()->HandlePetAction(data);
                }
                // Don't send the pet to attack if the bot is waiting for attack
                else if (sendPet)
                {
                    // Reset the pet state if no master
                    if (creatureAI->GetReactState() == REACT_PASSIVE && !ai->GetMaster())
                    {
                        creatureAI->SetReactState(REACT_DEFENSIVE);
                    }

                    // Don't send the pet to attack if set to passive
                    if (creatureAI->GetReactState() != REACT_PASSIVE)
                    {
                        creatureAI->AttackStart(target);
                    }
                }
            }
        }

        if (ai->CanMove() && !sServerFacade.IsInFront(bot, target, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
        {
            sServerFacade.SetFacingTo(bot, target);
        }

        bool result = true;

        // A healer in a player's group takes the target for its wand and nothing
        // more. Auto-attack drew the weapon and played the attack emote with no
        // swing to follow, since a healer does not melee (MeleeAction).
        const bool healerTargetOnly = PlayerbotAI::IsHeal(bot) && ai->HasRealPlayerMaster();

        // Don't attack target if it is waiting for attack or in stealth
        if (!healerTargetOnly && !ai->HasStrategy("stealthed", BotState::BOT_STATE_COMBAT) && !isWaitingForAttack)
        {
            ai->PlayAttackEmote(1);
            result = bot->Attack(target, !ai->IsRanged(bot) || (sServerFacade.GetDistance2d(bot, target) < 5.0f));
            SC_LOG("attack-cmd bot->Attack bot=%s tgt=%s result=%d",
                   bot ? bot->GetName() : "(null)",
                   target ? target->GetName() : "(null)",
                   (int)result);
        }
        else
        {
            SC_LOG("attack-cmd skip bot->Attack bot=%s — stealthed=%d isWaitingForAttack=%d",
                   bot ? bot->GetName() : "(null)",
                   (int)ai->HasStrategy("stealthed", BotState::BOT_STATE_COMBAT),
                   (int)isWaitingForAttack);
        }

        if (result)
        {
            // Force change combat state to have a faster reaction time
            ai->OnCombatStarted();
        }

        return result;
    }

    SC_LOG("attack-cmd FAIL bot=%s — IsTargetValid rejected", bot ? bot->GetName() : "(null)");
    return false;
}

bool AttackAction::IsTargetValid(Player* requester, Unit* target)
{
    if (!target)
    {
        if (verbose) 
        {
            ai->TellPlayerNoFacing(requester, "I have no target");
        }

        return false;
    }
    else if (sServerFacade.IsFriendlyTo(bot, target))
    {
        if (verbose)
        {
            std::ostringstream msg;
            msg << target->GetName();
            msg << " is friendly to me";
            ai->TellPlayerNoFacing(requester, msg.str());
        }

        return false;
    }
    else if (sServerFacade.UnitIsDead(target))
    {
        if (verbose)
        {
            std::ostringstream msg;
            msg << target->GetName();
            msg << " is dead";
            ai->TellPlayerNoFacing(requester, msg.str());
        }

        return false;
    }
    else if (sServerFacade.GetDistance2d(bot, target) > sPlayerbotAIConfig.sightDistance)
    {
        if (verbose)
        {
            std::ostringstream msg;
            msg << target->GetName();
            msg << " is too far away";
            ai->TellPlayerNoFacing(requester, msg.str());
        }

        return false;
    }

    return true;
}

bool AttackDuelOpponentAction::isUseful()
{
    return AI_VALUE(Unit*, "duel target");
}

bool AttackDuelOpponentAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    return Attack(requester, AI_VALUE(Unit*, "duel target"));
}
