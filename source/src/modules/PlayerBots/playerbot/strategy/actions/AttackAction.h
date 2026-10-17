#pragma once

#include "playerbot/strategy/Action.h"
#include "MovementActions.h"

namespace ai
{
    // Accepting an order to fight, and everything that has to stop being true
    // for the bot to actually carry it out.
    //
    // Two things, both of which `follow` turned on and neither of which the
    // order itself would otherwise undo:
    //
    //   passive        in both engines - the stand-down mode. Cleared here
    //                  rather than on a successful bot->Attack(), because that
    //                  call returns false for ordinary reasons (already
    //                  attacking that unit, for one) and a bot left passive by
    //                  a false return looks exactly like a bot ignoring you.
    //
    //   follow         in the COMBAT engine only. `follow` sets it in both so
    //                  a retreat survives being attacked on the way home, but
    //                  in combat it makes the bot walk back to its master
    //                  instead of closing on the target it was just told to
    //                  kill. Non-combat follow stays, so they still regroup
    //                  between fights.
    //
    // Called by every order that means "go and fight this": attack, attack rti,
    // and pull.
    void EngageOnOrder(PlayerbotAI* botAi);

    // One line in target_switch.csv each time a bot with a real player master
    // changes or drops its target. `by` is the action that did it. Off unless
    // the file is in AiPlayerbot.AllowedLogFiles.
    void LogTargetSwitch(PlayerbotAI* botAi, Unit* oldTarget, Unit* newTarget, const std::string& by);

	class AttackAction : public MovementAction
	{
	public:
		AttackAction(PlayerbotAI* ai, std::string name) : MovementAction(ai, name) {}

    public:
        virtual bool Execute(Event& event) override;
        virtual bool isPossible() override { return !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CLIENT_CONTROL_LOST); }; //Override movement stay.

    protected:
        bool Attack(Player* requester, Unit* target);
        bool IsTargetValid(Player* requester, Unit* target);

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "attack"; }
        virtual std::string GetHelpDescription()
        {
            return "This action makes the bot attack a specified target.\n"
                   "The bot will engage the enemy if it's within range and not friendly or already dead.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {"current target", "old target", "available loot"}; }
#endif 
    };

    class AttackMyTargetAction : public AttackAction
    {
    public:
        AttackMyTargetAction(PlayerbotAI* ai, std::string name = "attack my target") : AttackAction(ai, name) { MakeVerbose(true); }

    public:
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "attack my target"; }
        virtual std::string GetHelpDescription()
        {
            return "This action directs the bot to attack the target you currently have selected.\n"
                   "Useful for coordinating attacks with the bot on a specific enemy.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {"attack target"}; }
#endif 
    };

    class AttackRTITargetAction : public AttackAction
    {
    public:
        AttackRTITargetAction(PlayerbotAI* ai, std::string name = "attack rti target") : AttackAction(ai, name) { MakeVerbose(true); }

    public:
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "attack rti target"; }
        virtual std::string GetHelpDescription()
        {
            return "This action makes the bot attack a target marked with a Raid Target Icon (RTI).\n"
                   "This is useful in group scenarios where specific enemies are marked for focus.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {"rti target", "attack target"}; }
#endif 
    };

    class AttackDuelOpponentAction : public AttackAction
    {
    public:
        AttackDuelOpponentAction(PlayerbotAI* ai, std::string name = "attack duel opponent") : AttackAction(ai, name) {}

    public:
        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "attack duel opponent"; }
        virtual std::string GetHelpDescription()
        {
            return "This action makes the bot attack its opponent during a duel.\n"
                   "Use this to have the bot engage in combat when dueling another player.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return {"duel target"}; }
#endif 
    };
}
