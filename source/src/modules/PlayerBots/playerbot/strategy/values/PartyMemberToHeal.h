#pragma once
#include "playerbot/strategy/Value.h"
#include "PartyMemberValue.h"

namespace ai
{
    class PartyMemberToHeal : public PartyMemberValue
	{
	public:
        PartyMemberToHeal(PlayerbotAI* ai, std::string name = "party member to heal") :
          PartyMemberValue(ai, name) {}

        // A HEALER LOOKS FARTHER THAN IT HEALS (bug #139, 2026-11-03). Check()
        // dropped every member more than AiPlayerbot.HealDistance (30 yd) from
        // the bot, so a tank at 31 yd was never chosen, and nothing walked the
        // healer to it. Each heal spell still checks its own range.
        //
        // In a battleground: HealDistanceBg. A bot that is not a healer, or not
        // in a group: the heal range. A healer that may not move (passive or
        // stay): HealSpellRange(), so it heals what it can reach from where it
        // stands. Any other healer: SightDistance less 5 yd.
        static float SearchRange(PlayerbotAI* ai);

        // Every direct heal of this build has a 40 yd range (Lesser Heal, Heal,
        // Flash Heal, Greater Heal, Renew, Healing Touch, Regrowth, Rejuvenation,
        // Healing Wave, Lesser Healing Wave, Chain Heal, Holy Light, Flash of Light).
        static float HealSpellRange() { return 40.0f; }

        // A HEALER STANDS STILL WHILE IT CAN HEAL (2026-11-03). The figures that
        // keep a healer companion out of the 10 yd "keep away from the fight" flee
        // (`enemy too close for spell`) and inside heal range at once. Mobs on a
        // tank stand within about 5 yd of it.
        //
        // The walk to a patient stops this far inside HealSpellRange (35 yd).
        static float HealChaseMargin() { return 5.0f; }
        // A healer companion's loose follow distance in a dungeon. It stands still
        // while its owner is within this + 4 yd (22) and walks to this - 2 (16).
        static float HealerFollowDistance() { return 18.0f; }
        // A healer's flee spot is at least this far from every mob in the fight,
        // 5 yd more than the flee trigger's 10 yd, so a mob that shifts a yard or
        // two does not start the next flee.
        static float HealerFightGap() { return 15.0f; }

        // The patient is hurt enough for the healer to walk to it: below full
        // health and in a fight (its own or the bot's), or below AlmostFullHealth.
        // A healer seated with a drink walks only below AlmostFullHealth.
        static bool WorthWalkingTo(PlayerbotAI* ai, Unit* patient);

        // A healer in a group whose "party member to heal" is WorthWalkingTo.
        // Follow holds still for it (FollowAction::isUseful).
        static bool HealerHasPatient(PlayerbotAI* ai);

    protected:
        virtual Unit* Calculate() override;
        bool CanHealPet(Pet* pet);
        virtual bool Check(Unit* player);

    private:
        std::vector<Player*> GetPartyMembers();
	};

    // The MOB attacking one of my party, not the party member being attacked.
    //
    // Deliberately NOT PartyMemberToProtect, which only fires once the victim is
    // under 30% health (10% for a tank). That is a "save them" emergency; this
    // is "pick the add up before it gets that far", so it has no health
    // threshold at all.
    class PartyMemberAttacker : public PartyMemberValue
    {
    public:
        PartyMemberAttacker(PlayerbotAI* ai, std::string name = "party member attacker") :
            PartyMemberValue(ai, name) {}

    protected:
        Unit* Calculate() override;
    };

    class PartyMemberToProtect : public PartyMemberValue
    {
    public:
        PartyMemberToProtect(PlayerbotAI* ai, std::string name = "party member to protect") :
            PartyMemberValue(ai, name) {}

    protected:
        virtual Unit* Calculate() override;
    };

    class PartyMemberToRemoveRoots : public PartyMemberValue
    {
    public:
        PartyMemberToRemoveRoots(PlayerbotAI* ai, std::string name = "party member to remove roots") :
            PartyMemberValue(ai, name) {}

    protected:
        virtual Unit* Calculate() override;
    };
}
