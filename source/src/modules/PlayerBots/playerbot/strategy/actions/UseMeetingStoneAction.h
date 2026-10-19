#pragma once
#include "MovementActions.h"

namespace ai
{
    class SummonAction : public MovementAction
    {
    public:
        SummonAction(PlayerbotAI* ai, std::string name = "summon") : MovementAction(ai, name) {}
        bool isPossible() override { return true; }
        bool isUseful() override { return true; }
        bool isUsefulWhenStunned() override { return true; }
        virtual bool Execute(Event& event) override;

    protected:
        bool Teleport(Player* requester, Player *summoner, Player *player);
        bool SummonUsingGos(Player* requester, Player *summoner, Player *player);
        bool SummonUsingNpcs(Player* requester, Player *summoner, Player *player);

        // When this companion was last summoned by its owner. A member, because
        // the context keeps one action per bot.
        time_t lastCompanionSummon = 0;
    };

    class UseMeetingStoneAction : public SummonAction
    {
    public:
        UseMeetingStoneAction(PlayerbotAI* ai) : SummonAction(ai, "use meeting stone") {}
        virtual bool Execute(Event& event) override;
    };

    class AcceptSummonAction : public Action
    {
    public:
        AcceptSummonAction(PlayerbotAI* ai) : Action(ai, "accept summon") {}
        virtual bool Execute(Event& event) override;
    };
}
