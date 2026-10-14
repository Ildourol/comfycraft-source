#pragma once

#include "playerbot/strategy/Action.h"

#include "playerbot/BotSlots.h"
#include "playerbot/CompanionOwnership.h"
namespace ai
{
    class AcceptInvitationAction : public Action 
    {
    public:
        AcceptInvitationAction(PlayerbotAI* ai) : Action(ai, "accept invitation") {}

        virtual bool Execute(Event& event) override
        {
            Group* grp = bot->GetGroupInvite();
            if (!grp)
                return false;

            Player* inviter = sObjectMgr.GetPlayer(grp->GetLeaderGuid());
            if (!inviter)
                return false;

            // A COMPANION ONLY EVER JOINS ITS OWNER.
            //
            // The security check below already refuses a stranger inviting a
            // companion that is in somebody's party: it is not the group leader,
            // so LevelFor returns PLAYERBOT_SECURITY_GUILD (2) against the
            // PLAYERBOT_SECURITY_INVITE (3) required here. But there is a window
            // where that reasoning does not hold - between logging in and the
            // deferred "create group" invite firing a tick later, a fresh
            // companion is in the world with NO group, and an ungrouped bot
            // falls through LevelFor to PLAYERBOT_SECURITY_INVITE, which is
            // exactly enough to accept anyone.
            //
            // Narrow, but somebody spamming invites outside the hall would hit
            // it, and since a companion leaving your group now DELETES it, a
            // successful snipe destroys it rather than merely stealing it.
            //
            // Ownership is the table, not the group, so this holds during that
            // window and every other. GMs keep the escape hatch they have
            // everywhere else in LevelFor.
            const uint32 companionOwner = CompanionOwnership::GetOwner(bot->GetGUIDLow());
            if (companionOwner
                && companionOwner != inviter->GetGUIDLow()
                && inviter->GetSession()->GetSecurity() < SEC_GAMEMASTER)
            {
                WorldPacket data(SMSG_GROUP_DECLINE, 10);
                data << bot->GetName();
                sServerFacade.SendPacket(inviter, data);
                bot->UninviteFromGroup();
                return false;
            }

			if (!ai->GetSecurity()->CheckLevelFor(PlayerbotSecurityLevel::PLAYERBOT_SECURITY_INVITE, false, inviter))
            {
                WorldPacket data(SMSG_GROUP_DECLINE, 10);
                data << bot->GetName();
                sServerFacade.SendPacket(inviter, data);
                bot->UninviteFromGroup();
                return false;
            }
            
            if (bot->isAFK())
                bot->ToggleAFK();

            WorldPacket p;
            uint32 roles_mask = 0;
            p << roles_mask;
            bot->GetSession()->HandleGroupAcceptOpcode(p);

            if (!bot->GetGroup() || !bot->GetGroup()->IsMember(inviter->GetObjectGuid()))
                return false;

            if (sRandomPlayerbotMgr.IsFreeBot(bot))
            {
                ai->SetMaster(inviter);

                std::string defaultMovementStrategy = ai->GetDefaultMovementStrategy();
                ai->ChangeStrategy("+" + defaultMovementStrategy, BotState::BOT_STATE_NON_COMBAT);
            }

            ai->ResetStrategies();
            
            ai->ChangeStrategy("-lfg,-bg", BotState::BOT_STATE_NON_COMBAT);
            ai->Reset();

            sPlayerbotAIConfig.logEvent(ai, "AcceptInvitationAction", grp->GetLeaderName(), std::to_string(grp->GetMembersCount()));

            Player* master = inviter;

            if (GetBotAI(master)) //Copy formation from bot master.
            {
                if (sPlayerbotAIConfig.inviteChat && (sRandomPlayerbotMgr.IsFreeBot(bot) || !ai->HasActivePlayerMaster()))
                {
                    std::map<std::string, std::string> placeholders;
                    placeholders["%name"] = master->GetName();
                    std::string reply;
                    if (urand(0, 3))
                        reply = BOT_TEXT2("Send me an invite %name!", placeholders);
                    else
                        reply = BOT_TEXT2("Sure I will join you.", placeholders);

                    Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());

                    if (guild && master->IsInGuild(bot->GetGuildId()))
                        guild->BroadcastToGuild(bot->GetSession(), reply, LANG_UNIVERSAL);
                    else if (sServerFacade.GetDistance2d(bot, master) < sPlayerbotAIConfig.spellDistance * 1.5)
                        bot->Say(reply, (bot->GetTeam() == ALLIANCE ? LANG_COMMON : LANG_ORCISH));
                }

                Formation* masterFormation = MAI_VALUE(Formation*, "formation");
                FormationValue* value = (FormationValue*)context->GetValue<Formation*>("formation");
                value->Load(masterFormation->getName());
            }

            ai->TellPlayer(inviter, BOT_TEXT("hello"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

            // Say where the command list is. The greeting above cannot carry
            // this: its `isPrivate = false` sends it to PARTY, so it is
            // flavour everyone sees, and a five-bot group would repeat the
            // instruction five times to the whole party. This is a private
            // line to whoever did the inviting.
            //
            // Only for a person -- a bot inviting a bot needs no help text.
            //
            // Whisper() and not TellPlayer(): TellPlayerNoFacing INFERS the
            // channel, and one of the steps is
            //
            //     if (type == CHAT_MSG_SYSTEM && randomBotSayWithoutMaster) -> SAY
            //
            // which sits BEFORE the IsRealPlayer fallback that would have made
            // it a whisper. We run RandomBotSayWithoutMaster = 1, and the
            // SetMaster above only fires for a free bot, so a companion -- the
            // common case -- could have had this announced out loud to the
            // whole zone instead of told to its owner. Naming the channel costs
            // nothing and cannot drift.
            //
            // NOT FOR A COMPANION COMING BACK after its owner logged out or the
            // server restarted: the owner was told on the first hire, and the
            // restore already says "X is on the way back."
            const bool returning = CompanionOwnership::TakeReturning(bot->GetGUIDLow());
            if (sPlayerbotAIConfig.groupJoinHelpHint && ai->IsRealPlayer(inviter) && !returning)
            {
                ai->Whisper("Whisper me: follow, stay, attack, pull, flee"
                            " - or \"help\" for the full list.", inviter->GetName());
            }

            ai->DoSpecificAction("reset raids", event, true);
            ai->DoSpecificAction("update gear", event, true);

            return true;
        }

        virtual bool isUsefulWhenStunned() override { return true; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "accept invitation"; }
        virtual std::string GetHelpDescription()
        {
            return "This action makes the bot accept group invitations.\n"
                   "It will automatically handle AFK status and update strategies.\n"
                   "For free bots, the inviter becomes the bot's master.";
        }
        virtual std::vector<std::string> GetUsedActions() { return {"reset raids", "update gear"}; }
        virtual std::vector<std::string> GetUsedValues() { return {"formation"}; }
#endif 
    };

}
