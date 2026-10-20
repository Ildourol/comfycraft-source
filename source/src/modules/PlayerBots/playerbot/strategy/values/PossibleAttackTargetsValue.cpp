
#include "playerbot/playerbot.h"
#include "PossibleAttackTargetsValue.h"
#include "PossibleTargetsValue.h"

#include "playerbot/ServerFacade.h"
#include "Maps/GridNotifiers.h"
#include "Maps/GridNotifiersImpl.h"
#include "Maps/CellImpl.h"
#include "AttackersValue.h"
#include "EnemyPlayerValue.h"

using namespace ai;
using namespace MaNGOS;

std::list<ObjectGuid> PossibleAttackTargetsValue::Calculate()
{
    std::list<ObjectGuid> result;
    if (ai->AllowActivity(ALL_ACTIVITY))
    {
        if (bot->IsInWorld() && !bot->IsBeingTeleported())
        {
            // Check if we only need one possible attack target
            bool getOne = false;
            if (!qualifier.empty())
            {
                getOne = stoi(qualifier);
            }

            // One target is the first of the full list that is not under crowd
            // control. It was filtered from "attackers" with qualifier 1, a list
            // of one: when that one was a sheep, the last-resort bucket below
            // gave it back, and a dps broke the sheep with other mobs alive.
            result = AI_VALUE(std::list<ObjectGuid>, "attackers");
            RemoveNonThreating(result, getOne);
        }
    }

	return result;
}

void PossibleAttackTargetsValue::RemoveNonThreating(std::list<ObjectGuid>& targets, bool getOne)
{
    std::list<ObjectGuid> breakableCC;
    std::list<ObjectGuid> unBreakableCC;

    for(std::list<ObjectGuid>::iterator tIter = targets.begin(); tIter != targets.end();)
    {
        Unit* target = ai->GetUnit(*tIter);
        if (!IsValid(target, bot, sPlayerbotAIConfig.sightDistance, true, false))
        {
            std::list<ObjectGuid>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        // CONTROLLED MOBS GO IN A BUCKET, MARKED OR NOT.
        //
        // The buckets are a last resort: everything here is taken off the list,
        // and put back below only if nothing else survives. That is the right
        // shape and it took two goes to see it.
        //
        // First I dropped MARKED targets outright, so a deliberate cc mark could
        // never come back - which fixed a warrior sundering a sheep, and created
        // a worse deadlock. When the marked mob is the LAST one standing, the
        // list goes empty and the whole group waits out a fifty second sheep
        // with nothing else to do.
        //
        // Crowd control exists to reduce the number of enemies fighting you at
        // once. If it IS the only enemy there is nothing left to reduce, so the
        // mark has done its job and the right move is to kill it. The bucket
        // says both things at once: respected while anything else is worth
        // hitting, fair game when it is all that remains.
        else if (!HasIgnoreCCRti(target, bot) && HasBreakableCC(target, bot))
        {
            breakableCC.push_back(*tIter);

            std::list<ObjectGuid>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else if (!HasIgnoreCCRti(target, bot) && HasUnBreakableCC(target, bot))
        {
            unBreakableCC.push_back(*tIter);

            std::list<ObjectGuid>::iterator tIter2 = tIter;
            ++tIter;
            targets.erase(tIter2);
        }
        else
        {
            if (getOne)
            {
                // If the target is valid return it straight away
                std::list<ObjectGuid> result = { *tIter };
                targets = result;
                break;
            }
            else
            {
                ++tIter;
            }
        }
    }

    if (targets.empty())
    {
        if (!unBreakableCC.empty())
        {
            targets = unBreakableCC;
        }
        else if(!breakableCC.empty())
        {
            // THE PLAYER BREAKS THE LAST SHEEP (2026-10-20, the owner's call).
            //
            // With the other mobs dead, the mage broke her own sheep at once,
            // and the mob ran at her. A bot led by a real player now leaves a
            // sheep, sap or shackle alone: the player breaks it with the first
            // hit, or it wears off, and the mob is an ordinary target again.
            // It is not cast again on a last enemy (CcTargetValue), so this
            // cannot wait forever. Frozen and gouged mobs still come back: those
            // are short, and are cast mid-fight.
            if (ai->HasRealPlayerMaster())
            {
                breakableCC.remove_if([this](const ObjectGuid& guid)
                {
                    Unit* target = ai->GetUnit(guid);
                    return target && (target->IsPolymorphed() || ai->HasAura("sap", target) || ai->HasAura("shackle undead", target));
                });
            }

            targets = breakableCC;
        }
    }
}

bool PossibleAttackTargetsValue::HasIgnoreCCRti(Unit* target, Player* player)
{
    Group* group = player->GetGroup();
    return group && (group->GetTargetIcon(7) == target->GetObjectGuid());
}

bool PossibleAttackTargetsValue::HasBreakableCC(Unit* target, Player* player)
{
    if (target->IsPolymorphed())
    {
        return true;
    }

    if (sServerFacade.IsFrozen(target))
    {
        return true;
    }

    PlayerbotAI* ai = GetBotAI(player);
    if (ai)
    {
        if (ai->HasAura("sap", target))
        {
            return true;
        }

        if (ai->HasAura("gouge", target))
        {
            return true;
        }

        if (ai->HasAura("shackle undead", target))
        {
            return true;
        }
    }

    return false;
}

bool PossibleAttackTargetsValue::HasUnBreakableCC(Unit* target, Player* player)
{
    if (target->IsStunned())
    {
        return true;
    }

    if (sServerFacade.IsFeared(target))
    {
        return true;
    }

    if (sServerFacade.IsInRoots(target))
    {
        return true;
    }

    return false;
}

bool PossibleAttackTargetsValue::IsImmuneToDamage(Unit* target, Player* player)
{
    // Charmed
    if (sServerFacade.IsCharmed(target) && target->IsInTeam(player, true))
    {
        return true;
    }

    // Immune to damage
    PlayerbotAI* ai = GetBotAI(player);
    if (!ai)
        return false;

    for (const Aura* aura : ai->GetAuras(target))
    {
        const SpellEntry* spellInfo = aura->GetSpellProto();
        if (!spellInfo)
            continue;

        if (spellInfo->Mechanic == MECHANIC_BANISH)
            return true;

        if (!aura->IsPositive())
            continue;

        if (spellInfo->Mechanic == MECHANIC_INVULNERABILITY ||
            spellInfo->Mechanic == MECHANIC_IMMUNE_SHIELD)
        {
            return true;
        }
    }

    return false;
}

std::string PossibleAttackTargetsValue::Format()
{
    std::ostringstream out;

    for (auto& target : value)
    {
        if (target != value.front())
            out << ",";

        WorldObject* wo = ai->GetWorldObject(target);

        if (wo)
            out << wo->GetName();
        else
            out << target;
    }

    return out.str();
}

bool PossibleAttackTargetsValue::IsTapped(Unit* target, Player* player)
{
    if (player)
    {
        PlayerbotAI* ai = GetBotAI(player);

        if (ai && ai->HasAura("tame beast", target))
            return false;

        const Creature* creature = dynamic_cast<Creature*>(target);
        if (creature)
        {
            Unit* victim = creature->GetVictim();
            Player* master = ai ? ai->GetMaster() : nullptr;
            PlayerbotAI* ai = GetBotAI(player);

             if (!victim) //Target is not attacking anything.
                return true;

            if (master && victim == master) //Target is attacking master.
                return true;

            if (player->IsInGroup(victim)) //Target is attacking groupmember.
                return true;

            if (!creature->HasLootRecipient()) //Target is untapped.
                return true;

            if (creature->IsTappedBy(player)) //Target is tapped by player.
                return true;

            if (master && target->getThreatManager().getThreat(master)) //Master as threat
                return true;

            if (ai && ai->HasStrategy("attack tagged", BotState::BOT_STATE_NON_COMBAT)) //Can attack tagged.
                if(ai->HasActivePlayerMaster() || !player->GetGroup() || player->GetGroup()->IsLeader(player->GetObjectGuid())) //Playing with a player or not in a group or master of group.
                    return true;
        }
    }

    return false;
}

bool PossibleAttackTargetsValue::IsValid(Unit* target, Player* player, float range, bool ignoreCC, bool checkAttackerValid)
{
    // AN ORDERED TARGET NEED NOT BE FIGHTING YET (2026-10-19). `attack` sets the
    // bot's "attack target" to the owner's selection, and the line below that
    // accepts it was never reached: AttackersValue::IsValid wants the mob in
    // combat with the bot, which a mob only targeted is not. The next tick called
    // the target invalid, "select new target" dropped it ("charge no target") and
    // the companion never went. For that target the in-combat test is skipped;
    // friendly, evading and the other checks still apply.
    const bool ordered = target && GetBotAI(player) && target->GetObjectGuid() == PAI_VALUE(ObjectGuid, "attack target");

    // Check for the valid attackers value
    if (checkAttackerValid && !AttackersValue::IsValid(target, player, nullptr, !ordered))
    {
        return false;
    }

    if (!IsPossibleTarget(target, player, range, ignoreCC))
        return false;

    if (sServerFacade.GetThreatManager(target).getCurrentVictim())
        return true;

    if (target->GetGuidValue(UNIT_FIELD_TARGET))
        return true;

    if (target->GetObjectGuid().IsPlayer())
        return true;

    if (GetBotAI(player) && (target->GetObjectGuid() == PAI_VALUE(ObjectGuid, "attack target")))
        return true;

    // The master has started on it and nothing has hit back yet.
    if (GetBotAI(player) && AttackersValue::IsRealPlayerAttacking(target, GetBotAI(player)->GetMaster()))
        return true;

    if(!HasIgnoreCCRti(target, player) && (HasBreakableCC(target, player) || HasUnBreakableCC(target, player)))
        return true;

    if (GetBotAI(player) && !GetBotAI(player)->HasActivePlayerMaster()&& PAI_VALUE(Unit*, "rti target") == target)
        return true;

    return false;
}

bool PossibleAttackTargetsValue::IsPossibleTarget(Unit* target, Player* player, float range, bool ignoreCC)
{
    if(target)
    {
        // Defer to the same attackability check the real spell-cast validation uses
        // (Spell::CheckCast -> WorldObject::IsValidAttackTarget). This rejects targets
        // that have turned friendly (PvP flag drop, charm/MC ended, etc.) as well as
        // opposing-faction players who aren't actually PvP-flagged/in an FFA zone/duelling -
        // faction hostility alone isn't enough to legally attack another player, but the
        // bot target-selection logic only ever checked faction, so bots were picking
        // targets they could never actually hit and then never re-evaluating them.
        if (!player->IsValidAttackTarget(target))
        {
            return false;
        }

        // If the target is in an attackable distance
        if(!player->IsWithinDistInMap(target, range))
        {
            return false;
        }

        // If the target is immune to any damage
        if (IsImmuneToDamage(target, player))
        {
            return false;
        }

        // If the target is CC'ed
        if(!ignoreCC && !HasIgnoreCCRti(target, player) && (HasBreakableCC(target, player) || HasUnBreakableCC(target, player)))
        {
            return false;
        }

        // If the target is a NPC
        Player* enemyPlayer = dynamic_cast<Player*>(target);
        if (!enemyPlayer)
        {
            // If the target is not tapped (the player doesn't have the loot rights (gray name))
            if (!IsTapped(target, player))
            {
                return false;
            }
        }

        return true;
    }

    return false;
}

bool PossibleAddsValue::Calculate()
{
    PlayerbotAI *ai = GetBotAI(bot);
    std::list<ObjectGuid> possible = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible targets no los")->Get();
    std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();

    for (std::list<ObjectGuid>::iterator i = possible.begin(); i != possible.end(); ++i)
    {
        ObjectGuid guid = *i;
        if (find(attackers.begin(), attackers.end(), guid) != attackers.end()) continue;

        Unit* add = ai->GetUnit(guid);
        if (add && !add->GetGuidValue(UNIT_FIELD_TARGET) && !sServerFacade.GetThreatManager(add).getCurrentVictim() && sServerFacade.IsHostileTo(add, bot))
        {
            for (std::list<ObjectGuid>::iterator j = attackers.begin(); j != attackers.end(); ++j)
            {
                Unit* attacker = ai->GetUnit(*j);
                if (!attacker) continue;

                float dist = sServerFacade.GetDistance2d(attacker, add);
                if (sServerFacade.IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.aoeRadius * 1.5f)) continue;
                if (sServerFacade.IsDistanceLessOrEqualThan(dist, sPlayerbotAIConfig.aggroDistance)) return true;
            }
        }
    }
    return false;
}