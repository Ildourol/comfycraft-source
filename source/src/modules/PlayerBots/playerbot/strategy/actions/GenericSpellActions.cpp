
#include "playerbot/playerbot.h"
#include "GenericActions.h"
#include "UseItemAction.h"
#include "playerbot/strategy/values/CcTargetValue.h"

#include <iomanip>
#include <sstream>

using namespace ai;

CastSpellAction::CastSpellAction(PlayerbotAI* ai, std::string spell)
: Action(ai, spell)
, range(ai->GetRange("spell"))
{
    SetSpellName(spell);

    float spellRange;
    if (ai->GetSpellRange(spell, &spellRange))
    {
        range = spellRange;
    }
}

bool CastSpellAction::Execute(Event& event)
{
    bool executed = false;
    uint32 spellDuration = sPlayerbotAIConfig.globalCoolDown;
    if (spellName == "conjure food" || spellName == "conjure water")
    {
        uint32 castId = 0;
        for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
        {
            uint32 spellId = itr->first;

            const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
            if (!pSpellInfo)
                continue;

            std::string namepart = pSpellInfo->SpellName[0];
            strToLower(namepart);

            if (namepart.find(spellName) == std::string::npos)
                continue;

            if (pSpellInfo->Effect[0] != SPELL_EFFECT_CREATE_ITEM)
                continue;

            uint32 itemId = pSpellInfo->EffectItemType[0];
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
            if (!proto)
                continue;

            if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
                continue;

            if (pSpellInfo->Id > castId)
                castId = pSpellInfo->Id;
        }

        executed = ai->CastSpell(castId, bot, nullptr, false, &spellDuration);
    }
    else
    {
        Unit* target = GetTarget();
        if (!target)
        {
            sLog.outDebug("%s: CastSpellAction::Execute aborting '%s' - target resolved to null at cast time", bot->GetName(), spellName.c_str());
            return false;
        }

        if (GetTargetName() == "current target" && (!bot->GetCurrentSpell(CURRENT_MELEE_SPELL) && !bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL)))
        {
            if (bot->getClass() == CLASS_HUNTER && spellName != "auto shot" && sServerFacade.GetDistance2d(bot, target) > 5.0f)
                ai->CastSpell("auto shot", target);
        }

        executed = ai->CastSpell(spellName, target, nullptr, false, &spellDuration);
    }

    if (executed)
    {
        if (ai->HasCheat(BotCheatMask::attackspeed))
            spellDuration = 1;

        SetDuration(spellDuration);
    }

    return executed;
}

bool CastSpellAction::isPossible()
{
    if (spellName == "mount")
    {
        if (!bot->IsMounted() && !bot->IsInCombat())
        {
            return true;
        }
        if (bot->IsInCombat())
        {
            ai->Unmount();
            return false;
        }
    }

    // WHY "IMPOSSIBLE". With `debug action` on, the engine prints
    // "try: <spell> impossible" and nothing more, and this function has four
    // ways to say no. Say which one, on the line before the engine's.
    // (2026-10-18: a hunter's Auto Shot was refused every tick of a fight while
    // Multi-Shot and Serpent Sting worked, and the log could not say why.)
    const bool explain = ai->HasStrategy("debug action", BotState::BOT_STATE_NON_COMBAT) && ai->GetMaster();
    auto why = [&](const std::string& reason)
    {
        if (explain)
            ai->TellPlayerNoFacing(ai->GetMaster(), "why: " + spellName + " " + reason,
                PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
    };

    Unit* spellTarget = GetTarget();
    if (!spellTarget)
    {
        why("no target (" + GetTargetName() + ")");
        return false;
    }

    bool canReach = false;
    float dist = 0.0f, minRange = 0.0f, maxRange = range + sPlayerbotAIConfig.contactDistance;
    if (spellTarget == bot)
    {
        canReach = true;
    }
    else
    {
        dist = bot->GetDistance(spellTarget, true, ai->IsRanged(bot) ? DIST_CALC_COMBAT_REACH : DIST_CALC_COMBAT_REACH_WITH_MELEE);
        if (range == ATTACK_DISTANCE) 
        {
            canReach = bot->CanReachWithMeleeAttack(spellTarget);
        }
        else 
        {
            canReach = dist <= (range + sPlayerbotAIConfig.contactDistance);
            if (!spellId)
                return false;

            const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
            if (!pSpellInfo)
                return false;

            if (range != ATTACK_DISTANCE && pSpellInfo->rangeIndex != SPELL_RANGE_IDX_COMBAT && pSpellInfo->rangeIndex != SPELL_RANGE_IDX_SELF_ONLY && pSpellInfo->rangeIndex != SPELL_RANGE_IDX_ANYWHERE)
            {
                float max_range, min_range;
                if (ai->GetSpellRange(GetSpellName(), &max_range, &min_range))
                {
                    canReach = dist < max_range && dist >= min_range;
                    minRange = min_range;
                    maxRange = max_range;
                }
            }
        }
    }

    if(!canReach)
    {
        if (explain)
        {
            std::ostringstream out;
            out << std::fixed << std::setprecision(1) << "out of reach: " << spellTarget->GetName()
                << " at " << dist << ", range " << minRange << "-" << maxRange;
            why(out.str());
        }

        return false;
    }
    
    // Check if the spell can be casted
    SpellCastResult result = SPELL_CAST_OK;
    const bool possible = ai->CanCastSpell(spellName, spellTarget, 0, nullptr, true, false, false, &result);
    if (!possible)
        why("refused on " + std::string(spellTarget->GetName()) + ", cast result " + std::to_string((uint32)result));

    // A HEAL STOPS THE WAND (2026-10-18). Every wand shot starts the global
    // cooldown, so a heal checked while the wand runs is NOT_READY, and the bot
    // gave up instead of stopping the wand: a priest wanded for 28 s while the
    // party died, about 6,000 refused heals. A player who casts stops the wand
    // first. So does the bot now, and the heal goes out on a later tick.
    if (!possible && result == SPELL_FAILED_NOT_READY)
    {
        Spell* autoRepeat = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(GetSpellID());
        const bool heal = dynamic_cast<CastHealingSpellAction*>(this) || (spellInfo && PlayerbotAI::IsHealSpell(spellInfo));
        if (autoRepeat && spellInfo && autoRepeat->m_spellInfo->Id != spellInfo->Id && heal)
        {
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
            // And keep it stopped: the timer restarted it each second, each start
            // began the cooldown again, and the heal waited 4 s.
            ai->NoteHealCast();
            why("stopped the wand for " + spellName);
        }
    }

    return possible;
}

bool CastSpellAction::isUseful()
{
    if (ai->IsInVehicle() && !ai->IsInVehicle(false, false, true))
        return false;

    if(!AI_VALUE2(bool, "spell cast useful", spellName))
        return false;

    Unit* spellTarget = GetTarget();
    if (!spellTarget)
        return false;

    if (!spellTarget->IsInWorld() || spellTarget->GetMapId() != bot->GetMapId())
        return false;

    return true;
}

NextAction** CastSpellAction::getPrerequisites()
{
    // Set the reach action as the cast spell prerequisite when needed
    const std::string reachAction = GetReachActionName();
    if (!reachAction.empty())
    {
        const std::string targetName = GetTargetName();

        // No need for a reach action when target is self
        if (targetName != "self target")
        {
            const std::string spellName = GetSpellName();
            const std::string targetQualifier = GetTargetQualifier();

            // Generate the reach action with qualifiers
            std::vector<std::string> qualifiers = { spellName, targetName };
            if (!targetQualifier.empty())
            {
                qualifiers.push_back(targetQualifier);
            }

            const std::string qualifiersStr = Qualified::MultiQualify(qualifiers, "::");
            return NextAction::merge(NextAction::array(0, new NextAction(reachAction + "::" + qualifiersStr), NULL), Action::getPrerequisites());
        }
    }

    return Action::getPrerequisites();
}

void CastSpellAction::SetSpellName(const std::string& name, std::string spellIDContextName /*= "spell id"*/, bool force)
{
    if (force || spellName != name)
    {
        spellName = name;
        spellId = ai->GetAiObjectContext()->GetValue<uint32>(spellIDContextName, name)->Get();

        float spellRange;
        if (ai->GetSpellRange(spellName, &spellRange))
        {
            range = spellRange;
        }
    }
}

Unit* CastSpellAction::GetTarget()
{
    std::string targetName = GetTargetName();
    std::string targetNameQualifier = GetTargetQualifier();
    return targetNameQualifier.empty() ? AI_VALUE(Unit*, targetName) : AI_VALUE2(Unit*, targetName, targetNameQualifier);
}

bool CastPetSpellAction::isPossible()
{
    Unit* spellTarget = GetTarget();
    if (!spellTarget)
        return false;

    Unit* pet = AI_VALUE(Unit*, "pet target");
    if (pet && ai->IsSafe(pet))
    {
        const uint32& spellId = GetSpellID();
        if (pet->HasSpell(spellId) && pet->IsSpellReady(spellId))
        {
            // Check if the pet is not too far from the owner
            if (bot->GetDistance(pet) <= sPlayerbotAIConfig.sightDistance)
            {
                bool canReach = false;
                const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
                if (pSpellInfo)
                {
                    const float dist = pet->GetDistance(spellTarget, true, DIST_CALC_COMBAT_REACH);
                    canReach = dist <= (range + sPlayerbotAIConfig.contactDistance);

                    if (pSpellInfo->rangeIndex != SPELL_RANGE_IDX_COMBAT && pSpellInfo->rangeIndex != SPELL_RANGE_IDX_SELF_ONLY && pSpellInfo->rangeIndex != SPELL_RANGE_IDX_ANYWHERE)
                    {
                        float max_range, min_range;
                        if (ai->GetSpellRange(GetSpellName(), &max_range, &min_range))
                        {
                            canReach = dist < max_range&& dist >= min_range;
                        }
                    }
                }

                if (canReach)
                {
                    return ai->CanCastSpell(spellId, spellTarget, 0, true);
                }
            }
        }
    }

    return false;
}

bool CastAuraSpellAction::isUseful()
{
    return CastSpellAction::isUseful() && !ai->HasAura(GetSpellName(), GetTarget(), false, isOwner);
}

bool CastMeleeAoeSpellAction::isUseful()
{
    return CastSpellAction::isUseful() && sServerFacade.IsDistanceLessOrEqualThan(AI_VALUE2(float, "distance", GetTargetName()), radius);
}

bool CastEnchantItemAction::isPossible()
{
    if (!CastSpellAction::isPossible())
        return false;

    return GetSpellID() && AI_VALUE2(Item*, "item for spell", GetSpellID());
}

bool CastAoeHealSpellAction::isUseful()
{
    return CastSpellAction::isUseful();
}

bool HealHotPartyMemberAction::isUseful()
{
    return HealPartyMemberAction::isUseful() && !ai->HasAura(GetSpellName(), GetTarget());
}

bool CastVehicleSpellAction::isPossible()
{
    return ai->CanCastVehicleSpell(GetSpellID(), GetTarget());
}

bool CastVehicleSpellAction::isUseful()
{
    return ai->IsInVehicle(false, true);
}

bool CastVehicleSpellAction::Execute(Event& event)
{
    return ai->CastVehicleSpell(GetSpellID(), GetTarget(), speed, needTurn);
}

bool CastFrozenDeathboltAction::isPossible()
{
    Unit* target = GetTarget();

    if (!target)
        return false;

    if (target->GetDistance(bot) > range)
        return false;

    return CastVehicleSpellAction::isPossible();
}

bool CastDevourHumanoidAction::isPossible()
{
    Unit* target = GetTarget();

    if (!target)
        return false;

    if (target->GetDistance(bot) > range)
        return false;

    return CastVehicleSpellAction::isPossible();
}

bool CastShootAction::isPossible()
{
    // Check if the bot has a ranged weapon equipped and has ammo
    UpdateWeaponInfo();
    if (rangedWeapon && !needsAmmo)
    {
        // Check if the target exist and it can be shot
        Unit* target = GetTarget();
        if (target && sServerFacade.IsWithinLOSInMap(bot, target))
        {
            return CastSpellAction::isPossible();
        }
    }

    return false;
}

// ONLY HEALS FOR 10 S AFTER A HEAL (2026-10-18, the owner's request).
//
// A healer wanded between heals, and each wand shot starts the global cooldown,
// so the next heal waited or was refused. After a heal that worked, a healer does
// not start the wand for HEAL_ONLY_SECONDS, and the wand stops if it runs.
static const time_t HEAL_ONLY_SECONDS = 10;

bool CastShootAction::isUseful()
{
    if (PlayerbotAI::IsHeal(bot) && ai->HealedWithin(HEAL_ONLY_SECONDS))
    {
        if (bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);

        return false;
    }

    return CastSpellAction::isUseful();
}

bool CastShootAction::Execute(Event& event)
{
    bool succeeded = false;

    UpdateWeaponInfo();

    if (rangedWeapon && !needsAmmo)
    {
        // Prevent calling the shoot spell when already active.
        //
        // No wait while the weapon fires. This set the weapon speed plus the
        // GCD (3 s or more) on every call, and a healer priest calls shoot
        // once a second between heals, so it did not see a party member's
        // health for most of each fight (2026-10-17). A new shot keeps the
        // duration CastSpellAction sets, the GCD.
        Spell* autoRepeatSpell = ai->GetBot()->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
        if (autoRepeatSpell && (autoRepeatSpell->m_spellInfo->Id == GetSpellID()))
        {
            SetDuration(sPlayerbotAIConfig.reactDelay);
            succeeded = true;
        }
        else if (CastSpellAction::Execute(event))
        {
            succeeded = true;
        }
    }

    return succeeded;
}

bool CastCrowdControlSpellAction::Execute(Event& event)
{
    // A CAST ALREADY IN FLIGHT MUST BE LEFT ALONE.
    //
    // This is the polymorph spam, and the mechanism is the trigger rather than
    // anything about the spell. HasCcTargetTrigger is "there is a cc target and
    // it does not yet have my cc aura" - and during the one and a half seconds
    // the sheep is being CAST, the aura is not on yet. So the trigger stays
    // active for the whole cast and re-pushes this action, at ACTION_INTERRUPT,
    // which outranks nearly everything. Executing again calls PlayerbotAI::
    // CastSpell, which builds a NEW Spell and starts it - cancelling the cast
    // that was four fifths done. Every tick. Forever.
    //
    // Nothing downstream can fix this: from the cast's point of view it is
    // being legitimately re-ordered. The action has to decline to start a
    // second one.
    //
    // Returning TRUE matters as much as declining. False would let the tick
    // fall through to the next action - the reach prerequisite among them - and
    // moving is the other way a cast dies. True with a duration says "this is
    // handled, come back when it has had time to land".
    if (bot->IsNonMeleeSpellCasted(true, false, true))
    {
        SetDuration(sPlayerbotAIConfig.globalCoolDown);
        return true;
    }

    Unit* target = GetTarget();

    // Another group member's cc holds the mark or is being cast at it. The
    // target came from a value cached for up to a second, so ask again here.
    if (target && CcMarkIsTaken(ai, target))
        return false;

    // A new target starts the count again - the attempts that matter are the
    // ones spent on THIS mob.
    const ObjectGuid targetGuid = target ? target->GetObjectGuid() : ObjectGuid();
    if (targetGuid != lastTarget)
    {
        lastTarget = targetGuid;
        attempts = 0;
        lastFailure = SPELL_CAST_OK;
    }

    const bool started = CastRangedDebuffSpellAction::Execute(event);

    // ONE LINE THAT SETTLES IT, under `nc +debug`.
    //
    // The trace of 2026-09-07 showed "polymorph 1 (40.01) (duration: 0.10s)"
    // repeating: executed, but with the Action constructor's DEFAULT duration,
    // which means nothing on the success path ever called SetDuration - and
    // CastSpellAction::Execute always does when it casts. So the two facts
    // disagree, and everything downstream of that has been guesswork. This
    // reports what the base actually returned, whether the core thinks a spell
    // is in flight, and what duration came out, so the next attempt is decided
    // by evidence instead of inference.
    if (ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
    {
        if (Player* master = ai->GetMaster())
        {
            std::ostringstream out;
            out << "[cc] " << GetSpellName()
                << " base=" << (started ? "true" : "false")
                << " casting=" << (bot->IsNonMeleeSpellCasted(true, false, true) ? "yes" : "no")
                << " dur=" << GetDuration()
                << " target=" << (target ? target->GetName() : "none");
            ai->TellPlayerNoFacing(master, out.str());
        }
    }

    if (started)
    {
        attempts = 0;
        lastFailure = SPELL_CAST_OK;
        return true;
    }

    ++attempts;

    // WHY did it refuse - asked without the forgiveness isPossible was given,
    // which is what makes the answer worth branching on.
    SpellCastResult reason = SPELL_CAST_OK;
    if (target)
    {
        ai->CanCastSpell(GetSpellName(), target, 0, nullptr, false, false, false, &reason);
    }

    // The response belongs to the reason. Three groups, and they want different
    // things:
    uint32 backoff;

    if (reason == SPELL_FAILED_MOVING || reason == SPELL_FAILED_NOT_INFRONT ||
        reason == SPELL_FAILED_UNIT_NOT_INFRONT || reason == SPELL_FAILED_NOT_STANDING ||
        reason == SPELL_FAILED_TRY_AGAIN)
    {
        // ALREADY FIXED, JUST NOT YET. PlayerbotAI::CastSpell calls StopMoving
        // and corrects facing on its way to failing, so the next attempt has a
        // real chance. One tick, not one inside this one.
        backoff = sPlayerbotAIConfig.reactDelay;
    }
    else if (reason == SPELL_FAILED_OUT_OF_RANGE || reason == SPELL_FAILED_LINE_OF_SIGHT)
    {
        // FIXABLE BY WALKING, and something else is already walking. The reach
        // prerequisite ("reach spell") is what closes the gap; this only has to
        // stay out of its way and not burn the tick re-asking. Wait long enough
        // to have actually moved.
        backoff = sPlayerbotAIConfig.globalCoolDown;
    }
    else
    {
        // NOT FIXABLE BY ANYTHING THIS ACTION DOES - immune, wrong creature
        // type, out of the caster's control. Re-asking is how the mage ends up
        // locked in place, so back off hard and let it fight.
        backoff = 5000;
    }

    // The ceiling. Whatever the reason claims, a cc that has failed this many
    // times in a row on one mob is not about to start working, and standing
    // there proving it is the actual complaint. Nothing here can spin.
    if (attempts >= 5)
    {
        backoff = 10000;
    }

    // ai->SetActionDuration(uint32), not this action's SetDuration(): the
    // engine only reads an action's own duration when Execute returned TRUE,
    // and this one is about to return false. SetAIInternalUpdateDelay is
    // protected, so this public overload is the sanctioned way in.
    ai->SetActionDuration(backoff);

    // Say something on the FIRST refusal for this target, not only when the
    // reason changes.
    //
    // Reporting on change alone had a silent case, and it was the common one:
    // lastFailure starts at SPELL_CAST_OK, so a cast that failed while the
    // re-asked check came back OK - which is exactly what a line-of-sight
    // failure looks like once the bot has turned - compared equal and said
    // nothing at all. A mage stuck on a wall, in silence, was the report.
    if (attempts == 1 || reason != lastFailure)
    {
        lastFailure = reason;

        if (Player* master = ai->GetMaster())
        {
            std::ostringstream out;
            out << "can't " << GetSpellName() << " that (" << (uint32)reason << ")";
            ai->TellPlayerNoFacing(master, out.str());
        }
    }

    return false;
}

void CastShootAction::UpdateWeaponInfo()
{
    // Check if we have a new ranged weapon equipped
    const Item* equippedWeapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (equippedWeapon)
    {
        if (equippedWeapon != rangedWeapon)
        {
            std::string spellName = "shoot";
            bool isRangedWeapon = false;

#ifdef MANGOSBOT_ZERO
            needsAmmo = true;
#endif

            const ItemPrototype* itemPrototype = equippedWeapon->GetProto();
            switch (itemPrototype->SubClass)
            {
                case ITEM_SUBCLASS_WEAPON_GUN:
                {
                    isRangedWeapon = true;
#ifdef MANGOSBOT_ZERO
                    spellName += " gun";
#endif
                    break;
                }
                case ITEM_SUBCLASS_WEAPON_BOW:
                {
                    isRangedWeapon = true;
#ifdef MANGOSBOT_ZERO
                    spellName += " bow";
#endif
                    break;
                }
                case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                {
                    isRangedWeapon = true;
#ifdef MANGOSBOT_ZERO
                    spellName += " crossbow";
#endif
                    break;
                }
                case ITEM_SUBCLASS_WEAPON_WAND:
                {
                    isRangedWeapon = true;
#ifdef MANGOSBOT_ZERO
                    needsAmmo = false;
#endif
                    break;
                }
                case ITEM_SUBCLASS_WEAPON_THROWN:
                {
                    isRangedWeapon = true;
                    spellName = "throw";
                    break;
                }

                default: break;
            }

            // Set the new weapon parameters
            if (isRangedWeapon)
            {
                SetSpellName(spellName);
                rangedWeapon = equippedWeapon;
            }
        }

        // Check the ammunition
#ifdef MANGOSBOT_ZERO
        needsAmmo = (GetSpellName() != "shoot") ? (AI_VALUE2(uint32, "item count", "ammo") <= 0) : false;
#endif
    }
    else
    {
        rangedWeapon = nullptr;
    }
}

bool RemoveBuffAction::isUseful()
{
    return ai->HasAura(name, bot);
}

bool RemoveBuffAction::Execute(Event& event)
{
    ai->RemoveAura(name);
    return !ai->HasAura(name, bot);
}

bool InterruptCurrentSpellAction::isUseful()
{
    for (int type = CURRENT_MELEE_SPELL; type < CURRENT_CHANNELED_SPELL; type++)
    {
        Spell* currentSpell = bot->GetCurrentSpell((CurrentSpellTypes)type);
        if (currentSpell && currentSpell->CanBeInterrupted())
            return true;
    }
    return false;
}

bool InterruptCurrentSpellAction::Execute(Event& event)
{
    bool interrupted = false;
    for (int type = CURRENT_MELEE_SPELL; type < CURRENT_CHANNELED_SPELL; type++)
    {
        Spell* currentSpell = bot->GetCurrentSpell((CurrentSpellTypes)type);
        if (currentSpell && currentSpell->CanBeInterrupted())
        {
            bot->InterruptSpell((CurrentSpellTypes)type);
            ai->SpellInterrupted(currentSpell->m_spellInfo->Id);
            interrupted = true;
        }
    }
    return interrupted;
}

Unit* CastSpellTargetAction::GetTarget()
{
    // Check for assigned targets
    const std::list<ObjectGuid>& possibleTargets = AI_VALUE(std::list<ObjectGuid>, targetsValue);
    if (!possibleTargets.empty())
    {
        for (const ObjectGuid& possibleTargetGuid : possibleTargets)
        {
            Unit* possibleTarget = ai->GetUnit(possibleTargetGuid);
            if (IsTargetValid(possibleTarget))
            {
                return possibleTarget;
            }
        }
    }
    else
    {
        // Check for the default target
        Unit* possibleTarget = CastSpellAction::GetTarget();
        if (IsTargetValid(possibleTarget))
        {
            return possibleTarget;
        }
    }

    return nullptr;
}

bool CastSpellTargetAction::IsTargetValid(Unit* target)
{
    return target &&
           ai->IsSafe(target) &&
           (bot == target || sServerFacade.GetDistance2d(bot, target) < sPlayerbotAIConfig.sightDistance) &&
           bot->IsInGroup(target) &&
           (!aliveCheck || !target->IsDead()) &&
           (!auraCheck || !ai->HasAura(GetSpellID(), target));
}

bool CastItemTargetAction::IsTargetValid(Unit* target)
{
    if (CastSpellTargetAction::IsTargetValid(target))
    {
        if (itemAuraCheck)
        {
            const uint32 itemId = GetItemId();
            const ItemPrototype* proto = sObjectMgr.GetItemPrototype(itemId);
            if (proto)
            {
                for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                {
#ifdef MANGOSBOT_ZERO
                    if (proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE || proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_NO_DELAY_USE)
#else
                    if (proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
#endif
                    {
                        if (proto->Spells[i].SpellId > 0 && ai->HasAura(proto->Spells[i].SpellId, target))
                        {
                            return false;
                        }
                    }
                }

                return true;
            }
        }
        else
        {
            return true;
        }
    }

    return false;
}

bool CastItemTargetAction::isUseful()
{
    const ItemPrototype* proto = sObjectMgr.GetItemPrototype(GetItemId());
    if (proto)
    {
        std::set<uint32>& skipSpells = AI_VALUE(std::set<uint32>&, "skip spells list");
        if (!skipSpells.empty())
        {
            for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                const _Spell& spellData = proto->Spells[i];
                if (spellData.SpellId)
                {
                    if (skipSpells.find(spellData.SpellId) != skipSpells.end())
                    {
                        return false;
                    }
                }
            }
        }

        return true;
    }

    return false;
}

bool CastItemTargetAction::isPossible()
{
    uint32 itemId = GetItemId();
    if (!itemId)
        return false;

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);

    if (!proto)
        return false;

    if (HasSpellCooldown(itemId))
        return false;

    if (!ai->HasCheat(BotCheatMask::item) && !bot->HasItemCount(itemId, 1))
        return false;

    uint32 spellCount = 0;

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        _Spell const& spellData = proto->Spells[i];

        // no spell
        if (!spellData.SpellId)
            continue;

        // wrong triggering type
#ifdef MANGOSBOT_ZERO
        if (spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE && spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_NO_DELAY_USE)
#else
        if (spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
#endif
            continue;

        SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellData.SpellId);
        if (!spellInfo)
        {
            continue;
        }

        spellCount++;
    }

    return spellCount;
}

bool CastItemTargetAction::Execute(Event& event)
{
    uint32 itemId = GetItemId();
    Unit* target = GetTarget();
    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);

    if (!proto)
        return false;

    Item* item = nullptr;

    if (!ai->HasCheat(BotCheatMask::item)) //If bot has no item cheat it needs an item to cast.
    {
        std::list<Item*> items = AI_VALUE2(std::list<Item*>, "inventory items", chat->formatQItem(itemId));

        if (items.empty())
            return false;

        item = items.front();
    }

    SpellCastTargets targets;
    if (target)
    {
        targets.setUnitTarget(target);
        targets.setDestination(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    }
    else
        targets.m_targetMask = TARGET_FLAG_SELF;

    // use triggered flag only for items with many spell casts and for not first cast
    int count = 0;

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        _Spell const& spellData = proto->Spells[i];

        // no spell
        if (!spellData.SpellId)
            continue;

        // wrong triggering type
#ifdef MANGOSBOT_ZERO
        if (spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE && spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_NO_DELAY_USE)
#else
        if (spellData.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
#endif
            continue;

        SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellData.SpellId);
        if (!spellInfo)
        {
            continue;
        }

        if (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
            targets.m_targetMask = TARGET_FLAG_DEST_LOCATION;

        BotUseItemSpell* spell = new BotUseItemSpell(bot, spellInfo, (count > 0) ? TRIGGERED_OLD_TRIGGERED : TRIGGERED_NONE);

        Item* tItem = nullptr;

        if (item)
        {
            spell->SetCastItem(item);
            item->SetUsedInSpell(true);
        }

        spell->m_clientCast = true;

        bool result = (spell->ForceSpellStart(&targets) == SPELL_CAST_OK);

        if (!result)
            return false;

        if (ai->HasCheat(BotCheatMask::item))
        {
            if (!HasSpellCooldown(itemId))
            {
                bot->RemoveSpellCooldown(*spellInfo, false);
                bot->AddCooldown(*spellInfo, proto, false);
            }
        }

        ++count;
    }

    return count;
}

bool CastItemTargetAction::HasSpellCooldown(uint32 itemId)
{
    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);

    if (!proto)
        return false;

    uint32 spellId = 0;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (proto->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
        {
            continue;
        }

        if (proto->Spells[i].SpellId > 0)
        {
            if (!sServerFacade.IsSpellReady(bot, proto->Spells[i].SpellId))
                return true;

            if (!sServerFacade.IsSpellReady(bot, proto->Spells[i].SpellId, itemId))
                return true;
        }
    }

    return false;
}
