#pragma once
#include "playerbot/strategy/triggers/GenericTriggers.h"

namespace ai
{
    class KickInterruptSpellTrigger : public InterruptSpellTrigger
    {
    public:
        KickInterruptSpellTrigger(PlayerbotAI* ai) : InterruptSpellTrigger(ai, "kick") {}
    };

    CAN_CAST_TRIGGER_A(RiposteCastTrigger, "riposte");

    class SliceAndDiceTrigger : public NoBuffAndComboPointsAvailableTrigger
    {
    public:
        SliceAndDiceTrigger(PlayerbotAI* ai, uint8 comboPoints = 3) : NoBuffAndComboPointsAvailableTrigger(ai, "slice and dice", comboPoints) {}
    };

    class RogueBoostBuffTrigger : public BoostTrigger
    {
    public:
        RogueBoostBuffTrigger(PlayerbotAI* ai, std::string spellName) : BoostTrigger(ai, spellName, 200.0f) {}
        virtual bool IsActive() override { if (ai->HasAura("stealth", bot)) return false; return BoostTrigger::IsActive(); }
    };

    class RuptureTrigger : public NoDebuffAndComboPointsAvailableTrigger
    {
    public:
        RuptureTrigger(PlayerbotAI* ai, uint8 comboPoints = 4) : NoDebuffAndComboPointsAvailableTrigger(ai, "rupture", comboPoints) {}
    };

    class EviscerateTrigger : public ComboPointsAvailableTrigger
    {
    public:
        EviscerateTrigger(PlayerbotAI* ai, uint8 comboPoints = 4) : ComboPointsAvailableTrigger(ai, comboPoints) {}
    };

    class CloakOfShadowsTrigger : public NeedCureTrigger
    {
    public:
        CloakOfShadowsTrigger(PlayerbotAI* ai) : NeedCureTrigger(ai, "cloak of shadows", DISPEL_MAGIC) {}
    };

    /*
    class TricksOfTheTradeOnTankTrigger : public BuffOnTankTrigger 
    {
    public:
        TricksOfTheTradeOnTankTrigger(PlayerbotAI* ai) : BuffOnTankTrigger(ai, "tricks of the trade", 1) {}

        virtual bool IsActive() override
        {
            return BuffOnTankTrigger::IsActive() &&
                GetTarget() &&
                !ai->HasAura("tricks of the trade", GetTarget()) &&
#ifdef MANGOS
                (ai->GetBot()->IsInSameGroupWith((Player*)GetTarget()) || ai->GetBot()->IsInSameRaidWith((Player*)GetTarget())) &&
#endif
#ifdef CMANGOS
                (ai->GetBot()->IsInGroup((Player*)GetTarget(), true) || ai->GetBot()->IsInGroup((Player*)GetTarget()))
#endif               
                ;
        }
    };
    */

    class ExposeArmorTrigger : public NoDebuffAndComboPointsAvailableTrigger
    {
    public:
        ExposeArmorTrigger(PlayerbotAI* ai, uint8 comboPoints = 3) : NoDebuffAndComboPointsAvailableTrigger(ai, "expose armor", comboPoints) {}
    };

    class KickInterruptEnemyHealerSpellTrigger : public InterruptEnemyHealerTrigger
    {
    public:
        KickInterruptEnemyHealerSpellTrigger(PlayerbotAI* ai) : InterruptEnemyHealerTrigger(ai, "kick") {}
    };

    class InStealthTrigger : public HasAuraTrigger
    {
    public:
        InStealthTrigger(PlayerbotAI* ai) : HasAuraTrigger(ai, "stealth") {}
    };

    class NoStealthTrigger : public HasNoAuraTrigger
    {
    public:
        NoStealthTrigger(PlayerbotAI* ai) : HasNoAuraTrigger(ai, "stealth") 
        {
            checkInterval = 2;
        }
    };

    class RogueUnstealthTrigger : public BuffTrigger
    {
    public:
        RogueUnstealthTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "stealth", 2) {}

        bool IsActive() override
        {
            if (ai->HasAura("stealth", bot) && !bot->InBattleGround())
            {
                if (!AI_VALUE(bool, "has attackers") && !AI_VALUE(bool, "has enemy player targets"))
                {
                    if (AI_VALUE2(bool, "moving", "self target"))
                    {
                        if (ai->GetMaster())
                        {
                            return sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "master target"), 10.0f);
                        }
                        else
                        {
                            return true;
                        }
                    }
                }
            }

            return false;
        }
    };

    class StealthTrigger : public Trigger 
    {
    public:
        StealthTrigger(PlayerbotAI* ai) : Trigger(ai, "stealth") {}

        virtual bool IsActive() override
        {
            if (ai->HasAura("stealth", bot) || sServerFacade.IsInCombat(bot) || !sServerFacade.IsSpellReady(bot, 1784))
            {
                return false;
            }

            Unit* target = AI_VALUE(Unit*, "enemy player target");
            if (!target)
            {
                target = AI_VALUE(Unit*, "grind target");
            }
            if (!target)
            {
                target = AI_VALUE(Unit*, "dps target");
            }
            if (!target)
            {
                return false;
            }

            float distance = 30.0f;
            if (target && target->GetVictim())
            {
                distance -= 10;
            }

            if (sServerFacade.isMoving(target) && target->GetVictim())
            {
                distance -= 10;
            }

            if (bot->InBattleGround())
            {
                distance += 20;
            }

#ifndef MANGOSBOT_ZERO
            if (bot->InArena())
            {
                distance += 20;
            }
#endif

            return (target && sServerFacade.GetDistance2d(bot, target) < distance);
        }
    };

    class SapTrigger : public HasCcTargetTrigger
    {
    public:
        SapTrigger(PlayerbotAI* ai) : HasCcTargetTrigger(ai, "sap") {}

        virtual bool IsPossible()
        {
            return bot->GetLevel() > 10 && bot->HasSpell(6770) && !sServerFacade.IsInCombat(bot);
        }
    };

    class SprintTrigger : public BuffTrigger
    {
    public:
        SprintTrigger(PlayerbotAI* ai) : BuffTrigger(ai, "sprint", 2) {}

        virtual bool IsActive() override
        {
            if (!sServerFacade.IsSpellReady(bot, 2983))
            {
                return false;
            }

            float distance = ai->GetMaster() ? 45.0f : 35.0f;
            if (ai->HasAura("stealth", bot))
            {
                distance -= 10;
            }

            bool targeted = false;

            Unit* dps = AI_VALUE(Unit*, "dps target");
            if (dps)
            {
                targeted = (dps == AI_VALUE(Unit*, "current target"));
            }

            Unit* enemyPlayer = AI_VALUE(Unit*, "enemy player target");
            if (enemyPlayer && !targeted)
            {
                targeted = (enemyPlayer == AI_VALUE(Unit*, "current target"));
            }

            // use sprint on players
            if (enemyPlayer && !bot->CanReachWithMeleeAttack(enemyPlayer))
            {
                return true;
            }

            if (!targeted)
            {
                return false;
            }

            if ((dps && sServerFacade.IsInCombat(dps)) || (enemyPlayer))
            {
                distance -= 10;
            }

            return  AI_VALUE2(bool, "moving", "self target") &&
                    (AI_VALUE2(bool, "moving", "dps target") ||
                    AI_VALUE2(bool, "moving", "enemy player target")) &&
                    targeted &&
                    (sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "dps target"), distance) ||
                     sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "enemy player target"), distance));
        }
    };

    // The poison items of each kind. The actions apply the highest rank the bot can use.
    namespace RoguePoisonItems
    {
        inline const std::vector<uint32> deadly = { 2892, 2893, 8984, 8985, 20844, 22053, 22054, 43232, 43233 };
        inline const std::vector<uint32> crippling = { 3775, 3776 };
        inline const std::vector<uint32> mind = { 5237, 6951, 9186 };
        inline const std::vector<uint32> instant = { 6947, 6949, 6950, 8926, 8927, 8928, 21927, 43230, 43231 };
        inline const std::vector<uint32> wound = { 10918, 10920, 10921, 10922, 22055, 43234, 43235 };
        inline const std::vector<uint32> anesthetic = { 21835, 43237 };
    }

    // The weapon enchants each kind of poison gives.
    namespace RoguePoisonEnchants
    {
        inline const std::vector<uint32> deadly = { 7, 8, 626, 627, 2630, 2642, 2643, 3770, 3771 };
        inline const std::vector<uint32> crippling = { 22, 603 };
        inline const std::vector<uint32> mind = { 35, 23, 643 };
        inline const std::vector<uint32> instant = { 323, 324, 325, 623, 624, 625, 2641, 3768, 3769 };
        inline const std::vector<uint32> wound = { 703, 704, 705, 706, 2644, 3772, 3773 };
        inline const std::vector<uint32> anesthetic = { 2640, 3774 };
    }

    class ApplyPoisonTrigger : public Trigger
    {
    public:
        ApplyPoisonTrigger(PlayerbotAI* ai, bool inMainHand, const std::vector<uint32>& inPoisonEnchantIds, const std::vector<uint32>& inPoisonItemIds, std::string name = "apply poison")
        : Trigger(ai, name, 5)
        , mainHand(inMainHand)
        , poisonEnchantIds(inPoisonEnchantIds)
        , poisonItemIds(inPoisonItemIds) {}

        bool IsActive() override
        {
            // A poison the bot cannot use (level too low, no item) is never applied: do not ask for it
            if (!CanUsePoison(ai, poisonItemIds))
                return false;

            Item* weapon = mainHand ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND) : bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            if (weapon)
            {
                const uint32 currentEnchantId = weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
                if(currentEnchantId != 0)
                {
                    // Check if the current poison is the poison we need
                    return std::find(poisonEnchantIds.begin(), poisonEnchantIds.end(), currentEnchantId) == poisonEnchantIds.end();
                }

                return true;
            }

            return false;
        }

        // The same test as ApplyPoisonAction::isPossible
        static bool CanUsePoison(PlayerbotAI* ai, const std::vector<uint32>& itemIds)
        {
            Player* bot = ai->GetBot();
            for (const uint32 itemId : itemIds)
            {
                const ItemPrototype* proto = sObjectMgr.GetItemPrototype(itemId);
                if (proto && bot->GetLevel() >= proto->RequiredLevel && (ai->HasCheat(BotCheatMask::item) || bot->HasItemCount(itemId, 1)))
                    return true;
            }

            return false;
        }

    private:
        bool mainHand;
        std::vector<uint32> poisonEnchantIds;
        std::vector<uint32> poisonItemIds;
    };

    class ApplyDeadlyPoisonTrigger : public ApplyPoisonTrigger
    {
    public:
        ApplyDeadlyPoisonTrigger(PlayerbotAI* ai, bool inMainHand) : ApplyPoisonTrigger(ai, inMainHand, RoguePoisonEnchants::deadly, RoguePoisonItems::deadly, inMainHand ? "apply deadly poison main hand" : "apply deadly poison off hand") {}
    };

    // Deadly Poison wanted but not usable yet (below level 30): Instant Poison instead.
    // Either poison already on the weapon is enough, so this never undoes a Deadly Poison.
    class ApplyInstantPoisonNoDeadlyTrigger : public ApplyPoisonTrigger
    {
    public:
        ApplyInstantPoisonNoDeadlyTrigger(PlayerbotAI* ai, bool inMainHand) : ApplyPoisonTrigger(ai, inMainHand, InstantOrDeadly(), RoguePoisonItems::instant, inMainHand ? "apply instant poison main hand no deadly" : "apply instant poison off hand no deadly") {}

        bool IsActive() override
        {
            return !CanUsePoison(ai, RoguePoisonItems::deadly) && ApplyPoisonTrigger::IsActive();
        }

    private:
        static std::vector<uint32> InstantOrDeadly()
        {
            std::vector<uint32> ids = RoguePoisonEnchants::instant;
            ids.insert(ids.end(), RoguePoisonEnchants::deadly.begin(), RoguePoisonEnchants::deadly.end());
            return ids;
        }
    };

    class ApplyCripplingPoisonTrigger : public ApplyPoisonTrigger
    {
    public:
        ApplyCripplingPoisonTrigger(PlayerbotAI* ai, bool inMainHand) : ApplyPoisonTrigger(ai, inMainHand, RoguePoisonEnchants::crippling, RoguePoisonItems::crippling, inMainHand ? "apply crippling poison main hand" : "apply crippling poison off hand") {}
    };

    class ApplyMindPoisonTrigger : public ApplyPoisonTrigger
    {
    public:
        ApplyMindPoisonTrigger(PlayerbotAI* ai, bool inMainHand) : ApplyPoisonTrigger(ai, inMainHand, RoguePoisonEnchants::mind, RoguePoisonItems::mind, inMainHand ? "apply mind poison main hand" : "apply mind poison off hand") {}
    };

    class ApplyInstantPoisonTrigger : public ApplyPoisonTrigger
    {
    public:
        ApplyInstantPoisonTrigger(PlayerbotAI* ai, bool inMainHand) : ApplyPoisonTrigger(ai, inMainHand, RoguePoisonEnchants::instant, RoguePoisonItems::instant, inMainHand ? "apply instant poison main hand" : "apply instant poison off hand") {}
    };

    class ApplyWoundPoisonTrigger : public ApplyPoisonTrigger
    {
    public:
        ApplyWoundPoisonTrigger(PlayerbotAI* ai, bool inMainHand) : ApplyPoisonTrigger(ai, inMainHand, RoguePoisonEnchants::wound, RoguePoisonItems::wound, inMainHand ? "apply wound poison main hand" : "apply wound poison off hand") {}
    };

    class ApplyAnestheticPoisonTrigger : public ApplyPoisonTrigger
    {
    public:
        ApplyAnestheticPoisonTrigger(PlayerbotAI* ai, bool inMainHand) : ApplyPoisonTrigger(ai, inMainHand, RoguePoisonEnchants::anesthetic, RoguePoisonItems::anesthetic, inMainHand ? "apply anesthetic poison main hand" : "apply anesthetic poison off hand") {}
    };
}
