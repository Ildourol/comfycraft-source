#pragma once
#include "playerbot/strategy/Value.h"
#include "playerbot/PlayerbotAIConfig.h"

namespace ai
{
    // List of hostile targets that are in combat with the bot (or bot group)
    class AttackersValue : public ObjectGuidListCalculatedValue, public Qualified
	{
        struct AddGuardiansHelper
        {
            explicit AddGuardiansHelper(std::set<Unit*>& units) : units(units) {}
            void operator()(Unit* target) const
            {
                units.insert(target);
            }

            std::set<Unit*>& units;
        };

	public:
        AttackersValue(PlayerbotAI* ai, std::string name = "attackers", int interval = 2) : ObjectGuidListCalculatedValue(ai, name, interval), Qualified() {}
        virtual std::list<ObjectGuid> Calculate() override;

        // ignoreLos is for ORDERED targets only - the mob the player pointed at
        // by name. Everything else has to be seen to be fought.
        static bool IsValid(Unit* target, Player* player, Player* owner = nullptr, bool checkInCombat = true, bool validatePossibleTarget = true, bool ignoreLos = false);

    private:
        // Whether the pull hold was on last time this was calculated, so
        // entering and leaving it can be announced once each instead of every
        // tick. Holding is otherwise completely silent - a dps standing still
        // on purpose and a dps standing still by accident look identical, which
        // is the whole reason this exists.
        bool wasHoldingForPull = false;

        // THE PLAYER'S OWN PULL. Takes a mob off the list while it is still
        // running at the group, so a dps does not hit it on the way in. Released
        // is for good, so a mob that runs again later in the fight (fear, a
        // switch to the healer) is not held a second time. arrivedMs is when it
        // reached the player, 0 before that.
        struct Approach
        {
            bool held = true;
            uint32 arrivedMs = 0;
        };
        std::map<ObjectGuid, Approach> approaching;
        void HoldApproaching(std::list<ObjectGuid>& result);
        bool holdingApproach = false;

    public:
        // A mob was left off the list by the pull hold at the last calculation.
        bool IsHoldingApproach() const { return holdingApproach; }


        // mod-playerbots spelling; the range argument there is folded into the
        // validity checks here, so it is accepted and dropped.
        // A real player swinging at the target, or casting a harmful spell at it that is not
        // crowd control. Counts before the first hit lands, so the group joins on the swing.
        static bool IsRealPlayerAttacking(Unit* target, Player* player);

        // On the list only because a real player has started on it: nothing has hit it yet.
        // A reason for a bot with no fight to join, not for a bot in a fight to switch.
        static bool IsOnlyStartedOn(Unit* target, Player* player);

        // The bot's current target while it is still a fight: alive, in combat and on the
        // possible attack targets list. Null between fights.
        static Unit* HeldTarget(PlayerbotAI* ai);

        static bool IsPossibleTarget(Unit* target, Player* player, float /*range*/ = 0.0f, bool checkInCombat = true)
        { return IsValid(target, player, nullptr, checkInCombat); }
        static bool IgnoreTarget(Unit* target, Player* playerToCheckAgainst);
        virtual std::string Format() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "attackers"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "combat"; }
        virtual std::string GetHelpDescription()
        {
            return "This value contains all the units attacking the player, it's group or those the player or group is attacking.";
        }
        virtual std::vector<std::string> GetUsedValues() { return {"possible targets", "current target" , "attack target" ,  "pull target" }; }
#endif

	private:
        void AddTargetsOf(Group* group, std::set<Unit*>& targets, std::set<ObjectGuid>& invalidTargets, bool getOne = false);
        void AddTargetsOf(Player* player, std::set<Unit*>& targets, std::set<ObjectGuid>& invalidTargets, bool getOne = false);
        static float GetRange() { return sPlayerbotAIConfig.sightDistance; }

        static bool InCombat(Unit* target, Player* player, bool checkPullTargets = true);

        WorldPosition calculatePos;
    };

    // List of attackers that are currently targeting the bot
    class AttackersTargetingMeValue : public ObjectGuidListCalculatedValue
    {
    public:
        AttackersTargetingMeValue(PlayerbotAI* ai, std::string name = "attackers targeting me") : ObjectGuidListCalculatedValue(ai, name) {}
        std::list<ObjectGuid> Calculate() override;
    };
}
