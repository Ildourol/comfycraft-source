#pragma once

#include "playerbot/strategy/Action.h"
#include "MovementActions.h"

namespace ai
{
	class FollowAction : public MovementAction {
	public:
		FollowAction(PlayerbotAI* ai, std::string name = "follow") : MovementAction(ai, name) {}
		virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
        virtual bool CanDeadFollow(Unit* target);
	};

	class StopFollowAction : public MovementAction {
	public:
		StopFollowAction(PlayerbotAI* ai, std::string name = "stop follow") : MovementAction(ai, name) {}
		virtual bool Execute(Event& event) override { ai->StopMoving(); return true; }
        virtual bool isUseful() override;
	};

    // Off a patrol or a pack the bot stands too close to. A ranged bot runs toward
    // its master and through, to a spot on the far side of a ring around it; a
    // melee bot, or a ranged one with no spot, runs to the master.
    class AvoidPatrolAction : public MovementAction
    {
    public:
        AvoidPatrolAction(PlayerbotAI* ai) : MovementAction(ai, "avoid patrol") {}
        virtual bool Execute(Event& event) override;
    };

    class FleeToMasterAction : public FollowAction {
    public:
        FleeToMasterAction(PlayerbotAI* ai) : FollowAction(ai, "flee to master") {}

        virtual bool Execute(Event& event) override;
        virtual bool isUseful() override;
    };

}
