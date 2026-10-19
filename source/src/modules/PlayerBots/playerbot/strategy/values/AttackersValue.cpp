
#include "playerbot/playerbot.h"
#include "AttackersValue.h"
#include <iomanip>
#include <sstream>
#include "PossibleTargetsValue.h"
#include "EnemyPlayerValue.h"
#include "playerbot/strategy/generic/PullStrategy.h"

using namespace ai;
using namespace MaNGOS;

std::list<ObjectGuid> AttackersValue::Calculate()
{
    std::list<ObjectGuid> result;
    if (!ai->AllowActivity(ALL_ACTIVITY))
        return result;

    if (!bot->IsInWorld())
        return result;

    if (bot->IsBeingTeleported())
        return result;

    if (bot->IsFlying() && WorldPosition(bot).currentHeight() > 10.0f)
        return result;

    // lost control, e.g. BG ended
    if (bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CLIENT_CONTROL_LOST))
        return result;

    // HOLD WHILE THE TANK IS PULLING.
    //
    // "Wait for the pull" is enforced here, at the list of things worth
    // attacking, rather than by suppressing actions later. A bot with nothing
    // to attack holds by construction - there is no state to advance, no
    // relevance number to lose an argument with, and no way to be half held.
    // Everything the pull work this week taught: enforce it structurally, and
    // the periodic checks have nothing left to disagree about.
    //
    // Self-defence survives it. Only the tank's pull is being protected, so
    // anything ALREADY swinging at this bot stays on the list - holding still
    // while something eats you is not discipline, it is a bug. An add that
    // wanders in is fought; the mob the tank is walking back is not touched.
    //
    // Healers are unaffected either way: heal target selection does not come
    // through here, so "everyone holds except the healer keeps healing" needs
    // no special case.
    const bool holdingForPull = PullStrategy::IsGroupPullRunning(ai);

    // Say it, once each way. A dps standing still on purpose and a dps standing
    // still by accident look identical from outside, and telling them apart by
    // watching was costing a test run each time. Reported from the value that
    // actually enforces the hold, so it cannot claim one thing and do another.
    if (holdingForPull != wasHoldingForPull)
    {
        wasHoldingForPull = holdingForPull;

        if (Player* master = ai->GetMaster())
        {
            ai->TellPlayerNoFacing(master, holdingForPull
                ? "holding for the pull"
                : "pull's done - going in");
        }
    }

    if (holdingForPull)
    {
        std::list<ObjectGuid> defending;
        for (Unit* attacker : bot->getAttackers())
        {
            if (attacker && attacker->IsAlive())
            {
                defending.push_back(attacker->GetObjectGuid());
            }
        }

        return defending;
    }

    if (ai->HasStrategy("focus rti targets", BotState::BOT_STATE_COMBAT))
    {
        Unit* rtiTarget = AI_VALUE(Unit*, "rti target");

        // A MARK IS A CHOICE BETWEEN FIGHTS, NOT A FIGHT OF ITS OWN.
        //
        // The skull only narrows the list once its mob is actually in combat.
        // Without that, a skull on anything alive in sight range made it an
        // attacker, so "has attackers" stayed true and the bot never left its
        // combat engine: pull, then `follow`, and the companion stood in combat
        // over a mob nobody was fighting, and once follow's passive faded it
        // walked straight back to it. An untouched skull now falls through to
        // normal targeting, which only lists what is fighting the group. The
        // pull is unaffected: the other bots hold above, and by the time the
        // pull ends the skull is in combat.
        if (rtiTarget && rtiTarget->IsInWorld() && rtiTarget->GetMapId() == bot->GetMapId() && rtiTarget->IsInCombat())
        {
            // The cc mark deliberately does NOT need to be kept here. It briefly
            // was, because cc target selection used to be filtered out of this
            // list and collapsing to the skull turned crowd control off. That
            // dependency is gone: CcTargetValue resolves "rti cc target"
            // directly, so no narrowing of this attack list can switch cc off
            // any more - which is the point of having moved it.
            return { rtiTarget->GetObjectGuid() };
        }

        // NO MARK MEANS NORMAL TARGETING, NOT NO TARGETING.
        //
        // This used to `return result` here - an empty list - so a group with
        // the strategy on and nothing marked had bots that would not attack
        // anything at all. That made "focus the mark" unusable as a default,
        // which is why it was off everywhere and marking a skull did nothing.
        // Falling through instead makes the strategy mean what its name says:
        // when something IS marked it is the only target, and otherwise the
        // bot picks as it always did.
    }

    if (sPlayerbotAIConfig.shareTargets)
    {
        // Try to get the value from nearby friendly bots.
        std::list<ObjectGuid> nearGuids = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("nearest friendly players")->Get();
        for (auto& i : nearGuids)
        {
            Player* player = sObjectMgr.GetPlayer(i);

            if (!player)
                continue;

            if (player == bot)
                continue;

            if (!ai->IsSafe(player))
                continue;

            if (sServerFacade.GetDistance2d(bot, player) > 10.0f)
                continue;

            PlayerbotAI* botAi = GetBotAI(player);

            if (!botAi)
                continue;

            std::string valueName = "attackers" + !qualifier.empty() ? "::" + qualifier : "";

            // Ignore bots without the value.
            if (!PHAS_AI_VALUE(valueName))
                continue;

            UntypedValue* pValue = botAi->GetAiObjectContext()->GetUntypedValue(valueName);

            AttackersValue* pAttackersValue = dynamic_cast<AttackersValue*>(pValue);

            if (!pAttackersValue)
                continue;

            // Ignore expired values.
            if (pAttackersValue->Expired())
                continue;

            if (pAttackersValue->calculatePos.sqDistance2d(bot) > 100.0f)
                continue;

            // Make the value expire at the same time as the copied value.
            lastCheckTime = pAttackersValue->lastCheckTime;

            calculatePos = pAttackersValue->calculatePos;

            result = PAI_VALUE(std::list<ObjectGuid>, valueName);

            std::vector<std::string> specificTargetNames = { "current target","old target","attack target","pull target" };
            Unit* target;

            //Remove bot specific targets of the other bot.
            for (auto& targetName : specificTargetNames)
            {
                target = (targetName == "attack target") ? ai->GetUnit(PAI_VALUE(ObjectGuid, targetName)) : PAI_VALUE(Unit*, targetName);
                if (target)
                    result.remove(target->GetObjectGuid());
            }

            //Add bot specific targets of this bot.
            for (auto& targetName : specificTargetNames)
            {
                target = (targetName == "attack target") ? ai->GetUnit(AI_VALUE(ObjectGuid, targetName)) : AI_VALUE(Unit*, targetName);
                if (target)
                    result.push_back(target->GetObjectGuid());
            }

            //Validate these targets.
            std::list<ObjectGuid> filter;

            for (auto& guid : result)
            {
                target = ai->GetUnit(guid);

                if (!IsValid(target, bot, bot))
                    filter.push_back(guid);
            }

            for(auto& guid : filter)
                result.remove(guid);

            HoldApproaching(result);
            return result;
        }
    }
    
    calculatePos = bot;

    std::set<Unit*> targets;

    // Check if we only need one attacker
    bool getOne = false;
    if (!qualifier.empty())
    {
        getOne = stoi(qualifier);
    }

    std::set<ObjectGuid> invalidTargets;

    // Add the targets of the bot
    AddTargetsOf(bot, targets, invalidTargets, getOne);

    // Don't check for group member targets if we only need one
    if (targets.empty() || !getOne)
    {
        // Add the targets of the members of the group
        Group* group = bot->GetGroup();
        if (group && !bot->InBattleGround())
        {
            AddTargetsOf(group, targets, invalidTargets, getOne);
        }

        Player* master = GetMaster();
        if (master && !bot->InBattleGround() && (!group || master->GetGroup() != group))
            AddTargetsOf(master, targets, invalidTargets, getOne);
    }

    // Convert the targets to guids
    for (Unit* target : targets)
    {
        result.push_back(target->GetObjectGuid());
    }

    HoldApproaching(result);
    return result;
}

// HOLD FIRE WHILE THE PULL COMES IN (2026-10-18, the owner's request).
//
// The player pulls with a shot and the mob runs at them. It is in combat from
// the first hit, so it was on this list at once, and a dps companion hit it on
// the way in and took it off the tank. The tank pull hold above covers only a
// pull made by a companion tank.
//
// So a mob running at anyone in the group, and not yet in melee with them, is
// left off until it arrives. Anyone, not only the player: a Renew on the tank
// during the pull turns the mob onto the priest, and that must not release it
// early. A mob running at a companion is held the same way, so self-defence also
// waits for arrival.
//
// A mob that reaches the player is not released on arrival: the hunter's pet was
// on it before the player's first swing. It is released by the player's melee
// attack on it (a tank), or APPROACH_GRACE_MS after it arrived (a ranged dps that
// never swings). A caster that stops at range is the same once the player walks
// up to it.
//
// There is no time limit. An 8 s one released a slow mob on the way in. While a
// mob is held, the bot's list can be empty: it stays in the non-combat engine,
// follows the player and heals from there. Loot and meals check the group's
// combat flag (GroupInCombat), so it does not loot or sit down.
//
// Only for a bot led by a real player. Random bots and bot-led groups keep the
// old behaviour.
static const uint32 APPROACH_GRACE_MS = 1500;

void AttackersValue::HoldApproaching(std::list<ObjectGuid>& result)
{
    holdingApproach = false;

    // ONE HOLD STATE FOR THE BOT (2026-10-19). "attackers" and "attackers" with
    // qualifier 1 (the dps target's list) are two instances, and each kept its
    // own: the first released a mob when the player hit it, the second first saw
    // it a moment later, out of the player's melee reach, and held it. A mage
    // never joined. The unqualified instance owns the state.
    AttackersValue* owner = this;
    if (!qualifier.empty())
        if (AttackersValue* full = dynamic_cast<AttackersValue*>(context->GetValue<std::list<ObjectGuid>>("attackers")))
            owner = full;
    std::map<ObjectGuid, Approach>& approaching = owner->approaching;

    if (!ai->HasRealPlayerMaster())
    {
        approaching.clear();
        return;
    }

    Player* master = ai->GetMaster();
    const uint32 now = sWorld.GetCurrentMSTime();
    std::map<ObjectGuid, Approach> seen;

    for (auto it = result.begin(); it != result.end();)
    {
        const ObjectGuid guid = *it;
        Unit* unit = ai->GetUnit(guid);

        auto known = approaching.find(guid);
        Approach state = known != approaching.end() ? known->second : Approach();
        const bool released = !state.held;

        if (released || !unit || unit->IsPlayer() || !sServerFacade.IsAlive(unit))
        {
            if (released)
                seen[guid] = state;

            ++it;
            continue;
        }

        // No victim yet counts as coming in: the player's shot puts the mob on
        // this list while it is still in flight, before the mob picks anyone.
        Unit* victim = unit->GetVictim();
        Player* victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        const bool atGroup = victimPlayer &&
            (victimPlayer == bot || victimPlayer == master || bot->IsInSameGroupWith(victimPlayer));

        // Fighting somebody outside the group: none of this pull's business.
        if (victim && !atGroup)
        {
            ++it;
            continue;
        }

        // With `debug` on, the hold says what it does with each mob: once when it
        // starts holding, once when it lets go, and why.
        const bool explain = ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT) && master;
        auto where = [&](Unit* other) -> std::string
        {
            std::ostringstream out;
            out << std::fixed << std::setprecision(1) << sServerFacade.GetDistance2d(unit, other) << " yd from " << other->GetName();
            return out.str();
        };

        // At a companion (or a pet): it defends itself at once.
        bool release = victim && victim != master && unit->CanReachWithMeleeAttack(victim);
        std::string reason = release ? "reached " + where(victim) : "";

        // At the player, or the player went up to it.
        if (!release && master && sServerFacade.IsAlive(master) && unit->CanReachWithMeleeAttack(master))
        {
            if (master->GetVictim() == unit)
            {
                release = true;
                reason = "you attack it, " + where(master);
            }
            else if (!state.arrivedMs)
                state.arrivedMs = now ? now : 1;
            else if (now - state.arrivedMs >= APPROACH_GRACE_MS)
            {
                release = true;
                reason = "1.5 s at you, " + where(master);
            }
        }

        if (!release)
        {
            if (explain && known == approaching.end())
                ai->TellPlayerNoFacing(master, "hold: holding " + std::string(unit->GetName()) + ", " + where(master) +
                    (victim ? ", on " + std::string(victim->GetName()) : ", no target yet"));

            holdingApproach = true;
            seen[guid] = state;
            it = result.erase(it);
            continue;
        }

        if (explain)
            ai->TellPlayerNoFacing(master, "hold: released " + std::string(unit->GetName()) + ", " + reason +
                (known == approaching.end() ? " (never held)" : ""));

        // Fought from now on.
        state.held = false;
        seen[guid] = state;
        ++it;
    }

    // Keep only what is still on the list, so the map cannot grow. Only the full
    // list prunes: the qualified one holds a single target.
    if (owner == this)
        approaching.swap(seen);
    else
        for (const auto& entry : seen)
            approaching[entry.first] = entry.second;
}

void AttackersValue::AddTargetsOf(Group* group, std::set<Unit*>& targets, std::set<ObjectGuid>& invalidTargets, bool getOne)
{
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        // Only add group member targets that are alive and near the player
        Player* member = sObjectMgr.GetPlayer(itr->guid);
        if (member && (member != bot) &&
           sServerFacade.IsAlive(member) &&
           member->IsInWorld() &&
           !member->IsBeingTeleported() &&
           (member->GetMapId() == bot->GetMapId()) && 
           (sServerFacade.GetDistance2d(bot, member) <= GetRange()))
        {
            AddTargetsOf(member, targets, invalidTargets, getOne);

            // Finish early if we only need one target
            if (getOne && !targets.empty())
            {
                break;
            }
        }
    }
}

void AttackersValue::AddTargetsOf(Player* player, std::set<Unit*>& targets, std::set<ObjectGuid>& invalidTargets, bool getOne)
{
    // If the player is available
    if (ai->IsSafe(player))
    {
        std::set<Unit*> units;

        // If the player is a bot
        PlayerbotAI* playerBot = GetBotAI(player);
        if (playerBot)
        {
            // Get all the units around the player
            // NOTE: We don't validate the value here because it will be validated later on
            const std::string ignoreValidate = std::to_string(true);
            const std::string range = std::to_string((int32)GetRange());
            const std::vector<std::string> qualifiers = { range, ignoreValidate };
            const std::list<ObjectGuid> possibleTargets = PAI_VALUE2(std::list<ObjectGuid>, "possible targets", Qualified::MultiQualify(qualifiers, ":"));
            for (const ObjectGuid& guid : possibleTargets)
            {
                if (Unit* unit = ai->GetUnit(guid))
                {
                    units.insert(unit);
                }
            }

            // Add the current target
            Unit* currentTarget = PAI_VALUE(Unit*, "current target");
            if (currentTarget)
            {
                units.insert(currentTarget);
            }

            // Add the previous target
            Unit* oldTarget = PAI_VALUE(Unit*, "old target");
            if (oldTarget)
            {
                units.insert(oldTarget);
            }

            // Add the pull and attack targets (Only consider the owner bot)
            if (bot == player)
            {
                Unit* attackTarget = playerBot->GetUnit(PAI_VALUE(ObjectGuid, "attack target"));
                if (attackTarget)
                {
                    units.insert(attackTarget);
                }

                Unit* pullTarget = PAI_VALUE(Unit*, "pull target");
                if (pullTarget)
                {
                    units.insert(pullTarget);
                }
            }
        }

        // Get the current attackers of the player
        for (Unit* attacker : player->getAttackers())
        {
            units.insert(attacker);
        }

        // WHAT A REAL PLAYER HAS STARTED ON, BEFORE IT HITS BACK.
        //
        // Without this the group joined only once the mob was fighting someone:
        // threat from a landed hit, or the mob swinging. A caster's opener is
        // two or three seconds of cast and travel before that, and the
        // companions stood watching. InCombat accepts these targets through
        // IsRealPlayerAttacking. The pull hold above returns before this point,
        // so a running pull still holds.
        if (!playerBot)
        {
            // Auto attack sets the victim at any range, so a target with auto
            // attack on from thirty yards away is not a fight yet. It counts
            // once the swing can land.
            Unit* victim = player->GetVictim();
            if (victim && player->CanReachWithMeleeAutoAttack(victim))
                units.insert(victim);

            // By guid, not getUnitTarget(): the spell keeps that pointer until
            // the player's own next update, and a target deleted in between
            // (a totem or an add a companion killed mid-cast) leaves it freed.
            for (uint32 i = CURRENT_FIRST_NON_MELEE_SPELL; i < CURRENT_MAX_SPELL; ++i)
            {
                Spell* spell = player->GetCurrentSpell(CurrentSpellTypes(i));
                if (!spell)
                    continue;
                if (Unit* target = ai->GetUnit(spell->m_targets.getUnitTargetGuid()))
                    units.insert(target);
            }
        }

        // Add the duel opponent (Only consider the owner bot)
        if (bot == player && bot->m_duel && bot->m_duel->opponent)
        {
            // Penqle's DuelInfo::opponent is an ObjectGuid; resolve to Unit*.
            if (Unit* opp = ObjectAccessor::GetUnit(*bot, bot->m_duel->opponent))
                units.insert(opp);
        }

        // Add the pet attackers (if nearby)
        Pet* pet = player->GetPet();
        if (pet && sServerFacade.GetDistance2d(bot, pet) <= GetRange())
        {
            for (Unit* attacker : pet->getAttackers())
            {
                units.insert(attacker);
            }
        }

        // AN ORDERED TARGET DOES NOT HAVE TO BE IN SIGHT.
        //
        // PossibleTargetsValue::IsValid drops anything the bot cannot currently
        // see (IsVisibleForOrDetect), and it is right to: a bot should not pick
        // fights through walls. But the mob the PLAYER named is not something
        // the bot picked - the player can see it, and saying "attack" is the
        // authority for knowing where it is.
        //
        // Without this, "attack" on anything round a corner or past visibility
        // range was accepted, set as the current target, and then quietly
        // dropped from the attackers list on the next tick, so the dps stood
        // there. Reported 2026-09-07 as "they aren't coming to help if the
        // target is behind the corner or out of range" - one gate, both halves.
        //
        // Only the two targets the player actually named, and only for the bot
        // that was told. Everything else still has to be seen to be fought.
        ObjectGuid orderedAttack, orderedPull;
        if (bot == player)
        {
            orderedAttack = AI_VALUE(ObjectGuid, "attack target");
            if (Unit* pullTarget = AI_VALUE(Unit*, "pull target"))
            {
                orderedPull = pullTarget->GetObjectGuid();
            }
        }

        // Filter the units that are valid
        for (Unit* unit : units)
        {
            if (!ai->IsSafe(unit))
                continue;

            const bool ordered = (orderedAttack && unit->GetObjectGuid() == orderedAttack)
                              || (orderedPull && unit->GetObjectGuid() == orderedPull);

            // Prevent checking a target that has already been validated
            if((targets.find(unit) == targets.end()))
            {
                // Prevent checking a target that has already been invalidated
                if(InCombat(unit, player) || (invalidTargets.find(unit->GetObjectGuid()) == invalidTargets.end()))
                {
                    if (IsValid(unit, player, bot, true, true, ordered))
                    {
                        // Add the target to the list of combat targets
                        targets.insert(unit);

                        // Add the target's pet/guardian too
                        unit->CallForAllControlledUnits(AddGuardiansHelper(targets), CONTROLLED_PET | CONTROLLED_GUARDIANS | CONTROLLED_CHARM | CONTROLLED_MINIPET | CONTROLLED_TOTEMS);

                        if (getOne)
                        {
                            break;
                        }
                    }
                    else
                    {
                        invalidTargets.insert(unit->GetObjectGuid());
                    }
                }
            }
        }
    }
}

bool AttackersValue::InCombat(Unit* target, Player* player, bool checkPullTargets)
{
    // Check if the the target is attacking the player
    bool inCombat = (target->getThreatManager().getThreat(player) > 0.0f) ||
                    (target->GetVictim() && (target->GetVictim() == player));

    // Check if the target is attacking the player's pet
    if(!inCombat)
    {
        Pet* pet = player->GetPet();
        if (pet)
        {
            inCombat = (target->getThreatManager().getThreat(pet) > 0.0f) ||
                       (target->GetVictim() && (target->GetVictim() == pet));
        }
    }

    if(!inCombat && checkPullTargets && GetBotAI(player))
    {
        inCombat = (PAI_VALUE(ObjectGuid, "attack target") == target->GetObjectGuid()) ||
                   (PAI_VALUE(Unit*, "pull target") == target);
    }

    if (!inCombat)
    {
        inCombat = IsRealPlayerAttacking(target, player);
    }

    return inCombat;
}

bool AttackersValue::IsRealPlayerAttacking(Unit* target, Player* player)
{
    if (!target || !player || GetBotAI(player))
        return false;

    if (player->GetVictim() == target && player->CanReachWithMeleeAutoAttack(target))
        return true;

    for (uint32 i = CURRENT_FIRST_NON_MELEE_SPELL; i < CURRENT_MAX_SPELL; ++i)
    {
        Spell* spell = player->GetCurrentSpell(CurrentSpellTypes(i));
        if (!spell || spell->m_targets.getUnitTarget() != target)
            continue;

        // A sheep or a sap is a decision NOT to fight that mob yet. IsCCSpell
        // leaves sap out on purpose (its cast must not be delayed), so it is
        // named here.
        const SpellEntry* spellInfo = spell->m_spellInfo;
        if (spellInfo && (spellInfo->IsCCSpell() || spellInfo->IsFitToFamily<SPELLFAMILY_ROGUE, CF_ROGUE_SAP>()))
            continue;

        return true;
    }

    return false;
}

bool AttackersValue::IsOnlyStartedOn(Unit* target, Player* player)
{
    return target && !target->IsInCombat() && IsRealPlayerAttacking(target, player);
}

Unit* AttackersValue::HeldTarget(PlayerbotAI* ai)
{
    AiObjectContext* context = ai->GetAiObjectContext();

    Unit* current = AI_VALUE(Unit*, "current target");
    if (!current || !sServerFacade.IsAlive(current) || !current->IsInCombat())
        return nullptr;

    std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
    if (std::find(attackers.begin(), attackers.end(), current->GetObjectGuid()) == attackers.end())
        return nullptr;

    return current;
}

bool AttackersValue::IsValid(Unit* target, Player* player, Player* owner, bool checkInCombat, bool validatePossibleTarget, bool ignoreLos)
{
    Player* playerToCheckAgainst = owner != nullptr ? owner : player;

    // Validate possible target
    if (validatePossibleTarget && !PossibleTargetsValue::IsValid(target, playerToCheckAgainst, ignoreLos))
    {
        return false;
    }

    // This will be used on both enemy player and npc checks
    const bool inPvPProhibitedZone = sPlayerbotAIConfig.IsInPvpProhibitedZone(sServerFacade.GetAreaId(target));

    // If the target is a player
    Player* enemyPlayer = dynamic_cast<Player*>(target);
    if (enemyPlayer)
    {
        // Don't consider enemy players if pvp strategy is not set
        if (GetBotAI(playerToCheckAgainst) && !GetBotAI(playerToCheckAgainst)->HasStrategy("pvp", BotState::BOT_STATE_COMBAT))
        {
            return false;
        }

        // If the enemy player is in a PVP Prohibited zone
        if (inPvPProhibitedZone)
        {
            return false;
        }

        const bool isDuelOpponent = player->m_duel && player->m_duel->opponent == target->GetObjectGuid();

        // Don't check distance on duel opponents
        if (!isDuelOpponent)
        {
            // If the enemy player is not within sight distance
            if (!enemyPlayer->IsWithinDist(playerToCheckAgainst, EnemyPlayerValue::GetMaxAttackDistance(playerToCheckAgainst), false))
            {
                return false;
            }
        }

        // Unlike the NPC branch below, this had no actual-combat check at all: a stale
        // "current target"/"old target" reference to a player who stopped fighting long ago
        // (or was never fighting to begin with) stayed "valid" forever just by being nearby
        // and PvP-flagged, keeping "has attackers" true and the bot latched into combat
        // state with nothing to do (see AttackersValue::AddTargetsOf and InCombat()).
        if (checkInCombat && !isDuelOpponent && !InCombat(target, player, (player == owner)))
        {
            bool isRtiTarget = false;
            if (GetBotAI(player) && !GetBotAI(player)->HasActivePlayerMaster())
            {
                Unit* rtiTarget = PAI_VALUE(Unit*, "rti target");
                if (target == rtiTarget)
                    isRtiTarget = true;
            }

            if (!isRtiTarget)
                return false;
        }
    }
    // If the target is a NPC
    else
    {
        // Some callers (GrindTargetValue::FindTargetForGrinding, PullRequestAction)
        // pass validatePossibleTarget=false to reuse this function purely for its
        // combat/evade checks, which skips the friendliness gate above entirely.
        // Re-check here so a friendly NPC referenced by a stale "current target"/
        // "attack target"/"pull target" (the same staleness this function already
        // guards against for players, above) is never treated as a valid attacker.
        if (sServerFacade.IsFriendlyTo(playerToCheckAgainst, target))
        {
            return false;
        }

        // If the target is not fighting the player (and if the owner bot is not pulling the target)
        if (checkInCombat && !InCombat(target, player, (player == owner)))
        {
            bool isRtiTarget = false;
            if (GetBotAI(player) && !GetBotAI(player)->HasActivePlayerMaster())
            {
                Unit* rtiTarget = PAI_VALUE(Unit*, "rti target");
                if (target == rtiTarget)
                    isRtiTarget = true;
            }

            if(!isRtiTarget)
                return false;
        }

        // If the target is a player's pet and in a PvP prohibited zone
        if (target->GetObjectGuid().IsPet() && inPvPProhibitedZone)
        {
            return false;
        }

        // If the target is evading
        const Creature* creature = dynamic_cast<Creature*>(target);
        if (creature)
        {
            if (creature->GetCombatManager().IsInEvadeMode())
            {
                return false;
            }
        }        
    }

    if (IgnoreTarget(target, playerToCheckAgainst))
        return false;

    return true;
}

bool AttackersValue::IgnoreTarget(Unit* target, Player* playerToCheckAgainst)
{
    if (!GetBotAI(playerToCheckAgainst))
        return false; 

    PlayerbotAI* ai = GetBotAI(playerToCheckAgainst);
    AiObjectContext* context = ai->GetAiObjectContext();

    //Ignore Hard hostiles while not already fighting.
    if (target->GetLevel() > (playerToCheckAgainst->GetLevel() + 5) && ai->GetState() == BotState::BOT_STATE_NON_COMBAT)
    {
        //When traveling a long distance.
        if (AI_VALUE(bool, "travel target traveling") && AI_VALUE2(float, "distance", "travel target") > sPlayerbotAIConfig.reactDistance)
            return true;

        //When moving to master far away.
        if ((ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)) &&
            AI_VALUE2(bool, "trigger active", "out of react range"))
            return true;

        if (ai->GetMaster() && !ai->HasActivePlayerMaster())
        {
            Player* player = ai->GetMaster();

            //When master is traveling a long distance.
            if (PAI_VALUE(bool, "travel target traveling") && PAI_VALUE2(float, "distance", "travel target") > sPlayerbotAIConfig.reactDistance)
                return true;
        }
    }

    Player* enemyPlayer = dynamic_cast<Player*>(target);

    if (!enemyPlayer)
    {
        bool isDummy = false;


        if (WorldPosition(playerToCheckAgainst).isOverworld() && target->AI() && target->AI()->IsPreventingDeath())
        {

            isDummy = true;
        }

        uint32 entry = target->GetEntry();

#define TRAINING_DUMMY_NPC_ENTRY1 190013
#define TRAINING_DUMMY_NPC_ENTRY2 190014
#define TRAINING_DUMMY_NPC_ENTRY3 190015
#define THERAMORE_COMBAT_DUMMY 4952
#define NAXXRAMAS_COMBAT_DUMMY 16211
#ifdef MANGOSBOT_TWO
#define INITATES_TRAINING_DUMMY 32541
#endif
// This server's custom capital-city training dummies (custom_npc_training_dummy,
// sql/base/tw_world_creature_template.sql) - previously unrecognized here, which let
// bots latch onto them permanently since their 1,000,000 HP never dies.
#define HEROIC_TRAINING_DUMMY 50514
#define APPRENTICE_TRAINING_DUMMY 50515
#define EXPERT_TRAINING_DUMMY 50516

        switch (entry)
        {
        case TRAINING_DUMMY_NPC_ENTRY1:
        case TRAINING_DUMMY_NPC_ENTRY2:
        case TRAINING_DUMMY_NPC_ENTRY3:
        case THERAMORE_COMBAT_DUMMY:
        case NAXXRAMAS_COMBAT_DUMMY:
#ifdef MANGOSBOT_TWO
        case INITATES_TRAINING_DUMMY:
#endif
        case HEROIC_TRAINING_DUMMY:
        case APPRENTICE_TRAINING_DUMMY:
        case EXPERT_TRAINING_DUMMY:
            isDummy = true;
        }

        if (isDummy && ai->GetFixedBotNumber(BotTypeNumber::DUMMY_ATTACK_NUMBER, 10, 0.2f)) //90% of bots, cycle every 5 min.
            return true;
    }

    return false;
}

std::string AttackersValue::Format()
{
    std::ostringstream out;

    for (auto& target : value)
    {
        if(target != value.front())
        out << ",";

        WorldObject* wo = ai->GetWorldObject(target);

        if (wo)
            out << wo->GetName();
        else
            out << target;      
    }

    return out.str();
}

std::list<ObjectGuid> AttackersTargetingMeValue::Calculate()
{
    std::list<ObjectGuid> result;

    const std::list<ObjectGuid>& attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
    for (const ObjectGuid& attackerGuid : attackers)
    {
        Unit* attacker = ai->GetUnit(attackerGuid);
        if (attacker && (attacker->GetTarget() == bot || attacker->GetVictim() == bot))
        {
            result.push_back(attackerGuid);
        }
    }

    return result;
}
