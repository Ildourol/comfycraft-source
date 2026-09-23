#include "PlayerbotMgr.h"
#include "Chat/ComfyCommands.h"
#include "playerbot/PlayerbotChatHandler.h"
#include "playerbot/playerbot.h"
#include "playerbot/AiContextAugment.h"
#include "playerbot/PerformanceMonitor.h"
#include <stdarg.h>
#include <iomanip>

#include "playerbot/AiFactory.h"

#include "Movement/MovementGenerator.h"
#include "Maps/GridNotifiers.h"
#include "Maps/GridNotifiersImpl.h"
#include "Maps/CellImpl.h"
#include "strategy/values/LastMovementValue.h"
#include "strategy/actions/LogLevelAction.h"
#include "strategy/actions/SayAction.h"
#include "strategy/actions/EmoteAction.h"
#include "strategy/values/LastSpellCastValue.h"
#include "LootObjectStack.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "PlayerbotAI.h"
#include "BotDiagnostics.h"
#include "BotNpc.h"
#include "LuaScene.h"
#include "ScriptMgr.h"
#include "Database/DBCStores.h"
#include <algorithm>
#include <sstream>
#include <vector>
#include "playerbot/PlayerbotFactory.h"
#include "PlayerbotSecurity.h"
#include "Group/Group.h"
#include "Objects/Pet.h"
#include "Spells/SpellAuras.h"
#include "Spells/SpellMgr.h"
#include "PlayerbotDbStore.h"
#include "strategy/values/PositionValue.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/TravelMgr.h"
#include "Movement/spline/MoveSplineInitArgs.h"
#include "Maps/InstanceData.h"
#include "ChatHelper.h"
#include "strategy/values/BudgetValues.h"
#include "SocialMgr.h"
#include "PlayerbotTextMgr.h"
#include "RandomItemMgr.h"
#include "strategy/ItemVisitors.h"
#include "strategy/values/LootValues.h"
#include "strategy/values/AttackersValue.h"
#include "Transports/Transport.h"
#include "Guild/GuildMgr.h"
#include "Chat/ChannelMgr.h"
#include "PlayerbotLLMInterface.h"

#include <boost/algorithm/string.hpp>

#ifdef MANGOSBOT_TWO
#include "Entities/Vehicle.h"
#endif

#ifdef BUILD_ELUNA
#include "LuaEngine/LuaEngine.h"
#endif
#include "AI/ScriptDevAI/ScriptDevAIMgr.h"
#include "strategy/values/GuildValues.h"

using namespace ai;

std::vector<std::string>& split(const std::string &s, char delim, std::vector<std::string> &elems);
std::vector<std::string> split(const std::string &s, char delim);
char * strstri (std::string str1, std::string str2);
uint64 extractGuid(WorldPacket& packet);
std::string &trim(std::string &s);

std::set<std::string> PlayerbotAI::unsecuredCommands;

uint32 PlayerbotChatHandler::extractQuestId(std::string str)
{
    char* source = (char*)str.c_str();
    char* cId = ExtractKeyFromLink(&source,"Hquest");
    return cId ? atol(cId) : 0;
}

void PacketHandlingHelper::AddHandler(uint16 opcode, std::string handler, bool shouldDelay)
{
    handlers[opcode] = handler;
    delay[opcode] = shouldDelay;
}

void PacketHandlingHelper::Handle(ExternalEventHelper &helper)
{
    if (!m_botPacketMutex.try_lock()) //Packets do not have to be handled now. Handle them later.
        return;

    // queue holds unique_ptr<WorldPacket> due to Penqle's move-only WorldPacket.
    std::stack<std::unique_ptr<WorldPacket>> delayed;

    while (!queue.empty())
    {
        if (!helper.HandlePacket(handlers, *queue.top()))
            if (delay[queue.top()->GetOpcode()])
                delayed.push(std::move(queue.top()));
        queue.pop();
    }

    queue = std::move(delayed);

    m_botPacketMutex.unlock();
}

void PacketHandlingHelper::AddPacket(const WorldPacket& packet)
{
    if (packet.empty() && packet.GetOpcode() != MSG_RAID_READY_CHECK)
        return;

    m_botPacketMutex.lock(); //We are going to add packets. Stop any new handling and add them.

	if (handlers.find(packet.GetOpcode()) != handlers.end())
        queue.push(std::make_unique<WorldPacket>(packet));

    m_botPacketMutex.unlock();
}

PlayerbotAI::PlayerbotAI() : PlayerbotAIBase(), bot(NULL), aiObjectContext(NULL),
    currentEngine(NULL), chatHelper(this), chatFilter(this), accountId(0), security(NULL), master(NULL), currentState(BotState::BOT_STATE_NON_COMBAT), faceTargetUpdateDelay(0), jumpTime(0), fallAfterJump(false)
{
    for (uint8 i = 0 ; i < (uint8)BotState::BOT_STATE_ALL; i++)
        engines[i] = NULL;

    for (int i = 0; i < MAX_ACTIVITY_TYPE; i++)
    {
        allowActiveCheckTimer[i] = time(nullptr);
        allowActive[i] = false;
    }
}

PlayerbotAI::PlayerbotAI(Player* bot) :
    PlayerbotAIBase(), chatHelper(this), chatFilter(this), security(bot), master(NULL), faceTargetUpdateDelay(0), jumpTime(0), fallAfterJump(false)
{
    this->bot = bot;
    if (!bot->isTaxiCheater() && HasCheat(BotCheatMask::taxi))
        bot->SetTaxiCheater(true);

    for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        engines[i] = NULL;

    for (int i = 0; i < MAX_ACTIVITY_TYPE; i++)
    {
        allowActiveCheckTimer[i] = time(nullptr);
        allowActive[i] = false;
    }

	accountId = sObjectMgr.GetPlayerAccountIdByGUID(bot->GetObjectGuid());

    aiObjectContext = AiFactory::createAiObjectContext(bot, this);
    // Hand the fresh context to module augmenters - see AiContextAugment.h.
    RunAiContextAugmenters(this, aiObjectContext);

    UpdateTalentSpec();

    engines[(uint8)BotState::BOT_STATE_COMBAT] = AiFactory::createCombatEngine(bot, this, aiObjectContext);
    engines[(uint8)BotState::BOT_STATE_NON_COMBAT] = AiFactory::createNonCombatEngine(bot, this, aiObjectContext);
    engines[(uint8)BotState::BOT_STATE_DEAD] = AiFactory::createDeadEngine(bot, this, aiObjectContext);
    engines[(uint8)BotState::BOT_STATE_REACTION] = reactionEngine = AiFactory::createReactionEngine(bot, this, aiObjectContext);

    for (uint8 e = 0; e < (uint8)BotState::BOT_STATE_ALL; e++)
    {
        engines[e]->initMode = false;
        engines[e]->Init();
    }

    currentEngine = engines[(uint8)BotState::BOT_STATE_NON_COMBAT];
    currentState = BotState::BOT_STATE_NON_COMBAT;

    masterIncomingPacketHandlers.AddHandler(CMSG_GAMEOBJ_USE, "use game object");
    masterIncomingPacketHandlers.AddHandler(CMSG_AREATRIGGER, "area trigger");
    masterIncomingPacketHandlers.AddHandler(CMSG_LOOT_ROLL, "loot roll", true);
    masterIncomingPacketHandlers.AddHandler(CMSG_GOSSIP_HELLO, "gossip hello");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUESTGIVER_HELLO, "gossip hello");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUESTGIVER_COMPLETE_QUEST, "complete quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUESTGIVER_ACCEPT_QUEST, "accept quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_QUEST_CONFIRM_ACCEPT, "confirm quest");
    masterIncomingPacketHandlers.AddHandler(CMSG_ACTIVATETAXI, "activate taxi");
    masterIncomingPacketHandlers.AddHandler(CMSG_ACTIVATETAXIEXPRESS, "activate taxi");
    masterIncomingPacketHandlers.AddHandler(CMSG_TAXICLEARALLNODES, "taxi done");
    masterIncomingPacketHandlers.AddHandler(CMSG_TAXICLEARNODE, "taxi done");
    masterIncomingPacketHandlers.AddHandler(CMSG_GROUP_UNINVITE, "uninvite");
    masterIncomingPacketHandlers.AddHandler(CMSG_GROUP_UNINVITE_GUID, "uninvite guid");
    masterIncomingPacketHandlers.AddHandler(CMSG_PUSHQUESTTOPARTY, "quest share");
    masterIncomingPacketHandlers.AddHandler(CMSG_CAST_SPELL, "see spell");
    masterIncomingPacketHandlers.AddHandler(CMSG_REPOP_REQUEST, "release spirit");
    masterIncomingPacketHandlers.AddHandler(CMSG_RECLAIM_CORPSE, "revive from corpse");

#ifdef MANGOSBOT_TWO
    masterIncomingPacketHandlers.AddHandler(CMSG_LFG_TELEPORT, "lfg teleport");
#endif

    botOutgoingPacketHandlers.AddHandler(SMSG_PETITION_SHOW_SIGNATURES, "petition offer");
    botOutgoingPacketHandlers.AddHandler(SMSG_BATTLEFIELD_STATUS, "bg status");
    botOutgoingPacketHandlers.AddHandler(SMSG_GROUP_INVITE, "group invite");
    botOutgoingPacketHandlers.AddHandler(SMSG_GUILD_INVITE, "guild accept");
    botOutgoingPacketHandlers.AddHandler(BUY_ERR_NOT_ENOUGHT_MONEY, "not enough money");
    botOutgoingPacketHandlers.AddHandler(BUY_ERR_REPUTATION_REQUIRE, "not enough reputation");
    botOutgoingPacketHandlers.AddHandler(SMSG_GROUP_SET_LEADER, "group set leader");
    botOutgoingPacketHandlers.AddHandler(SMSG_FORCE_RUN_SPEED_CHANGE, "check mount state");
    botOutgoingPacketHandlers.AddHandler(SMSG_RESURRECT_REQUEST, "resurrect request");
    botOutgoingPacketHandlers.AddHandler(SMSG_INVENTORY_CHANGE_FAILURE, "cannot equip");
    botOutgoingPacketHandlers.AddHandler(SMSG_TRADE_STATUS, "trade status");
    botOutgoingPacketHandlers.AddHandler(SMSG_LOOT_RESPONSE, "loot response", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_ADD_KILL, "quest update add kill", true);
#ifndef MANGOSBOT_TWO
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_ADD_ITEM, "quest update add item", true);
#else
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_ADD_ITEM_OBSOLETE, "quest update add item", true);
#endif
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_FAILED, "quest update failed", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_FAILEDTIMER, "quest update failed timer", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTUPDATE_COMPLETE, "quest update complete", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_ITEM_PUSH_RESULT, "item push result", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_PARTY_COMMAND_RESULT, "party command");
    botOutgoingPacketHandlers.AddHandler(SMSG_LEVELUP_INFO, "levelup", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_LOG_XPGAIN, "xpgain", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_PVP_CREDIT, "honorgain", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_TEXT_EMOTE, "receive text emote");
    botOutgoingPacketHandlers.AddHandler(SMSG_EMOTE, "receive emote");
    botOutgoingPacketHandlers.AddHandler(SMSG_LOOT_START_ROLL, "loot start roll", true);
    botOutgoingPacketHandlers.AddHandler(SMSG_SUMMON_REQUEST, "summon request");
    botOutgoingPacketHandlers.AddHandler(MSG_RAID_READY_CHECK, "ready check");
    botOutgoingPacketHandlers.AddHandler(SMSG_QUEST_CONFIRM_ACCEPT, "confirm quest");
    botOutgoingPacketHandlers.AddHandler(SMSG_QUESTGIVER_QUEST_DETAILS, "quest details");   

#ifndef MANGOSBOT_ZERO
    botOutgoingPacketHandlers.AddHandler(SMSG_ARENA_TEAM_INVITE, "arena team invite");
#endif
#ifdef MANGOSBOT_TWO
    botOutgoingPacketHandlers.AddHandler(SMSG_LFG_ROLE_CHECK_UPDATE, "lfg role check");
    botOutgoingPacketHandlers.AddHandler(SMSG_LFG_PROPOSAL_UPDATE, "lfg proposal");
#endif

    botOutgoingPacketHandlers.AddHandler(SMSG_CAST_RESULT, "cast failed");
    botOutgoingPacketHandlers.AddHandler(SMSG_DUEL_REQUESTED, "duel requested");
    botOutgoingPacketHandlers.AddHandler(SMSG_INVENTORY_CHANGE_FAILURE, "inventory change failure");

    masterOutgoingPacketHandlers.AddHandler(SMSG_PARTY_COMMAND_RESULT, "party command");
#ifndef MANGOSBOT_ZERO
    masterOutgoingPacketHandlers.AddHandler(MSG_RAID_READY_CHECK_FINISHED, "ready check finished");
#endif

    if (!HasRealPlayerMaster() && bot->GetFreeTalentPoints() > 0)
    {
        DoSpecificAction("auto talents");
    }
}

PlayerbotAI::~PlayerbotAI()
{
    for (uint8 i = 0 ; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        if (engines[i])
            delete engines[i];
    }

    if (aiObjectContext)
        delete aiObjectContext;
}

void PlayerbotAI::FadePassiveAfter(uint32 ms)
{
    // | 1 so a start that lands exactly on 0 still reads as pending.
    passiveFadeStartMs = WorldTimer::getMSTime() | 1;
    passiveFadeDurationMs = ms;
}

void PlayerbotAI::UpdateAI(uint32 elapsed, bool minimal)
{
    AiObjectContext* context = aiObjectContext;
    std::string mapString = WorldPosition(bot).isInstance() ? "I" : std::to_string(bot->GetMapId());
    auto pmo = sPerformanceMonitor.start(PERF_MON_TOTAL, "PlayerbotAI::UpdateAI " + mapString, nullptr, bot->GetMapId(), bot->GetInstanceId());

    SC_PHASE("UpdateAI.entry", bot ? bot->GetName() : "(null)");

    // revalidate the
    // cached master pointer against ObjectAccessor BEFORE any code
    // path can deref it. If the master Player was destroyed since
    // SetMaster() was called (typical when master logs out /
    // disconnects), FindPlayer returns nullptr or a different live
    // pointer. We null `master` defensively. The masterGuid shadow
    // (set in SetMaster, see PlayerbotAI.h) is the safe lookup key —
    // we never deref the cached `master` pointer in this check.
    if (master)
    {
        Player* live = masterGuid ? sObjectAccessor.FindPlayer(masterGuid) : nullptr;
        if (live != master)
        {
            master = nullptr;
            masterGuid = ObjectGuid();
        }
    }

    // LOOT WATCHDOG. Opening a corpse sets UNIT_FLAG_LOOTING, the kneel, and only
    // a loot release clears it. A bot has no client to close a loot window, so
    // its one release is StoreLootAction, run from the "loot response" trigger.
    // That action is one-shot and can be lost: a state change (combat, death)
    // re-inits the engine and empties its queue, a multiplier can zero it, and
    // the queue expires it after ExpireActionTime. Then the bot kneels until the
    // next corpse it opens. The normal path answers in about lootDelay (1s), so
    // five seconds of the flag means the answer is not coming.
    if (bot->IsInWorld() && bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING))
    {
        uint32 now = WorldTimer::getMSTime();
        if (!lootingSinceMs)
            lootingSinceMs = now;
        else if (WorldTimer::getMSTimeDiff(lootingSinceMs, now) > 5000)
        {
            ObjectGuid lootGuid = bot->GetLootGuid();
            sLog.outString("[BOT LOOT] %s: loot watchdog released guid=%lu after 5s (state=%u combat=%u)",
                bot->GetName(), lootGuid.GetRawValue(), (uint32)currentState, bot->IsInCombat() ? 1 : 0);

            WorldPacket packet(CMSG_LOOT_RELEASE, 8);
            packet << lootGuid;
            bot->GetSession()->HandleLootReleaseOpcode(packet);
            // No loot guid means DoLootRelease never runs, so clear the flag itself.
            bot->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING);
            RESET_AI_VALUE(LootObject, "loot target");
            lootingSinceMs = 0;
        }
    }
    else
        lootingSinceMs = 0;

    // FOLLOW LETS GO. See FollowChatShortcutAction: the passive it imposes lifts
    // on this timer, and anything that means to keep passive on (flee, the
    // security check) cancels the timer as it sets it.
    if (passiveFadeStartMs && WorldTimer::getMSTimeDiff(passiveFadeStartMs, WorldTimer::getMSTime()) >= passiveFadeDurationMs)
    {
        passiveFadeStartMs = 0;
        if (HasStrategy("passive", BotState::BOT_STATE_NON_COMBAT) || HasStrategy("passive", BotState::BOT_STATE_COMBAT))
        {
            ChangeStrategy("-passive", BotState::BOT_STATE_NON_COMBAT);
            ChangeStrategy("-passive", BotState::BOT_STATE_COMBAT);
        }
    }

    if(aiInternalUpdateDelay > elapsed)
    {
        aiInternalUpdateDelay -= elapsed;
    }
    else
    {
        aiInternalUpdateDelay = 0;
        isWaiting = false;
    }

    // cancel logout in combat
    if (bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut())
    {
        if (sServerFacade.IsInCombat(bot) || (master && sServerFacade.IsInCombat(master) && sServerFacade.GetDistance2d(bot, master) < 30.0f))
        {
            WorldPacket p;
            bot->GetSession()->HandleLogoutCancelOpcode(p);
            TellPlayer(GetMaster(), BOT_TEXT("logout_cancel"));
        }
    }

    // Leontiesh - fix movement desync
    bool botMoving = false;
    if (!bot->IsStopped() || bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE) 
        botMoving = true;
    if (!bot->GetMotionMaster()->empty())
        if (MovementGenerator* movgen = bot->GetMotionMaster()->top())
            botMoving = true;


    if (botMoving && !bot->IsBeingTeleported() && bot->IsInWorld())
    {
        isMoving = true;

        GuidPosition lootObject(bot->GetLootGuid(), bot);

        bool shouldRelease = true;

        if (lootObject)
        {
            if (lootObject.IsGameObject() && lootObject.GetGameObjectInfo()->type == GAMEOBJECT_TYPE_FISHINGNODE)
                shouldRelease = false;
            else if (lootObject.GetWorldObject(bot->GetInstanceId()) && bot->GetDistance(lootObject.GetWorldObject(bot->GetInstanceId())) < INTERACTION_DISTANCE)
                shouldRelease = false;

            
            if (Loot* loot = sLootMgr.GetLoot(bot, bot->GetLootGuid()))
            {
                if (lootObject.IsUnit())
                {
                    Unit* unitTarget = (Unit*)loot->GetLootTarget();
                    // Unit doesn't have m_loot; only Creature does.
                    Loot* utLoot = (unitTarget && unitTarget->GetTypeId() == TYPEID_UNIT) ? ((Creature*)unitTarget)->m_loot : nullptr;
                    // LootAccess wraps Loot* now (no more reinterpret_cast layout-cheat).
                    LootAccess lootAccess(utLoot);
                    if (utLoot && lootAccess.playersLooting().count(bot->GetObjectGuid()) == 0)
                    {
                        // You shouldn't release if you aren't looting already
                        shouldRelease = false;
                    }
                }
                else if (lootObject.IsGameObject())
                {
                    GameObject* gameobjectTarget = (GameObject*)loot->GetLootTarget();
                    // LootAccess wraps Loot* now.
                    LootAccess lootAccess(gameobjectTarget ? gameobjectTarget->m_loot : nullptr);
                    if (!lootAccess.playersLooting().count(bot->GetObjectGuid()))
                    {
                        // You shouldn't release if you aren't looting already
                        shouldRelease = false;
                    }
                }
                else
                {
                    // If it's not a unit or GO, what looting would this be?
                    // safe to assume you probably shouldn't release
                    shouldRelease = false;
                }
                // release loot if moving and far from object.
                if (shouldRelease)
                {
                    loot->Release(bot);
                }
            }
        }
    }
    else if (isMoving)
    {
        if (!bot->IsTaxiFlying())
            StopMoving();

        isMoving = false;
    }

#ifdef MANGOSBOT_TWO
    if (bot->IsPendingDismount())
        bot->ResolvePendingUnmount();
    else
        bot->ResolvePendingMount();
#endif

    // wake up if in combat
    bool isCasting = bot->IsNonMeleeSpellCasted(true);
    if (sServerFacade.IsInCombat(bot))
    {
        if (!inCombat && !isCasting && !isWaiting)
        {
            ResetAIInternalUpdateDelay();
            // REACT_ACTIVITY has its own cache slot (separate from ALL_ACTIVITY above) that
            // can still be holding a stale "inactive" verdict from just before combat started -
            // force a fresh check now instead of waiting up to 5s for it to expire on its own.
            AllowActivity(REACT_ACTIVITY, true);
        }
        else if (!AllowActivity())
        {
            if (AllowActivity(ALL_ACTIVITY, true))
                ResetAIInternalUpdateDelay();
        }

        inCombat = true;
    }
    else
    {
        if (inCombat && !isCasting && !isWaiting)
            ResetAIInternalUpdateDelay();

        inCombat = false;
    }

    // force stop if moving but should not
#ifndef MANGOSBOT_TWO
    if (!bot->IsStopped() && !IsJumping() && !CanMove() && !bot->IsTaxiFlying() && !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING) && !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED))
#else
    if (!bot->IsStopped() && !IsJumping() && !CanMove() && !bot->m_movementInfo.HasMovementFlag(MOVEFLAG_FALLING) && !bot->IsTaxiFlying() && !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING) && !bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED))
#endif
    {
        StopMoving();
    }

    // land after knockback/jump
    uint32 curTime = sWorld.GetCurrentMSTime();
    if (jumpTime && (jumpTime < curTime || (jumpTime + 10000 < curTime)))
    {
        // might be not needed
        if (GetJumpDestination())
        {
            bot->Relocate(jumpDestination.getX(), jumpDestination.getY(), jumpDestination.getZ());
        }

        // normal landing
        if (!fallAfterJump)
        {
            bot->m_movementInfo.AddMovementFlag(MOVEFLAG_FALLINGFAR);

            WorldPacket stop(MSG_MOVE_STOP);
#ifdef MANGOSBOT_TWO
            stop << bot->GetObjectGuid().WriteAsPacked();
#endif
            stop << bot->m_movementInfo;
            QueuePacket(stop);

            bot->m_movementInfo.SetMovementFlags(MOVEFLAG_NONE);
            bot->m_movementInfo.jump = MovementInfo::JumpInfo();

            WorldPacket land(MSG_MOVE_FALL_LAND);
#ifdef MANGOSBOT_TWO
            land << bot->GetObjectGuid().WriteAsPacked();
#endif
            land << bot->m_movementInfo;
            QueuePacket(land);
            sLog.outDetail("%s: Jump: Landed, landTime: %u", bot->GetName(), curTime);

            // Player::IsFalling() (m_fallStartZ != 0, set by SetFallInformation() above
            // this block and in the knockback handler) is what actually gates CanMove().
            // It's meant to clear via Player::UpdateFallInformationIfNeed() when the
            // MSG_MOVE_FALL_LAND packet just queued above is processed by
            // WorldSession::HandleMovementOpcodes - but that processing is unreliable for
            // an AI-synthesized packet (dropped if the bot's movespline isn't finalized,
            // rejected by anti-cheat, etc.), and when it's dropped m_fallStartZ is never
            // reset, permanently blocking the bot from moving again. Clear it here
            // directly so landing doesn't depend on that fragile round-trip.
            bot->SetFallInformation(0, 0.0f);

            jumpTime = 0;
            fallAfterJump = false;
            ResetJumpDestination();

            bot->InterruptMoving();
        }
        // falling after hitting something
        else
        {
            //bot->SetFallInformation(0, bot->m_movementInfo.pos.z);
#ifdef MANGOSBOT_ZERO
            bot->m_movementInfo.AddMovementFlag(MOVEFLAG_JUMPING);
#else
            bot->m_movementInfo.AddMovementFlag(MOVEFLAG_FALLING);
#endif
            // use motion master (disabled for now, makes bot move to ceiling it just hit)
            static bool useMoveFall = false;
            if (useMoveFall && bot->GetMotionMaster()->MoveFall())
            {
                jumpTime = 0;
                fallAfterJump = false;
                ResetJumpDestination();
                sLog.outDetail("%s: Jump: MoveFall activated", bot->GetName());
            }
            // simulate falling
            else
            {
                float landingHeight = bot->m_movementInfo.pos.z;
                bot->UpdateAllowedPositionZ(bot->m_movementInfo.pos.x, bot->m_movementInfo.pos.y, landingHeight);

                // calculate fall time
                float gravity = 19.2911f;
                float terminalVelocity = 60.148f;
                float time;

                const float terminal_length = float(terminalVelocity* terminalVelocity) / (2.f* gravity);
                const float terminalFallTime = float(terminalVelocity / gravity);

                float path_length = fabs(bot->m_movementInfo.pos.z - landingHeight);
                if (path_length >= terminal_length)
                    time = (path_length - terminal_length) / terminalVelocity + terminalFallTime;
                else
                    time = sqrtf(2.f * path_length / gravity);

                SetJumpTime(curTime + uint32(time * static_cast<uint32>(IN_MILLISECONDS)) + 1000);
                fallAfterJump = false;
                jumpDestination = WorldPosition(bot->GetMapId(), bot->m_movementInfo.pos.x, bot->m_movementInfo.pos.y, landingHeight);
                sLog.outDetail("%s: Jump: Falling simulated, height: %f, timeToLand %u", bot->GetName(), landingHeight, jumpTime);
            }
        }
    }

    // cheat options
    if (bot->IsAlive() && ((uint32)GetCheat() > 0 || (uint32)sPlayerbotAIConfig.botCheatMask > 0))
    {
        if (HasCheat(BotCheatMask::health))
            bot->SetHealthPercent(100);
        if (HasCheat(BotCheatMask::mana) && bot->GetPowerType() == POWER_MANA)
            bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
        if (HasCheat(BotCheatMask::power) && bot->GetPowerType() != POWER_MANA)
            bot->SetPower(bot->GetPowerType(), bot->GetMaxPower(bot->GetPowerType()));
        if (HasCheat(BotCheatMask::cooldown))
            bot->RemoveAllCooldowns();
        if (HasCheat(BotCheatMask::repair))
        {
            // The "repair" cheat is meant to mean gear never actually breaks, but nothing
            // ever called PlayerbotAI::DurabilityLoss() (the only place that checked this
            // cheat before applying durability loss) - normal combat/death durability loss
            // goes through the core engine directly and bypasses it entirely. Left
            // unrepaired, a broken (0-durability) weapon makes every spell that needs it
            // fail CanCastSpell with SPELL_FAILED_EQUIPPED_ITEM_CLASS forever, regardless of
            // range, since that failure has nothing to do with distance - this is what was
            // permanently soft-locking hunters on their current target. Only pay for the
            // repair scan when something is actually below full durability.
            bool needsRepair = false;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (item && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) &&
                    item->GetUInt32Value(ITEM_FIELD_DURABILITY) < item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
                {
                    needsRepair = true;
                    break;
                }
            }
            if (needsRepair)
                bot->DurabilityRepairAll(false, 0.0f);
        }
        if (HasCheat(BotCheatMask::movespeed))
        {
            bot->UpdateSpeed(MOVE_WALK, true, 10);
            bot->UpdateSpeed(MOVE_RUN, true, 10);
            bot->UpdateSpeed(MOVE_SWIM, true, 10);
        }
        if (HasCheat(BotCheatMask::breath))
        {
            bot->SetWaterBreathingIntervalMultiplier(0);
        }
        if (HasCheat(BotCheatMask::item) && (bot->getClass() == CLASS_HUNTER || bot->getClass() == CLASS_ROGUE || bot->getClass() == CLASS_WARRIOR))
        {
            uint32 itemId = bot->GetUInt32Value(PLAYER_AMMO_ID);
            if (itemId && bot->GetItemCount(itemId))
            {                
                std::list<Item*> items = AI_VALUE2(std::list<Item*>, "inventory items", this->chatHelper.formatQItem(itemId));

                for (auto item : items)
                {
                    if (bot->getClass() == CLASS_HUNTER && item->GetProto()->SubClass == ITEM_SUBCLASS_WEAPON_THROWN) //Do not replenish thrown weapons for hunters.
                        break;

                    item->SetCount(item->GetProto()->GetMaxStackSize());
                    break;
                }
            }
            else
            {
#ifndef MANGOSBOT_ZERO
                if (!bot->HasAura(46699)) // Thori'dal
                {
                    PlayerbotFactory(bot, bot->GetLevel(), 0).InitAmmo();
                }
#else
                PlayerbotFactory(bot, bot->GetLevel(), 0).InitAmmo();
#endif
            }
        }
    }

    // Alt level sync - level up non-random bots to match master level
    if (sPlayerbotAIConfig.syncAltLevelToMaster && bot->IsAlive() && master && !sRandomPlayerbotMgr.IsRandomBot(bot)
        && bot->GetGroup() && master->GetGroup() && IsSafe(master) && bot->GetGroup()->GetLeaderGuid() == master->GetObjectGuid())
    {
        uint32 botLevel = bot->GetLevel();
        uint32 masterLevel = master->GetLevel();
        uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

        if (botLevel < masterLevel && botLevel < maxLevel)
        {
            bot->GiveLevel(botLevel + 1);
         
            if (sPlayerbotAIConfig.hasLog("bot_events.csv"))
            {
                sPlayerbotAIConfig.logEvent(this, "AltLevelSync", ObjectGuid(),
                    "Leveled from " + std::to_string(botLevel) + " to " + std::to_string(bot->GetLevel()) +
                    " (master: " + std::to_string(masterLevel) + ")");
            }
        }
    }

    if (master && IsSafe(master) && bot->GetDistance(master) < INTERACTION_DISTANCE * 2.5 && master->GetTransport() != bot->GetTransport() && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
    {
        bot->StopMoving();
        if (master->GetTransport() && WorldPosition(bot).isOnTransport(master->GetTransport()))
            master->GetTransport()->AddPassenger(bot);
        else if (bot->GetTransport())
        {
            WorldPosition botPos(bot);
            bot->GetTransport()->RemovePassenger(bot);
            bot->NearTeleportTo(bot->m_movementInfo.pos.x, bot->m_movementInfo.pos.y, bot->m_movementInfo.pos.z, bot->m_movementInfo.pos.o);
            MANGOS_ASSERT(botPos.fDist(bot) < 500.0f);
        }
    }
    else if (!HasRealPlayerMaster() && !bot->IsBeingTeleported() && bot->GetTransport() && bot->GetMapId() == bot->GetTransport()->GetMapId() && !WorldPosition(bot).isOnTransport(bot->GetTransport()) && !isMovingToTransport)
    {
        if (HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
        {
            TellPlayer(GetMaster(), "Jumping off " + std::string(bot->GetTransport()->GetName()));
        }

        WorldPosition botPos(bot);
        bot->GetTransport()->RemovePassenger(bot);
        bot->NearTeleportTo(bot->m_movementInfo.pos.x, bot->m_movementInfo.pos.y, bot->m_movementInfo.pos.z, bot->m_movementInfo.pos.o);
        MANGOS_ASSERT(botPos.fDist(bot) < 500.0f);
        bot->StopMoving();
    }

    // Update facing
    SC_PHASE("UpdateAI.UpdateFaceTarget", bot ? bot->GetName() : "(null)");
    UpdateFaceTarget(elapsed, minimal);

    bool doMinimalReaction = minimal || !AllowActivity(REACT_ACTIVITY);

#ifdef PLAYERBOT_ELUNA
    // used by eluna
    if (Eluna* e = bot->GetEluna())
        e->OnUpdateAI(this, bot->GetName());
#endif

    // Sitting down to eat sleeps the internal AI for as long as the meal lasts
    // - up to 27 seconds - and that sleep is deliberate: it is the only thing
    // stopping the follow strategy from standing the bot up after every
    // mouthful. It needs an escape, though, or a master who walks off leaves
    // the bot sitting in an empty room until the food runs out.
    //
    // So: while the bot is sitting and asleep, wake it the moment the master is
    // past the eat/drink leash. Standing up ends the meal on its own, because
    // both auras carry AURA_INTERRUPT_FLAG_NOT_SEATED. Cheap enough to sit on
    // the hot path - it only runs while the AI is already asleep, and the first
    // test is a byte compare.
    if (!CanUpdateAIInternal() && bot->getStandState() == UNIT_STAND_STATE_SIT && master && IsSafe(master))
    {
        const float leash = sPlayerbotAIConfig.EatDrinkMinDistance +
            ((bot->GetGroup() && bot->GetGroup()->IsRaidGroup()) ? sPlayerbotAIConfig.raidFollowDistance
                                                                 : sPlayerbotAIConfig.followDistance);

        if (bot->GetMapId() != master->GetMapId() || !bot->IsWithinDist(master, leash))
            ResetAIInternalUpdateDelay();
    }

    // Only update the internal ai when no reaction is running and the internal ai can be updated
    SC_PHASE("UpdateAI.UpdateAIReaction", bot ? bot->GetName() : "(null)");
    if(!UpdateAIReaction(elapsed, doMinimalReaction, bot->IsTaxiFlying()) && CanUpdateAIInternal())
    {
        // Update the delay with the spell cast time
        Spell* currentSpell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (currentSpell && (currentSpell->getState() == SPELL_STATE_CASTING) && (currentSpell->GetCastedTime() > 0U))
        {
            SetAIInternalUpdateDelay(currentSpell->GetCastedTime() + sPlayerbotAIConfig.reactDelay + sWorld.GetAverageDiff());

            // Cancel the update if the new delay increased
            if (!CanUpdateAIInternal())
            {
                return;
            }
        }

        SC_PHASE("UpdateAI.UpdateAIInternal", bot ? bot->GetName() : "(null)");
        UpdateAIInternal(elapsed, minimal);

        bool min = minimal;
        // test fix lags because of BG
        if (!inCombat)
            min = true;

        if (bot && HasRealPlayerMaster())
            min = false;

        SC_PHASE("UpdateAI.YieldAIInternalThread", bot ? bot->GetName() : "(null)");
        YieldAIInternalThread(min);
    }
    SC_PHASE("UpdateAI.exit", bot ? bot->GetName() : "(null)");
}

bool PlayerbotAI::UpdateAIReaction(uint32 elapsed, bool minimal, bool isStunned)
{
    bool reactionFound;
    std::string mapString = WorldPosition(bot).isInstance() ? "I" : std::to_string(bot->GetMapId());
    auto pmo = sPerformanceMonitor.start(PERF_MON_TOTAL, "PlayerbotAI::UpdateAIReaction " + mapString, nullptr, bot->GetMapId(), bot->GetInstanceId());
    const bool reactionInProgress = reactionEngine->Update(elapsed, minimal, isStunned, reactionFound);
    pmo.reset();

    if(reactionFound)
    {
        // If new reaction found force stop current actions (if required)
        const Reaction* reaction = reactionEngine->GetReaction();
        if(reaction)
        {
            if(reaction->ShouldInterruptCast())
            {
                InterruptSpell();
            }

            if (reaction->ShouldInterruptMovement())
            {
                StopMoving();
            }
        }
    }

    return reactionInProgress;
}

void PlayerbotAI::UpdateFaceTarget(uint32 elapsed, bool minimal)
{
    faceTargetUpdateDelay = faceTargetUpdateDelay > elapsed ? faceTargetUpdateDelay - elapsed : 0U;
    if (faceTargetUpdateDelay <= 0U)
    {
        // Only update the target facing when in combat
        if (IsStateActive(BotState::BOT_STATE_COMBAT))
        {
            // Don't update facing if bot is moving
            if (!sServerFacade.isMoving(bot) && !bot->isMovingOrTurning())
            {
                AiObjectContext* context = GetAiObjectContext();
                Unit* target = AI_VALUE(Unit*, "current target");
                if(target)
                {
                    // Do not update the facing while pulling
                    Unit* pullTarget = AI_VALUE(Unit*, "pull target");
                    if (pullTarget == nullptr)
                    {
                        if (!AI_VALUE2(bool, "facing", "current target"))
                        {
                            if (!sServerFacade.UnitIsDead(bot) &&
                                !sServerFacade.IsFrozen(bot) &&
                                !sServerFacade.IsCharmed(bot) &&
                                !bot->IsPolymorphed() &&
                                !bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) &&
                                !bot->IsBeingTeleported() &&
                                !bot->HasAuraType(SPELL_AURA_MOD_CONFUSE) &&
                                !bot->HasAuraType(SPELL_AURA_MOD_STUN) &&
                                !bot->IsTaxiFlying() &&
                                !bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
                            {
                                sServerFacade.SetFacingTo(bot, target);
                                bot->SetInFront(target);
                                WorldPacket data(MSG_MOVE_SET_FACING);
                                data << bot->GetPackGUID();
                                data << bot->m_movementInfo;
                                bot->GetMover()->SendMessageToSetExcept(data, bot);
                            }
                        }
                    }
                }
            }
        }

        faceTargetUpdateDelay = minimal ? sPlayerbotAIConfig.reactDelay * 10 : sPlayerbotAIConfig.reactDelay * 5;
    }
}

void PlayerbotAI::SetActionDuration(const Action* action)
{
    if (action)
    {
        if(action->IsReaction())
        {
            reactionEngine->SetReactionDuration(action);
        }
        else
        {
            SetAIInternalUpdateDelay(action->GetDuration());
        }
    }
}

void PlayerbotAI::SetActionDuration(uint32 duration)
{
    SetAIInternalUpdateDelay(duration);
}

const Action* PlayerbotAI::GetLastExecutedAction(BotState state) const
{
    const Engine* engine = engines[(uint8)state];
    if(engine)
    {
        return engine->GetLastExecutedAction();
    }

    return nullptr;
}

bool PlayerbotAI::IsImmuneToSpell(uint32 spellId) const
{
    for (std::list<uint32>::iterator i = sPlayerbotAIConfig.immuneSpellIds.begin(); i != sPlayerbotAIConfig.immuneSpellIds.end(); ++i)
    {
        if (spellId == (*i))
        {
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::IsInPve()
{
    return !IsInPvp() && !IsInRaid();
}

bool PlayerbotAI::IsInPvp()
{
    if (IsSafe(bot))
    {
        const bool inDuel = bot->m_duel && bot->m_duel->opponent;
        if (!inDuel)
        {
            const bool inBattleground = bot->InBattleGround();
            bool inArena = false;
#ifndef MANGOSBOT_ZERO
            inArena = bot->InArena();
#endif
            if (!inBattleground && !inArena)
            {
                AiObjectContext* context = aiObjectContext;
                const bool isPlayerNear = AI_VALUE(bool, "has enemy player targets");
                if (!isPlayerNear)
                {
                    return false;
                }
            }
        }

        return true;
    }

    return false;
}

bool PlayerbotAI::IsInRaid()
{
    bool inRaidFight = false;
    if (IsSafe(bot))
    {
        const Map* map = bot->FindMap();
        if (map && (map->IsDungeon() || map->IsRaid()))
        {
            inRaidFight = true;
        }
        else if (!IsInPvp() && GetState() == BotState::BOT_STATE_COMBAT)
        {
            AiObjectContext* context = GetAiObjectContext();
            const std::list<ObjectGuid>& attackers = AI_VALUE(std::list<ObjectGuid>, "attackers");
            for (const ObjectGuid& attackerGuid : attackers)
            {
                Creature* creature = GetCreature(attackerGuid);
                if (creature)
                {
                    const CreatureInfo* creatureInfo = creature->GetCreatureInfo();
                    if (creatureInfo)
                    {
                        if (creatureInfo->Rank == CREATURE_ELITE_WORLDBOSS)
                        {
                            inRaidFight = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    return inRaidFight;
}

PlayerTalentSpec PlayerbotAI::GetTalentSpec()
{
    return aiObjectContext->GetValue<PlayerTalentSpec>("talent spec")->Get();
}

void PlayerbotAI::UpdateTalentSpec(PlayerTalentSpec spec)
{
    if(spec == PlayerTalentSpec::TALENT_SPEC_INVALID)
    {
        int talentsTab = 0;
        if(bot->GetLevel() < 10)
        {
            switch (bot->getClass())
            {
                case CLASS_MAGE:
                {
                    talentsTab = 1;
                    break;
                }

                case CLASS_PALADIN:
                {
                    talentsTab = 0;
                    break;
                }

                case CLASS_PRIEST:
                {
                    talentsTab = 1;
                    break;
                }
            }
        }
        else
        {
            talentsTab = AiFactory::GetPlayerSpecTab(bot);
        }

        spec = PlayerTalentSpec(((bot->getClass() * 3) - 2) + talentsTab);
    }

    aiObjectContext->GetValue<PlayerTalentSpec>("talent spec")->Set(spec);
}

bool PlayerbotAI::CanEnterArea(const AreaTrigger* area)
{
    if (sRandomPlayerbotMgr.IsRandomBot(GetBot()))
    {
        DungeonPersistentState* state = bot->GetBoundInstanceSaveForSelfOrGroup(area->target_mapId);
        Map* map = sMapMgr.FindMap(area->target_mapId, state ? state->GetInstanceId() : 0);
        const MapEntry* mapEntry = sMapStore.LookupEntry(area->target_mapId);

        // check if this account try to abuse reseting instance
#ifdef MANGOSBOT_ZERO
        if (mapEntry->IsNonRaidDungeon())
#elif MANGOSBOT_ONE
        if (mapEntry->IsNonRaidDungeon() && ((map && map->GetDifficulty() == DUNGEON_DIFFICULTY_NORMAL) || (bot->GetDifficulty() == DUNGEON_DIFFICULTY_NORMAL)))
#else
        if (mapEntry->IsNonRaidDungeon() && ((map && map->GetDifficulty() == DUNGEON_DIFFICULTY_NORMAL) || (bot->GetDifficulty(false) == DUNGEON_DIFFICULTY_NORMAL)))
#endif
        {
            if (!bot->CanEnterNewInstance(state ? state->GetInstanceId() : 0))
            {
                return false;
            }
        }

        // Map's state check
        if (map && map->IsDungeon())
        {
            // cannot enter if the instance is full (player cap), GMs don't count, must not check when teleporting around the same map
            if (bot->GetMapId() != area->target_mapId)
            {
                if (((DungeonMap*)map)->GetPlayersCountExceptGMs() >= ((DungeonMap*)map)->GetMaxPlayers())
                {
                    return false;
                }

                // In Combat check
                if (map && map->GetInstanceData() && map->GetInstanceData()->IsEncounterInProgress())
                {
                    return false;
                }

                // Bind Checks
#ifdef MANGOSBOT_ZERO
                InstancePlayerBind* pBind = bot->GetBoundInstance(area->target_mapId);
#elif MANGOSBOT_ONE
                InstancePlayerBind* pBind = bot->GetBoundInstance(area->target_mapId, bot->GetDifficulty());
#else
                InstancePlayerBind* pBind = bot->GetBoundInstance(area->target_mapId, bot->GetDifficulty(mapEntry->IsRaid()));
#endif
                if (pBind && pBind->perm && pBind->state != state)
                {
                    return false;
                }

                if (pBind && pBind->perm && pBind->state != map->GetPersistentState())
                {
                    return false;
                }
            }
        }

        return true;
    }

    return false;
}

void PlayerbotAI::Unmount()
{
    if ((bot->IsMounted() || bot->GetMountID()) && !bot->IsTaxiFlying())
    {
        bot->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        bot->Unmount();

        bot->UpdateSpeed(MOVE_RUN, true);
        bot->UpdateSpeed(MOVE_RUN, false);

        if (bot->IsFlying())
        {
            bot->GetMotionMaster()->MoveFall();
        }
    }
}

bool PlayerbotAI::IsStateActive(BotState state) const
{
    return currentEngine == engines[(uint8)state];
}

time_t PlayerbotAI::GetCombatStartTime() const
{
    return aiObjectContext->GetValue<time_t>("combat start time")->Get();
}

void PlayerbotAI::OnCombatStarted()
{
    if(!IsStateActive(BotState::BOT_STATE_COMBAT))
    {
        // Reset the combat start timestamp
        aiObjectContext->GetValue<time_t>("combat start time")->Set(time(0));

        // Update stay position on location when combat starts
        if (HasStrategy("stay", BotState::BOT_STATE_COMBAT) && !HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT))
        {
            aiObjectContext->GetValue<PositionEntry>("pos", "stay")->Set(PositionEntry(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId()));
        }

        // Stop follow movement on combat start
        if (!(HasStrategy("follow", BotState::BOT_STATE_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_COMBAT)) &&
            (HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
        {
            StopMoving();
        }

        aiObjectContext->GetValue<std::list<ObjectGuid>>("attackers", 1)->Reset();
        aiObjectContext->GetValue<bool>("has attackers")->Reset();

        ChangeEngine(BotState::BOT_STATE_COMBAT);
        if (bot->IsSitState())
        {
            ResetAIInternalUpdateDelay();
        }
    }
}

void PlayerbotAI::OnCombatEnded()
{
    if (!IsStateActive(BotState::BOT_STATE_NON_COMBAT))
    {
        // Reset the combat start timestamp
        aiObjectContext->GetValue<time_t>("combat start time")->Set(0);

        // Stop following on combat end
        if ((HasStrategy("follow", BotState::BOT_STATE_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_COMBAT)) &&
            !(HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
        {
            StopMoving();
        }

        ChangeEngine(BotState::BOT_STATE_NON_COMBAT);
    }
}

void PlayerbotAI::OnDeath()
{
    if (!IsStateActive(BotState::BOT_STATE_DEAD) && !sServerFacade.IsAlive(bot))
    {
        // A bot that already has a corpse AND the ghost flag did not just die — it died
        // earlier and is corpse-running. The engine transiently leaves DEAD state during
        // the corpse run (e.g. on the graveyard teleport), so DeathTrigger
        // (!inDeadState && !isAlive) re-fires and OnDeath would otherwise log/count the
        // same death a second time — the "killer=none phantom death at the graveyard"
        // that inflated every death metric ~2x. A genuine fresh death always has no corpse
        // and no ghost flag yet (both spawn/set only after death, during release-spirit).
        // Restore the DEAD engine state so the corpse run continues, then bail without
        // re-logging or re-incrementing the death count.
        if (bot->GetCorpse() && bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        {
            ChangeEngine(BotState::BOT_STATE_DEAD);
            return;
        }

        StopMoving();

        Player* master = GetMaster();
        AiObjectContext* context = aiObjectContext;
        if (!HasActivePlayerMaster() && !bot->InBattleGround())
        {
            SET_AI_VALUE(uint32, "death count", AI_VALUE(uint32, "death count") + 1);

            if (sPlayerbotAIConfig.hasLog("deaths.csv"))
            {
                WorldPosition botPos(bot);

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);

                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                float subLevel = ((float)bot->GetLevel() + ((float)bot->GetUInt32Value(PLAYER_XP) / (float)bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)));

                out << subLevel << ",";

                botPos.printWKT(out);

                AiObjectContext* context = GetAiObjectContext();

                Unit* ctarget = AI_VALUE(Unit*, "current target");

                if (ctarget)
                {
                    out << "\"" << ctarget->GetName() << "\"," << ctarget->GetLevel() << "," << ctarget->GetHealthPercent() << ",";
                }
                else
                    out << "\"none\",0,100,";

                std::list<ObjectGuid> targets = AI_VALUE_LAZY(std::list<ObjectGuid>, "all targets");

                out << "\"";

                uint32 adds = 0;

                for (auto target : targets)
                {
                    if (!target.IsCreature())
                        continue;

                    Unit* unit = GetUnit(target);
                    if (!unit)
                        continue;

                    if (unit->GetVictim() != bot)
                        continue;

                    if (unit == ctarget)
                        continue;

                    out << unit->GetName() << "(" << unit->GetLevel() << ")";

                    adds++;
                }

                out << "\"," << std::to_string(adds);

                TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
                std::string travelInfo = "none";
                if (travelTarget && travelTarget->GetDestination())
                    travelInfo = travelTarget->GetDestination()->GetTitle();
                out << ",\"" << travelInfo << "\"";

                sPlayerbotAIConfig.log("deaths.csv", out.str().c_str());
            }
        }

        SET_AI_VALUE(Unit*, "current target", nullptr);
        SET_AI_VALUE(Unit*, "enemy player target", nullptr);
        SET_AI_VALUE(Unit*, "pull target", nullptr);
        SET_AI_VALUE(ObjectGuid, "attack target", ObjectGuid());
        SET_AI_VALUE(LootObject, "loot target", LootObject());
        SET_AI_VALUE(time_t, "combat start time", 0);
        SET_AI_VALUE2(bool, "manual bool", "enemies near corpse", false);
        SET_AI_VALUE2(bool, "manual bool", "enemies near graveyard", false);
        ChangeEngine(BotState::BOT_STATE_DEAD);
    }
}

void PlayerbotAI::OnResurrected()
{
    if (IsStateActive(BotState::BOT_STATE_DEAD) && sServerFacade.IsAlive(bot))
    {
        // Stop following on resurrected
        if ((HasStrategy("follow", BotState::BOT_STATE_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_COMBAT)) &&
            !(HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
        {
            StopMoving();
        }

        ChangeEngine(BotState::BOT_STATE_NON_COMBAT);
    }
}

void PlayerbotAI::HandleCommands()
{
    ExternalEventHelper helper(aiObjectContext);
    std::list<ChatCommandHolder> delayed;
    while (!chatCommands.empty())
    {
        ChatCommandHolder holder = chatCommands.front();
        time_t checkTime = holder.GetTime();
        if (checkTime && time(0) < checkTime)
        {
            delayed.push_back(holder);
            chatCommands.pop();
            continue;
        }

        std::string command = holder.GetCommand();
        Player* owner = holder.GetOwner();
        if (!helper.ParseChatCommand(command, owner) && holder.GetType() == CHAT_MSG_WHISPER)
        {
            // AN UNRECOGNISED WHISPER IS DROPPED IN SILENCE, and that silence
            // is expensive. `+debug` is not a command -- the toggle needs an
            // engine prefix, `nc +debug`, which is what our own conf documents
            // next to DebugFilter -- but ParseChatCommand simply returns false
            // for it and the reply that used to sit here was commented out
            // upstream, so it looked like a broken feature rather than a typo. That cost an
            // evening on 2026-09-06, in a session where `debug` was the one
            // tool that would have shortened everything else.
            //
            // Reinstating the reply wholesale is NOT the fix: ParseChatCommand
            // returns false for anything ChatHelper cannot parse, so every bot
            // would answer "Unknown command" to ordinary conversation.
            //
            // A command opening with + - or ~ is unambiguous, though. Nobody
            // types that at a bot conversationally; it is always somebody
            // reaching for a strategy toggle and missing the prefix.
            if (owner && IsRealPlayer(owner) && !command.empty() &&
                (command[0] == '+' || command[0] == '-' || command[0] == '~'))
            {
                std::ostringstream out;
                out << "\"" << command << "\" needs an engine: \"nc " << command
                    << "\" out of combat, \"co " << command << "\" in combat."
                    << " Whisper \"nc ?\" to list what is on.";
                TellPlayer(owner, out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true);
            }
        }

        chatCommands.pop();
    }

    for (std::list<ChatCommandHolder>::iterator i = delayed.begin(); i != delayed.end(); ++i)
    {
        chatCommands.push(*i);
    }
}

void PlayerbotAI::UpdateAIInternal(uint32 elapsed, bool minimal)
{
    SC_PHASE("UpdateAIInternal.entry", bot ? bot->GetName() : "(null)");
    if (bot->IsBeingTeleported() || !bot->IsInWorld())
        return;

    std::string mapString = WorldPosition(bot).isInstance() ? "I" : std::to_string(bot->GetMapId());
    auto pmo = sPerformanceMonitor.start(PERF_MON_TOTAL, "PlayerbotAI::UpdateAIInternal " + mapString, nullptr, bot->GetMapId(), bot->GetInstanceId());

    ExternalEventHelper helper(aiObjectContext);

    // chat replies
    std::list<ChatQueuedReply> delayedResponses;
    {
        std::scoped_lock lock(chatRepliesMutex);
        while (!chatReplies.empty())
        {
            ChatQueuedReply holder = chatReplies.front();
            time_t checkTime = holder.m_time;
            if (checkTime && time(0) < checkTime)
            {
                delayedResponses.push_back(holder);
                chatReplies.pop();
                continue;
            }
            ChatReplyAction::ChatReplyDo(bot, holder.m_type, holder.m_guid1, holder.m_guid2, holder.m_msg, holder.m_chanName, holder.m_name);
            chatReplies.pop();
        }

        for (std::list<ChatQueuedReply>::iterator i = delayedResponses.begin(); i != delayedResponses.end(); ++i)
        {
            chatReplies.push(*i);
        }
    }
    // logout if logout timer is ready or if instant logout is possible
    if (bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut())
    {
        WorldSession* botWorldSessionPtr = bot->GetSession();
        bool logout = botWorldSessionPtr->ShouldLogOut(time(nullptr));
        if (!master || master->GetSession()->GetState() != WORLD_SESSION_STATE_READY)
            logout = true;

        if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || bot->IsTaxiFlying() ||
            botWorldSessionPtr->GetSecurity() >= (AccountTypes)sWorld.getConfig(CONFIG_UINT32_INSTANT_LOGOUT))
        {
            logout = true;
        }

        if (master && (master->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || master->IsTaxiFlying() ||
            (master->GetSession() && master->GetSession()->GetSecurity() >= (AccountTypes)sWorld.getConfig(CONFIG_UINT32_INSTANT_LOGOUT))))
        {
            logout = true;
        }

        if (logout && !bot->GetSession()->ShouldLogOut(time(nullptr)))
        {
            if (master && GetBotMgr(master))
            {
                GetBotMgr(master)->LogoutPlayerBot(bot->GetObjectGuid().GetRawValue());
            }
            else
            {
                sRandomPlayerbotMgr.LogoutPlayerBot(bot->GetObjectGuid().GetRawValue());
            }
            return;
        }

        SetAIInternalUpdateDelay(sPlayerbotAIConfig.reactDelay);
        return;
    }

    SC_PHASE("UpdateAIInternal.botOutgoingPackets", bot ? bot->GetName() : "(null)");
    botOutgoingPacketHandlers.Handle(helper);
    SC_PHASE("UpdateAIInternal.masterIncomingPackets", bot ? bot->GetName() : "(null)");
    masterIncomingPacketHandlers.Handle(helper);
    SC_PHASE("UpdateAIInternal.masterOutgoingPackets", bot ? bot->GetName() : "(null)");
    masterOutgoingPacketHandlers.Handle(helper);

    /*
     * A BOT NPC DOES NOTHING. It stands there.
     *
     * Everything above this line still runs -- packet handling, the logout
     * path -- so the session stays healthy and a whisper still reaches it.
     * DoNextAction is where the combat, non-combat and reaction engines pick a
     * target and act on it, and that is the whole of what an NPC must not do.
     *
     * Chat replies used to be on that list of things that still run, and that
     * was the bug rather than the design: the reply queue is drained at the top
     * of this function, so a bot NPC answered anyone talking near it. It is
     * refused at QueueChatResponse now, which is above this gate as well.
     *
     * Found the hard way: the first bot NPC placed in the world re-rolled its
     * gear on a loop and then wandered off to attack something. Both are the AI
     * behaving exactly as designed for a bot, which is the point -- a character
     * conscripted as scenery needs the AI switched OFF, not tuned down. A
     * "passive" strategy set would have been the tuning-down answer and would
     * have left every future strategy free to reintroduce the problem.
     *
     * The delay keeps it out of the hot path rather than re-entering every tick.
     */
    if (bot->IsBotNpc())
    {
        SetAIInternalUpdateDelay(sPlayerbotAIConfig.reactDelay);
        SC_PHASE("UpdateAIInternal.exit", bot ? bot->GetName() : "(null)");
        return;
    }

    SC_PHASE("UpdateAIInternal.DoNextAction", bot ? bot->GetName() : "(null)");
	DoNextAction(minimal);
    SC_PHASE("UpdateAIInternal.exit", bot ? bot->GetName() : "(null)");
}

void PlayerbotAI::HandleTeleportAck()
{
    if (IsRealPlayer() && bot->IsBeingTeleportedFar())
        return;

    StopMoving();

	if (bot->IsBeingTeleportedNear())
	{
        if (!bot->GetMotionMaster()->empty())
            if (MovementGenerator* movgen = bot->GetMotionMaster()->top())
                movgen->Interrupt(*bot);

       /* WorldLocation dest = bot->GetTeleportDest();
        bot->Relocate(dest.coord_x, dest.coord_y, dest.coord_z, dest.orientation);*/

		WorldPacket p = WorldPacket(MSG_MOVE_TELEPORT_ACK, 8 + 4 + 4);
#ifdef MANGOSBOT_TWO
        p << bot->GetObjectGuid().WriteAsPacked();
#else
        p << bot->GetObjectGuid();
#endif
		p << bot->GetLastCounterForMovementChangeType(TELEPORT); // movement counter the core is waiting to ACK
		p << (uint32) time(0); // time - not currently used
        bot->GetSession()->HandleMoveTeleportAckOpcode(p);

        // add delay to simulate teleport delay
        SetAIInternalUpdateDelay(urand(1000, 2000));
	}
	else if (bot->IsBeingTeleportedFar())
	{
        bot->GetSession()->HandleMoveWorldportAckOpcode();

        // add delay to simulate teleport delay
        SetAIInternalUpdateDelay(urand(2000, 5000));
	}

    if (IsRealPlayer())
        bot->SendHeartBeat();

    Reset();
}

void PlayerbotAI::Reset(bool full)
{
    AiObjectContext* context = aiObjectContext;

    if (bot->IsTaxiFlying())
        return;

    if (!HasActivePlayerMaster() && currentEngine == engines[(uint8)BotState::BOT_STATE_COMBAT] && sServerFacade.IsInCombat(bot) && time(0) - AI_VALUE(time_t,"combat start time") > 5 * MINUTE)
    {
        bot->CombatStop();
    }

    currentEngine = engines[(uint8)BotState::BOT_STATE_NON_COMBAT];
    currentState = BotState::BOT_STATE_NON_COMBAT;
    ResetAIInternalUpdateDelay();
    reactionEngine->Reset();
    whispers.clear();

    PullStrategy* strategy = PullStrategy::Get(this);
    if (strategy)
        strategy->OnPullEnded();

    RESET_AI_VALUE(Unit*,"old target");
    RESET_AI_VALUE(Unit*,"current target");
    RESET_AI_VALUE(Unit*,"pull target");
    RESET_AI_VALUE(ObjectGuid,"attack target");
    RESET_AI_VALUE(GuidPosition,"rpg target");
    RESET_AI_VALUE(LootObject,"loot target");
    RESET_AI_VALUE(uint32,"lfg proposal");
    RESET_AI_VALUE(time_t,"combat start time");
    bot->SetSelectionGuid(ObjectGuid());

    LastSpellCast & lastSpell = AI_VALUE(LastSpellCast&,"last spell cast");
    lastSpell.Reset();

    if (bot->GetTradeData())
        bot->TradeCancel(true);

    if (full)
    {
        RESET_AI_VALUE(LastMovement&,"last movement");
        RESET_AI_VALUE(LastMovement&,"last area trigger");
        RESET_AI_VALUE(LastMovement&,"last taxi");

        TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");
        sTravelMgr.SetNullTravelTarget(target);
        target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        target->SetExpireIn(1000);

        *AI_VALUE(FutureDestinations*, "future travel destinations") = FutureDestinations();
        RESET_AI_VALUE2(std::string, "manual string", "future travel purpose");
        RESET_AI_VALUE2(int, "manual int", "future travel relevance");

        InterruptSpell();

        StopMoving();

        jumpTime = 0;
        fallAfterJump = false;
        ResetJumpDestination();

        WorldSession* botWorldSessionPtr = bot->GetSession();
        bool logout = botWorldSessionPtr->ShouldLogOut(time(nullptr));

        // cancel logout
        if (!logout && (bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut()))
        {
            WorldPacket p;
            bot->GetSession()->HandleLogoutCancelOpcode(p);
            TellPlayer(GetMaster(), BOT_TEXT("logout_cancel"));
        }
    }

    AI_VALUE(std::set<ObjectGuid>&,"ignore rpg target").clear();

    if (bot->IsTaxiFlying())
    {
#ifdef MANGOS
        bot->m_taxi.ClearTaxiDestinations();
#endif
        bot->OnTaxiFlightEject(true);
    }

    if (full)
    {
        for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        {
            engines[i]->Init();
        }
    }
}

std::map<std::string,ChatMsg> chatMap;

bool PlayerbotAI::IsAllowedCommand(std::string text)
{
    if (unsecuredCommands.empty())
    {
        unsecuredCommands.insert("who");
        unsecuredCommands.insert("where");
        unsecuredCommands.insert("wts");
        unsecuredCommands.insert("sendmail");
        unsecuredCommands.insert("invite");
        unsecuredCommands.insert("leave");
        unsecuredCommands.insert("join");
        unsecuredCommands.insert("lfg");
        unsecuredCommands.insert("guild invite");
        unsecuredCommands.insert("guild leave");
    }

    for (std::set<std::string>::iterator i = unsecuredCommands.begin(); i != unsecuredCommands.end(); ++i)
    {
        if (text.find(*i) == 0)
        {
            return true;
        }
    }

    return false;
}

void PlayerbotAI::HandleCommand(uint32 type, const std::string& text, Player& fromPlayer, const uint32 lang)
{
    std::string filtered = text;

    if (!IsAllowedCommand(filtered) && !GetSecurity()->CheckLevelFor(PlayerbotSecurityLevel::PLAYERBOT_SECURITY_INVITE, type != CHAT_MSG_WHISPER, &fromPlayer))
        return;

    if (type == CHAT_MSG_ADDON)
        return;

    if (filtered.find("BOT\t") == 0) //Mangosbot has BOT prefix so we remove that.
        filtered = filtered.substr(4);
    else if (lang == LANG_ADDON) //Other addon messages should not command bots.
        return;

    if (type == CHAT_MSG_SYSTEM)
        return;

    if (filtered.find(sPlayerbotAIConfig.commandSeparator) != std::string::npos)
    {
        std::vector<std::string> commands;
        split(commands, filtered, sPlayerbotAIConfig.commandSeparator.c_str());
        for (std::vector<std::string>::iterator i = commands.begin(); i != commands.end(); ++i)
        {
            HandleCommand(type, *i, fromPlayer);
        }
        return;
    }

    if (!sPlayerbotAIConfig.commandPrefix.empty())
    {
        if (filtered.find(sPlayerbotAIConfig.commandPrefix) != 0)
            return;

        filtered = filtered.substr(sPlayerbotAIConfig.commandPrefix.size());
    }

    // "attack" means attack WHAT I HAVE SELECTED.
    //
    // It did not. `attack` runs MeleeAction, which targets the value
    // "current target" - and that is a MANUALLY SET value (CurrentTargetValue
    // holds a guid somebody called Set() with), not anything derived from the
    // person giving the order. After a retreat the bot has no current target,
    // so the order resolved to nothing and appeared to be ignored; typing it
    // again only worked when something else had picked a target in between,
    // which is exactly the "takes two goes" report.
    //
    // That breaks the ordinary dungeon body-pull - walk in, back off, stop,
    // fight where you chose - because the whole manoeuvre ends with an
    // explicit attack order on a target the bot is not already fighting.
    //
    // So adopt the orderer's selection, and remember the order so it outlives
    // this tick (see NoteMasterAttackOrder).
    {
        static const std::set<std::string> attackOrders = {
            "attack", "pull", "attack rti target", "pull rti target",
            "pull my target", "pull nearest target", "attack least hp target",
            "dps assist", "tank assist"
        };

        if (attackOrders.count(filtered))
        {
            NoteMasterAttackOrder();

            // "attack" also means GET INTO THE FIGHT, which for a healer means
            // start healing rather than start swinging.
            //
            // A bot only enters combat state through CombatStartTrigger, which
            // tests `has attackers` - a group member being attacked NEAR THIS
            // BOT. A healer that has retreated is by definition not near the
            // fight, so nothing is "near" it, so it never enters combat state,
            // so it runs the non-combat engine and simply follows. And it
            // cannot be rescued with `attack` either, because
            // AttackMyTargetAction::isUseful refuses the order outright for
            // anything carrying a heal strategy.
            //
            // That combination is why a retreating healer went quiet and
            // stayed quiet. A person saying "attack" has decided the fight is
            // on; take them at their word.
            // AND CLEAR PASSIVE, which is the real reason this was broken.
            //
            // `flee` is not a movement, it is a MODE. FleeChatShortcutAction
            // does ai->Reset() and then adds "+passive" to BOTH engines and
            // leaves it there:
            //
            //     ai->ChangeStrategy("+" + movement + ",+passive", NON_COMBAT);
            //     ai->ChangeStrategy("+" + movement + ",+passive", COMBAT);
            //
            // A passive bot does not fight and does not heal. `follow`, `stay`,
            // `guard`, `free` and `wander` all clear it; `attack` did not, so
            // after a retreat an attack order landed on a bot that had been
            // switched off, and looked like a command the bot had never heard
            // of. That is the whole "it takes two goes" story: the second
            // command only worked when something in between had cleared
            // passive.
            ChangeStrategy("-passive", BotState::BOT_STATE_NON_COMBAT);

            // AND TAKE THE MOVEMENT STRATEGY BACK OUT OF THE COMBAT ENGINE.
            //
            // FleeChatShortcutAction adds the default movement strategy -
            // "follow", for anything with a player master - to BOTH engines:
            //
            //     ai->ChangeStrategy("+" + movement + ",+passive", COMBAT);
            //
            // Follow does not belong in the combat engine and is never put
            // there deliberately: EnsureDefaultMovementStrategy only ever
            // touches REACTION and NON_COMBAT, and the `follow` command itself
            // strips it from combat ("-passive,-follow"). Left behind, the bot
            // attacks and is dragged back to its master on alternate ticks -
            // the "janky, switching between attacking and staying close to me"
            // report.
            //
            // Same list the follow shortcut uses, plus stay and wander so an
            // attack order is a clean slate whichever mode preceded it.
            ChangeStrategy("-passive,-follow,-stay,-wander", BotState::BOT_STATE_COMBAT);

            if (!IsStateActive(BotState::BOT_STATE_COMBAT))
                ChangeEngine(BotState::BOT_STATE_COMBAT);

            if (Unit* selected = fromPlayer.GetSelectedUnit())
            {
                // Same predicate the companion spike settled on: CanAttack
                // defers to the owner and is unhelpfully unforced, so ask the
                // plain questions instead.
                if (selected->IsAlive() &&
                    !bot->IsFriendlyTo(selected) &&
                    selected->IsTargetable(true, true))
                {
                    // Not SET_AI_VALUE here: that macro expands to `context->`,
                    // which is not in scope in this method.
                    aiObjectContext->GetValue<Unit*>("current target")->Set(selected);
                }
            }
        }
    }

    if (chatMap.empty())
    {
        chatMap["#w "] = CHAT_MSG_WHISPER;
        chatMap["#p "] = CHAT_MSG_PARTY;
        chatMap["#r "] = CHAT_MSG_RAID;
        chatMap["#a "] = CHAT_MSG_ADDON;
        chatMap["#g "] = CHAT_MSG_GUILD;
    }
    currentChat = std::pair<ChatMsg, time_t>(CHAT_MSG_WHISPER, 0);
    for (std::map<std::string,ChatMsg>::iterator i = chatMap.begin(); i != chatMap.end(); ++i)
    {
        if (filtered.find(i->first) == 0)
        {
            filtered = filtered.substr(3);
            currentChat = std::pair<ChatMsg, time_t>(i->second, time(0) + 3);
            break;
        }
    }

    filtered = chatFilter.Filter(trim((std::string&)filtered));
    if (filtered.empty())
        return;

    if (filtered.substr(0, 6) == "debug ")
    {
        std::string response = HandleRemoteCommand(filtered.substr(6));
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_ADDON, response.c_str(), LANG_ADDON,
                CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());
        sServerFacade.SendPacket(&fromPlayer, data);
        return;
    }

    if (!IsAllowedCommand(filtered) && !GetSecurity()->CheckLevelFor(PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, type != CHAT_MSG_WHISPER, &fromPlayer))
        return;

    if (type == CHAT_MSG_RAID_WARNING && filtered.find(bot->GetName()) != std::string::npos && filtered.find("award") == std::string::npos)
    {
        ChatCommandHolder cmd("warning", &fromPlayer, type);
        chatCommands.push(cmd);
        return;
    }

    if ((filtered.size() > 2 && filtered.substr(0, 2) == "d ") || (filtered.size() > 3 && filtered.substr(0, 3) == "do "))
    {
        Event event("do", "", &fromPlayer);
        std::string action = filtered.substr(filtered.find(" ") + 1);
        DoSpecificAction(action, event);
    }
    if (ChatHelper::parseValue("command", filtered).substr(0, 3) == "do ")
    {
        Event event("do", "", &fromPlayer);
        std::string action = ChatHelper::parseValue("command", filtered);
        action = action.substr(3);
        DoSpecificAction(action, event);
    }
    else if (type != CHAT_MSG_WHISPER && filtered.size() > 6 && filtered.substr(0, 6) == "queue ")
    {
        std::string remaining = filtered.substr(filtered.find(" ") + 1);
        int index = 1;
        Group* group = bot->GetGroup();
        if (group)
        {
            for (GroupReference *ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                if (ref->getSource() == master)
                    continue;

                if (ref->getSource() == bot)
                    break;

                index++;
            }
        }
        ChatCommandHolder cmd(remaining, &fromPlayer, type, time(0) + index);
        chatCommands.push(cmd);
    }
    else if (filtered == "reset")
    {
        Reset(true);
    }
    else if (filtered == "logout")
    {
        if (!(bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut()))
        {
            if (type == CHAT_MSG_WHISPER)
                TellPlayer(&fromPlayer, BOT_TEXT("logout_start"));

            if (master && GetBotMgr(master))
                SetShouldLogOut(true);
        }
    }
    else if (filtered == "logout cancel")
    {
        if (bot->IsStunnedByLogout() || bot->GetSession()->isLogingOut())
        {
            if (type == CHAT_MSG_WHISPER)
                TellPlayer(&fromPlayer, BOT_TEXT("logout_cancel"));

            WorldPacket p;
            bot->GetSession()->HandleLogoutCancelOpcode(p);
            SetShouldLogOut(false);
        }
    }
    else if ((filtered.size() > 5) && (filtered.substr(0, 5) == "wait ") && (filtered.find("wait for attack") == std::string::npos))
    {
        std::string remaining = filtered.substr(filtered.find(" ") + 1);
        uint32 delay = atof(remaining.c_str()) * static_cast<uint32>(IN_MILLISECONDS);
        if (delay > 20000)
        {
            TellPlayer(&fromPlayer, "Max wait time is 20 seconds!");
            return;
        }

        IncreaseAIInternalUpdateDelay(delay);
        isWaiting = true;
        TellPlayer(&fromPlayer, "Waiting for " + remaining + " seconds!");
        return;
    }
    else
    {
        SC_LOG("chat-cmd queued bot=%s from=%s type=%u text='%s'",
               bot ? bot->GetName() : "(null)",
               fromPlayer.GetName(),
               (unsigned)type,
               filtered.c_str());
        ChatCommandHolder cmd(filtered, &fromPlayer, type);
        chatCommands.push(cmd);
    }
}

void PlayerbotAI::HandleBotOutgoingPacket(const WorldPacket& packet)
{
    //if (packet.empty())
    //    return;

	switch (packet.GetOpcode())
	{
	case SMSG_SPELL_FAILURE:
	{
		WorldPacket p(packet);
		p.rpos(0);
		ObjectGuid casterGuid;
        p >> casterGuid.ReadAsPacked();
		if (casterGuid != bot->GetObjectGuid())
			return;

		uint32 spellId;
		p >> spellId;
		SpellInterrupted(spellId);
		return;
	}
	case SMSG_SPELL_DELAYED:
	{
		WorldPacket p(packet);
		p.rpos(0);
		ObjectGuid casterGuid;
        p >> casterGuid.ReadAsPacked();

		if (casterGuid != bot->GetObjectGuid())
			return;

		uint32 delaytime;
		p >> delaytime;
		if (delaytime <= 1000)
            IncreaseAIInternalUpdateDelay(delaytime);
		return;
	}
    case SMSG_EMOTE: // do not react to NPC emotes
    {
        WorldPacket p(packet);
        ObjectGuid source;
        uint32 emoteId;
        p.rpos(0);
        p >> emoteId >> source;
        if (source.IsPlayer())
            botOutgoingPacketHandlers.AddPacket(packet);

        return;
    }
    case SMSG_MESSAGECHAT: // do not react to self or if not ready to reply
    {
        if (!AllowActivity())
            return;

        WorldPacket p(packet);
#ifndef MANGOSBOT_ZERO
        if (!p.empty() && (p.GetOpcode() == SMSG_MESSAGECHAT || p.GetOpcode() == SMSG_GM_MESSAGECHAT))
#else
        if (!p.empty() && p.GetOpcode() == SMSG_MESSAGECHAT)
#endif
        {
            p.rpos(0);
            uint8 msgtype, chatTag;
            uint32 lang, textLen, unused;
            ObjectGuid guid1, guid2;
            std::string name, chanName, message;
            p >> msgtype >> lang;

            // filter msg type
            switch (msgtype)
            {
            case CHAT_MSG_CHANNEL:
            case CHAT_MSG_SAY:
            case CHAT_MSG_PARTY:
#ifdef MANGOSBOT_TWO
            case CHAT_MSG_PARTY_LEADER:
#endif
            case CHAT_MSG_YELL:
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_GUILD:
                break;
            default:
                return;
                break;
            }

#ifdef MANGOSBOT_ZERO
            switch (msgtype)
            {
            case CHAT_MSG_SAY:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_YELL:
                p >> guid1 >> guid2;
                break;
            case CHAT_MSG_CHANNEL:
                p >> chanName >> unused >> guid1;
                break;
            default:
                p >> guid1;
                break;
            }

            if (guid1.IsEmpty() || p.size() > 0x1000)
                return;

            p >> textLen >> message >> chatTag;
#endif
#ifdef MANGOSBOT_ONE
            p >> guid1 >> unused;
            if (guid1.IsEmpty() || p.size() > 0x1000)
                return;

            switch (msgtype)
            {
            case CHAT_MSG_CHANNEL:
                p >> chanName;
                [[fallthrough]];
            case CHAT_MSG_SAY:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_YELL:
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_GUILD:
                p >> guid2;
                p >> textLen >> message >> chatTag;
                break;
            default:
                break;
            }
#endif
#ifdef MANGOSBOT_TWO
            p >> guid1 >> unused;
            if (guid1.IsEmpty() || p.size() > 0x1000)
                return;

            if (p.GetOpcode() == SMSG_GM_MESSAGECHAT)
            {
                p >> textLen;
                p >> name;
            }

            switch (msgtype)
            {
            case CHAT_MSG_CHANNEL:
                p >> chanName;
                [[fallthrough]];
            case CHAT_MSG_SAY:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            case CHAT_MSG_YELL:
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_GUILD:
                p >> guid2;
                p >> textLen >> message >> chatTag;
                break;
            default:
                break;
            }
#endif

            bool isAiChat = sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3);

            if (m_recordIncommingMessages)
            {
                std::string recievedMessage = message;

                std::string senderName;
                if(!sObjectMgr.GetPlayerNameByGUID(guid1, senderName))
                    senderName = "unknown";

                std::string recievedChatType;

                //Add chat type prefix before recieved message. Ie. for Guilg "guild:".
                switch (msgtype)
                {
                    case CHAT_MSG_SAY:
                        recievedChatType = "says";
                        break;
                    case CHAT_MSG_YELL:
                        recievedChatType = "yells";
                        break;
                    case CHAT_MSG_PARTY:
                        recievedChatType = "party";
                        break;
#ifdef MANGOSBOT_TWO
                    case CHAT_MSG_PARTY_LEADER:
                        recievedChatType = "party leader";
                        break;
#endif
                    case CHAT_MSG_GUILD:
                        recievedChatType = "guild";
                        break;
                    case CHAT_MSG_OFFICER:
                        recievedChatType = "officer";
                        break;
                    case CHAT_MSG_WHISPER:
                        //Get player name for whisper and add "<whispe:" before message.
                        recievedChatType = "whispers";
                        break;
                    case CHAT_MSG_CHANNEL: //Channel needs channel name to be added as prefix, so we add "c:" before channel name.
                        recievedChatType = chanName;
                        break;
                    default:
                        break;
                }

                recievedMessage = "[" + senderName + "] " + recievedChatType + ": " + recievedMessage;

                m_recordedMessages.push_back(recievedMessage);
            }

            if (isAiChat && (lang == LANG_ADDON || message.find("d:") == 0))
                return;

            if (guid1 != bot->GetObjectGuid()) // do not reply to self
            {
                // try to always reply to real player
                time_t lastChat = GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Get();
                bool isPaused = time(0) < lastChat;
                bool shouldReply = false;
                bool isFromFreeBot = false;
                sObjectMgr.GetPlayerNameByGUID(guid1, name);
                uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(guid1);
                isFromFreeBot = sPlayerbotAIConfig.IsInRandomAccountList(accountId);
                if (!isFromFreeBot)
                {

                    isFromFreeBot = sPlayerbotAIConfig.IsFreeAltBot(guid1);

                    if (isFromFreeBot)
                    {
                        Player* player = sObjectMgr.GetPlayer(guid1);
                        if (player && IsRealPlayer(player))
                            isFromFreeBot = false;
                    }
                }

                bool isMentioned = message.find(bot->GetName()) != std::string::npos;
                

                ChatChannelSource chatChannelSource = GetChatChannelSource(bot, msgtype, chanName);

                if (!isAiChat || isFromFreeBot)
                {
                    // random bot speaks, chat CD
                    if ((isFromFreeBot || isAiChat) && isPaused)
                        return;

                    // BG: react only if mentioned or if not channel and real player spoke
                    if (bot->InBattleGround() && !(isMentioned || (msgtype != CHAT_MSG_CHANNEL && !isFromFreeBot)))
                        return;

                    if (HasRealPlayerMaster() && guid1 != GetMaster()->GetObjectGuid())
                        return;

                    if (lang == LANG_ADDON)
                        return;

                    if (boost::algorithm::istarts_with(message, sPlayerbotAIConfig.toxicLinksPrefix)
                        && (GetChatHelper()->ExtractAllItemIds(message).size() > 0 || GetChatHelper()->ExtractAllQuestIds(message).size() > 0)
                        && sPlayerbotAIConfig.toxicLinksRepliesChance)
                    {
                        if (urand(0, 50) > 0 || urand(1, 100) > sPlayerbotAIConfig.toxicLinksRepliesChance)
                        {
                            return;
                        }
                    }
                    else if ((GetChatHelper()->ExtractAllItemIds(message).count(19019) && sPlayerbotAIConfig.thunderfuryRepliesChance))
                    {
                        if (urand(0, 60) > 0 || urand(1, 100) > sPlayerbotAIConfig.thunderfuryRepliesChance)
                        {
                            return;
                        }
                    }
                    else
                    {
                        if (isFromFreeBot && urand(0, 20))
                            return;

                        if (msgtype == CHAT_MSG_GUILD && (!sPlayerbotAIConfig.guildRepliesRate || urand(1, 100) >= sPlayerbotAIConfig.guildRepliesRate))
                            return;

                        if (!isFromFreeBot)
                        {
                            if (!isMentioned && urand(0, 4))
                                return;
                        }
                        else
                        {
                            if (urand(0, 20 + 10 * isMentioned))
                                return;
                        }
                    }
                }

                MANGOS_ASSERT(!message.empty());     
                QueueChatResponse(msgtype, guid1, ObjectGuid(), message, chanName, name, isAiChat);
                GetAiObjectContext()->GetValue<time_t>("last said", "chat")->Set(time(0) + urand(5, 25));

                return;
            }

            if (isAiChat)
            {
                ChatChannelSource chatChannelSource = GetBotAI(bot)->GetChatChannelSource(bot, msgtype, chanName);

                std::string llmChannel;

                if (!sPlayerbotAIConfig.llmGlobalContext)
                    llmChannel = ((chatChannelSource == ChatChannelSource::SRC_WHISPER) ? name : std::to_string(chatChannelSource));

                AiObjectContext* context = aiObjectContext;
                std::string llmContext = AI_VALUE(std::string, "manual string::llmcontext" + llmChannel);
                llmContext = llmContext + " " + bot->GetName() + ":" + message;
                PlayerbotLLMInterface::LimitContext(llmContext, llmContext.size());
                SET_AI_VALUE(std::string, "manual string::llmcontext" + llmChannel, llmContext);
            }
        }

        return;
    }
    case SMSG_MOVE_KNOCK_BACK: // handle knockbacks
    {
        if (!sPlayerbotAIConfig.useKnockback || !HasPlayerNearby())
            return;

        WorldPacket p(packet);
        p.rpos(0);

        ObjectGuid guid;
        uint32 counter;
        float vcos, vsin, horizontalSpeed, verticalSpeed = 0.f;

        p >> guid.ReadAsPacked() >> counter >> vcos >> vsin >> horizontalSpeed >> verticalSpeed;
        verticalSpeed = -verticalSpeed;

        // stop casting
        InterruptSpell(false);

        // stop movement
        StopMoving();

        float timeToLand, distToLand, maxHeight;
        bool goodLanding = true;
        float angleRadian = (vsin > 0) ? acos(vcos) : -acos(vcos);
        float angle = angleRadian;
        std::vector<WorldPosition> path;
        WorldPosition dest_calculated = JumpAction::CalculateJumpParameters(WorldPosition(bot), bot, angle, verticalSpeed, horizontalSpeed, timeToLand, distToLand, maxHeight, goodLanding, path, 200.f);
        bool jumpInPlace = horizontalSpeed == 0.f;
        if (!dest_calculated)
        {
            sLog.outDetail("Knockback fail to calculate");
            return;
        }

        // set fall height for fall damage calculations
        bot->SetFallInformation(0, maxHeight);

        // fix height
        if (goodLanding)
        {
            float ox = dest_calculated.getX();
            float oy = dest_calculated.getY();
            float oz = dest_calculated.getZ();
            bot->UpdateAllowedPositionZ(ox, oy, oz);
            // set to fall after land if not at the ground
            if (fabs(oz - dest_calculated.getZ()) > 5.0f)
            {
                SetFallAfterJump();
            }
            else
            {
                dest_calculated = WorldPosition(dest_calculated.getMapId(), ox, oy, oz);
            }

        }
        else
            SetFallAfterJump();


        // add moveflags
#ifdef MANGOSBOT_ZERO
        bot->m_movementInfo.SetMovementFlags(MOVEFLAG_JUMPING);
        bot->m_movementInfo.AddMovementFlag(MOVEFLAG_BACKWARD);
#else
        bot->m_movementInfo.SetMovementFlags(MOVEFLAG_FALLING);
        bot->m_movementInfo.AddMovementFlag(MOVEFLAG_BACKWARD);
#endif

        // send ack
        WorldPacket ack(CMSG_MOVE_KNOCK_BACK_ACK);
        bot->m_movementInfo.jump.cosAngle = vcos;
        bot->m_movementInfo.jump.sinAngle = vsin;
        bot->m_movementInfo.jump.zspeed = -verticalSpeed;
        bot->m_movementInfo.jump.xyspeed = horizontalSpeed;
#ifdef MANGOSBOT_TWO
        ack << bot->GetObjectGuid().WriteAsPacked();
#else
        ack << bot->GetObjectGuid();
#endif
        ack << uint32(0);
        ack << bot->m_movementInfo;
        bot->GetSession()->HandleMoveKnockBackAck(ack);

        // write jump time
        uint32 curTime = sWorld.GetCurrentMSTime();
        jumpTime = curTime + sWorld.GetAverageDiff() + (uint32)(timeToLand * static_cast<uint32>(IN_MILLISECONDS)) + 1000;
        SetJumpDestination(dest_calculated);

        // set highest jump point to relocate
        WorldPosition highestPoint = dest_calculated;
        for (auto& point : path)
        {
            if (point.getZ() > highestPoint.getZ())
                highestPoint = point;
        }

        bot->Relocate(highestPoint.getX(), highestPoint.getY(), highestPoint.getZ());
        //bot->m_movementInfo.ChangePosition(dest_calculated.getX(), dest_calculated.getY(), dest_calculated.getZ(), bot->GetOrientation());
        sLog.outDetail("%s: KNOCKBACK x: %f, y: %f, z: %f, time: %f, dist: %f, maxHeight: %f inPlace: %u, landTime: %u", bot->GetName(), dest_calculated.getX(), dest_calculated.getY(), dest_calculated.getZ(), timeToLand, distToLand, maxHeight, jumpInPlace, jumpTime);
        return;
    }
	default:
		// log group invite arrivals so we
		// can verify the auto-accept handler fires. If we see this line in
		// the server log but no subsequent "AcceptInvitationAction" event,
		// the strategy/trigger is registered but the action isn't being
		// scheduled — points us at engine priority or trigger.Check() bugs.
		if (packet.GetOpcode() == SMSG_GROUP_INVITE) {
			SC_LOG("bot=%s received SMSG_GROUP_INVITE — queued for 'group invite' trigger",
			       bot ? bot->GetName() : "(null)");
		}
		botOutgoingPacketHandlers.AddPacket(packet);
	}
}

void PlayerbotAI::SpellInterrupted(uint32 spellid)
{
    LastSpellCast& lastSpell = aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get();
    if (!spellid || lastSpell.id != spellid)
        return;

    time_t now = time(0);
    if (now <= lastSpell.time)
        return;

    uint32 castTimeSpent = 1000 * (now - lastSpell.time);
    uint32 globalCooldown = CalculateGlobalCooldown(lastSpell.id);
    if (castTimeSpent < globalCooldown)
        SetAIInternalUpdateDelay(globalCooldown - castTimeSpent);
    else
        SetAIInternalUpdateDelay(sPlayerbotAIConfig.reactDelay);

    lastSpell.id = 0;
}

int32 PlayerbotAI::CalculateGlobalCooldown(uint32 spellid)
{
    if (!spellid)
        return 0;

    int32 globalCooldown = 0;
    SpellEntry const* spellEntry = sSpellTemplate.LookupEntry<SpellEntry>(spellid);
    if (spellEntry)
    {
        globalCooldown = spellEntry->StartRecoveryTime;
    }

    return globalCooldown > 0 ? globalCooldown : sPlayerbotAIConfig.reactDelay;
}

void PlayerbotAI::HandleMasterIncomingPacket(const WorldPacket& packet)
{
    masterIncomingPacketHandlers.AddPacket(packet);
}

void PlayerbotAI::HandleMasterOutgoingPacket(const WorldPacket& packet)
{
    masterOutgoingPacketHandlers.AddPacket(packet);
}

void PlayerbotAI::ChangeEngine(BotState type)
{
    Engine* engine = engines[(uint8)type];

    if (currentEngine != engine)
    {
        currentEngine = engine;
        currentState = type;
        ReInitCurrentEngine();

        switch (type)
        {
        case BotState::BOT_STATE_COMBAT:
            sLog.outDebug( "=== %s COMBAT ===", bot->GetName());
            break;
        case BotState::BOT_STATE_NON_COMBAT:
            sLog.outDebug( "=== %s NON-COMBAT ===", bot->GetName());
            break;
        case BotState::BOT_STATE_DEAD:
            sLog.outDebug( "=== %s DEAD ===", bot->GetName());
            break;
        case BotState::BOT_STATE_REACTION:
            sLog.outDebug("=== %s REACTION ===", bot->GetName());
            break;
        default: break;
        }
    }
}


void PlayerbotAI::DoNextAction(bool min)
{
    SC_PHASE("DoNextAction.entry", bot ? bot->GetName() : "(null)");
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || (GetMaster() && GetMaster()->IsBeingTeleported()))
    {
        SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        return;
    }

    // if in combat but stuck with old data - clear targets
    SC_PHASE("DoNextAction.staleTargetCheck", bot ? bot->GetName() : "(null)");
    if (currentEngine == engines[(uint8)BotState::BOT_STATE_NON_COMBAT] && sServerFacade.IsInCombat(bot))
    {
        if (aiObjectContext->GetValue<Unit*>("current target")->Get() != NULL ||
            aiObjectContext->GetValue<ObjectGuid>("attack target")->Get() != ObjectGuid() ||
            aiObjectContext->GetValue<Unit*>("dps target")->Get() != NULL)
        {
            Reset();
        }
    }

    bool minimal = !AllowActivity();

    SC_PHASE("DoNextAction.engineDoNextAction", bot ? bot->GetName() : "(null)");
    currentEngine->DoNextAction(NULL, 0, (minimal || min), bot->IsTaxiFlying());
    SC_PHASE("DoNextAction.afterEngine", bot ? bot->GetName() : "(null)");

    if (!bot->IsInWorld()) //Teleport out of bg
        return;

    if (minimal)
    {
        if (!MovementAction::MinimalMove(this) && !bot->isAFK() && !bot->InBattleGround() && !HasRealPlayerMaster())
            bot->ToggleAFK();

        SetAIInternalUpdateDelay(sPlayerbotAIConfig.passiveDelay);
        return;
    }
    else if (bot->isAFK())
        bot->ToggleAFK();


    Group *group = bot->GetGroup();

    // A test-harness login keeps its externally-assigned master. The two
    // self-heals below both fire on the harness setup (the GM driver is
    // deliberately neither a group member nor the group leader): the first
    // cleared the master and RESET STRATEGIES, the harness's react-delay
    // repair reinstated the master a tick later, and the pair ping-ponged
    // once per second on every run member (gdb backtrace 2026-08-24,
    // 500+ resets per bot in 8 minutes, action queue wiped each pass).
    bool const externallyManagedMaster =
        sRandomPlayerbotMgr.IsExternallyManaged(bot->GetGUIDLow());

    //Remove bot masters not in our group.
    if (!externallyManagedMaster && master && master != bot && !HasActivePlayerMaster() && (!group || group->GetLeaderGuid() != master->GetObjectGuid()))
    {
        master = IsRealPlayer() ? bot : nullptr;
        SetMaster(master);
        ResetStrategies();
    }

    // test BG master set
    if (!externallyManagedMaster && (!master || !HasActivePlayerMaster()) && group && !IsRealPlayer())
    {
        //Ideally we want to have the leader as master.
        Player* newMaster = GetGroupMaster();
        Player* playerMaster = nullptr;

        //Are there any non-bot players in the group?
        if (!newMaster || GetBotAI(newMaster))
            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* member = gref->getSource();

                if (!member)
                    continue;

                if (member == bot)
                    continue;

                if (member == newMaster)
                    continue;

                if (!member->IsInWorld())
                    continue;

                if (!member->IsInGroup(bot, true))
                    continue;

                //Do not make bots your master if they are nog group leader.
                if (GetBotAI(member) && !bot->InBattleGround())
                    continue;

                if (bot->InBattleGround())
                    continue;

                // same BG
                if (bot->InBattleGround() && bot->GetBattleGround()->GetTypeId() == BATTLEGROUND_AV && !GetBotAI(member) && member->InBattleGround() && bot->GetMapId() == member->GetMapId())
                {
                    // TODO disable move to objective if have master in bg
                    continue;

                    if (!group->SameSubGroup(bot, member))
                        continue;

                    if (member->GetLevel() < bot->GetLevel())
                        continue;

                    // follow real player only if he has more honor/arena points
                    bool isArena = false;
#ifndef MANGOSBOT_ZERO
                    if (bot->GetBattleGround()->IsArena())
                        isArena = true;
#endif
                    if (isArena)
                    {
                        if (group->IsLeader(member->GetObjectGuid()))
                        {
                            playerMaster = member;
                            break;
                        }
                        else
                            continue;
                    }
                    else
                    {
#ifndef MANGOSBOT_ZERO
                        uint32 honorpts = member->GetHonorPoints();
                        if (bot->GetHonorPoints() && honorpts < bot->GetHonorPoints())
                            continue;
#else
                        if (bot->GetHonorRankInfo().rank >= member->GetHonorRankInfo().rank)
                            continue;
#endif
                    }

                    playerMaster = member;
                    continue;
                }

                newMaster = member;
                break;
            }

        if (!newMaster && playerMaster)
        {
            newMaster = playerMaster;
        }

        if (newMaster && (!master || master != newMaster) && bot != newMaster)
        {
            master = newMaster;
            SetMaster(newMaster);
            ResetStrategies();

            if (sRandomPlayerbotMgr.IsFreeBot(bot))
            {
                std::string defaultMovementStrategy = GetDefaultMovementStrategy();
                ChangeStrategy("+" + defaultMovementStrategy, BotState::BOT_STATE_NON_COMBAT);
            }

            if (GetMaster() == GetGroupMaster())
            {
                TellPlayer(master, BOT_TEXT("hello_follow"));
            }
            else
            {
                TellPlayer(master, BOT_TEXT("hello"));
            }
        }
    }

    // fix bots in BG not having proper strategies
#ifdef MANGOSBOT_ZERO
    if (bot->InBattleGround() && !HasStrategy("battleground", BotState::BOT_STATE_NON_COMBAT))
        ResetStrategies();
#else
    if ((bot->InBattleGround() && (!bot->IsBeingTeleported() && !bot->InArena()) && !HasStrategy("battleground", BotState::BOT_STATE_NON_COMBAT)) || ((!bot->IsBeingTeleported()&&bot->InArena()) && !HasStrategy("arena", BotState::BOT_STATE_NON_COMBAT)))
        ResetStrategies();
#endif

    if (master && master->IsInWorld())
	{
		if (master->m_movementInfo.HasMovementFlag(MOVEFLAG_WALK_MODE) && sServerFacade.GetDistance2d(bot, master) < 20.0f) bot->m_movementInfo.AddMovementFlag(MOVEFLAG_WALK_MODE);
		else bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);

        if (master->IsSitState() && aiInternalUpdateDelay < 1000)
        {
            if (!sServerFacade.isMoving(bot) && sServerFacade.GetDistance2d(bot, master) < 10.0f)
                bot->SetStandState(UNIT_STAND_STATE_SIT);
        }
        else if (aiInternalUpdateDelay < 1000)
            bot->SetStandState(UNIT_STAND_STATE_STAND);

        if (!group && sRandomPlayerbotMgr.IsFreeBot(bot) && !IsRealPlayer())
        {
            GetBotAI(bot)->SetMaster(nullptr);
        }
	}
	else if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_WALK_MODE)) bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);
    else if ((aiInternalUpdateDelay < 1000) && bot->IsSitState()) bot->SetStandState(UNIT_STAND_STATE_STAND);

#ifndef MANGOSBOT_ZERO
    if (bot->IsFlying() && !bot->IsFreeFlying())
    {
        if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING))
            bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_FLYING);
#ifdef MANGOSBOT_ONE
        if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_FLYING2))
            bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_FLYING2);
#endif
        if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_CAN_FLY))
            bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_CAN_FLY);
        if (bot->m_movementInfo.HasMovementFlag(MOVEFLAG_LEVITATING))
            bot->m_movementInfo.RemoveMovementFlag(MOVEFLAG_LEVITATING);
    }
#endif

    if (bot->IsTaxiFlying())
    {
        return;
    }
}

void PlayerbotAI::ReInitCurrentEngine()
{
    InterruptSpell();
    currentEngine->Init();
}

void PlayerbotAI::ChangeStrategy(const std::string& names, BotState type)
{
    if(type == BotState::BOT_STATE_ALL)
    {
        for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        {
            Engine* engine = engines[i];
            if (engine)
            {
                engine->ChangeStrategy(names);
            }
        }
    }
    else
    {
        Engine* engine = engines[(uint8)type];
        if (engine)
        {
            engine->ChangeStrategy(names);
        }
    }
}

void PlayerbotAI::PrintStrategies(Player* requester, BotState type)
{
    if (type == BotState::BOT_STATE_ALL)
    {
        for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        {
            Engine* engine = engines[i];
            if (engine)
            {
                engine->PrintStrategies(requester, BotStateToString(BotState(i)));
            }
        }
    }
    else
    {
        Engine* engine = engines[(uint8)type];
        if (engine)
        {
            engine->PrintStrategies(requester, BotStateToString(type));
        }
    }
}

void PlayerbotAI::ClearStrategies(BotState type)
{
    if (type == BotState::BOT_STATE_ALL)
    {
        for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
        {
            Engine* engine = engines[i];
            if (engine)
            {
                engine->removeAllStrategies();
            }
        }
    }
    else
    {
        Engine* engine = engines[(uint8)type];
        if (engine)
        {
            engine->removeAllStrategies();
        }
    }
}

std::list<std::string_view> PlayerbotAI::GetStrategies(BotState type)
{
    // Can't get all strategies for all engines
    if (type != BotState::BOT_STATE_ALL)
    {
        Engine* engine = engines[(uint8)type];
        if (engine)
        {
            return engine->GetStrategies();
        }
    }

    return std::list<std::string_view>();
}

bool PlayerbotAI::CanDoSpecificAction(const std::string& name, bool isUseful, bool isPossible)
{
    for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        Engine* engine = engines[i];
        if(engine)
        {
            if(engine->CanExecuteAction(name, isUseful, isPossible))
            {
                return true;
            }
        }
    }

    return false;
}

bool PlayerbotAI::DoSpecificAction(const std::string& name, Event event, bool silent)
{
    Player* requester = event.getOwner();
    for (uint8 i = 0 ; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        Engine* engine = engines[i];
        if(engine)
        {
            ActionResult res = engine->ExecuteAction(name, event);
            switch (res)
            {
                case ACTION_RESULT_OK:
                {
                    if (!silent)
                    {
                        PlaySound(TEXTEMOTE_NOD);
                    }

                    return true;
                }

                case ACTION_RESULT_IMPOSSIBLE:
                {
                    if (!silent)
                    {
                        std::ostringstream out;
                        out << name << ": impossible";
                        TellError(requester, out.str());
                        PlaySound(TEXTEMOTE_NO);
                    }

                    return false;
                }

                case ACTION_RESULT_USELESS:
                {
                    if (!silent)
                    {
                        std::ostringstream out;
                        out << name << ": useless";
                        TellError(requester, out.str());
                        PlaySound(TEXTEMOTE_NO);
                    }

                    return false;
                }

                case ACTION_RESULT_FAILED:
                {
                    if (!silent)
                    {
                        std::ostringstream out;
                        out << name << ": failed";
                        TellError(requester, out.str());
                    }

                    return false;
                }

                default:
                {
                    if (!silent)
                    {
                        std::ostringstream out;
                        out << name << ": unknown action";
                        TellError(requester, out.str());
                    }

                    return false;
                }
            }
        }
        else
        {
            if (!silent)
            {
                std::ostringstream out;
                out << name << ": engine not ready";
                TellError(requester, out.str());
            }
        }
    }

    return false;
}

bool PlayerbotAI::PlaySound(uint32 emote)
{
    if (EmotesTextSoundEntry const* soundEntry = FindTextSoundEmoteFor(emote, bot->getRace(), bot->getGender()))
    {
        bot->PlayDistanceSound(soundEntry->SoundId);
        return true;
    }

    return false;
}

bool PlayerbotAI::PlayEmote(uint32 emote)
{
    WorldPacket data(SMSG_TEXT_EMOTE);
    data << (TextEmotes)emote;
    data << urand(0, EmoteAction::GetNumberOfEmoteVariants((TextEmotes)emote, bot->getRace(), bot->getGender()) - 1);
    data << ((master && (sServerFacade.GetDistance2d(bot, master) < 30.0f) && urand(0, 1)) ? master->GetObjectGuid() : (bot->GetSelectionGuid() && urand(0, 1)) ? bot->GetSelectionGuid() : ObjectGuid());
    bot->GetSession()->HandleTextEmoteOpcode(data);

    return false;
}

bool PlayerbotAI::ContainsStrategy(StrategyType type)
{
    for (uint8 i = 0 ; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        if(engines[i])
        {
            if (engines[i]->ContainsStrategy(type))
            {
                return true;
            }
        }
    }

    return false;
}

bool PlayerbotAI::HasStrategy(const std::string& name, BotState type)
{
    // Can't check the strategy for all engines at once
    if(type != BotState::BOT_STATE_ALL)
    {
        const uint8 typeIndex = (uint8)type;
        if (engines[typeIndex])
        {
            return engines[typeIndex]->HasStrategy(name);
        }
    }

    return false;
}

void PlayerbotAI::ResetStrategies(bool autoLoad)
{
    for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        engines[i]->initMode = true;
        engines[i]->removeAllStrategies();
    }

    AiFactory::AddDefaultCombatStrategies(bot, this, engines[(uint8)BotState::BOT_STATE_COMBAT]);
    AiFactory::AddDefaultNonCombatStrategies(bot, this, engines[(uint8)BotState::BOT_STATE_NON_COMBAT]);
    AiFactory::AddDefaultDeadStrategies(bot, this, engines[(uint8)BotState::BOT_STATE_DEAD]);
    AiFactory::AddDefaultReactionStrategies(bot, this, reactionEngine);
    if (autoLoad && HasPlayerRelation()) sPlayerbotDbStore.Load(this);

    // A role handed out by the dungeon finder wins over both the talent spec
    // and the stored strategy set - hence applied last. addStrategy() drops the
    // sibling spec strategy by itself and does nothing for classes that cannot
    // fill the role, so a mage stays a mage.
    if (m_forcedRole)
    {
        char const* roleStrategy = m_forcedRole == 1 ? "tank" : (m_forcedRole == 2 ? "heal" : "dps");
        engines[(uint8)BotState::BOT_STATE_COMBAT]->addStrategy(roleStrategy);
        engines[(uint8)BotState::BOT_STATE_NON_COMBAT]->addStrategy(roleStrategy);
        sLog.outBasic("LFT: %s strategies rebuilt as %s", bot->GetName(), roleStrategy);
    }

    for (uint8 i = 0; i < (uint8)BotState::BOT_STATE_ALL; i++)
    {
        engines[i]->initMode = false;
        engines[i]->Init();
    }
}

bool PlayerbotAI::IsRanged(Player* player, bool inGroup)
{
    PlayerbotAI* botAi = GetBotAI(player);
    if (botAi)
    {
        bool isRanged = botAi->ContainsStrategy(STRATEGY_TYPE_RANGED);
        if (inGroup || isRanged)
            return isRanged;
    }

    switch (player->getClass())
    {
    case CLASS_PALADIN:
    case CLASS_WARRIOR:
    case CLASS_ROGUE:
#ifdef MANGOSBOT_TWO
    case CLASS_DEATH_KNIGHT:
#endif
        return false;
    case CLASS_DRUID:
        return !HasAnyAuraOf(player, "cat form", "bear form", "dire bear form", NULL);
    }
    return true;
}

bool PlayerbotAI::IsMelee(Player* player, bool inGroup)
{
    return !IsRanged(player, inGroup);
}

bool PlayerbotAI::IsTank(Player* player, bool inGroup)
{
    PlayerbotAI* botAi = GetBotAI(player);
    if (botAi)
    {
        bool isTank = botAi->ContainsStrategy(STRATEGY_TYPE_TANK);
        if (inGroup || isTank)
            return isTank;
    }

    BotRoles botRoles = AiFactory::GetPlayerRoles(player);

    return (botRoles & BOT_ROLE_TANK) != 0;
}

bool PlayerbotAI::IsHeal(Player* player, bool inGroup)
{
    PlayerbotAI* botAi = GetBotAI(player);
    if (botAi)
    {
        bool isHeal = botAi->ContainsStrategy(STRATEGY_TYPE_HEAL);
        if (inGroup || isHeal)
            return isHeal;
    }

    BotRoles botRoles = AiFactory::GetPlayerRoles(player);

    return (botRoles & BOT_ROLE_HEALER) != 0;
}

namespace MaNGOS
{

    class UnitByGuidInRangeCheck
    {
    public:
        UnitByGuidInRangeCheck(WorldObject const* obj, ObjectGuid guid, float range) : i_obj(obj), i_range(range), i_guid(guid) {}
        WorldObject const& GetFocusObject() const { return *i_obj; }
        bool operator()(Unit* u)
        {
            return u->GetObjectGuid() == i_guid && i_obj->IsWithinDistInMap(u, i_range);
        }
    private:
        WorldObject const* i_obj;
        float i_range;
        ObjectGuid i_guid;
    };

    class GameObjectByGuidInRangeCheck
    {
    public:
        GameObjectByGuidInRangeCheck(WorldObject const* obj, ObjectGuid guid, float range) : i_obj(obj), i_range(range), i_guid(guid) {}
        WorldObject const& GetFocusObject() const { return *i_obj; }
        bool operator()(GameObject* u)
        {
            if (u && i_obj->IsWithinDistInMap(u, i_range) && sServerFacade.isSpawned(u) && u->GetGOInfo() && u->GetObjectGuid() == i_guid)
                return true;

            return false;
        }
    private:
        WorldObject const* i_obj;
        float i_range;
        ObjectGuid i_guid;
    };

};


Unit* PlayerbotAI::GetUnit(ObjectGuid guid)
{
    if (!guid)
        return NULL;

    // FindMap, not GetMap: GetMap asserts on a bot without a map and this
    // core's MANGOS_ASSERT throws, which killed the server from inside a
    // loot check while a bot was mid-teleport. The null test right below
    // has always been here - it just could never fire.
    Map* map = bot->FindMap();
    if (!map)
        return NULL;

    return sObjectAccessor.GetUnit(*bot, guid);
}

Unit* PlayerbotAI::GetUnit(CreatureDataPair const* creatureDataPair)
{
    if (!creatureDataPair)
        return NULL;

    ObjectGuid guid(HIGHGUID_UNIT, creatureDataPair->second.creature_id[0], creatureDataPair->first);

    if (!guid)
        return NULL;

    Map* map = sMapMgr.FindMap(creatureDataPair->second.position.mapid);

    if (!map)
        return NULL;

    return map->GetUnit(guid);
}


Creature* PlayerbotAI::GetCreature(ObjectGuid guid) const
{
    if (!guid)
        return NULL;

    // FindMap, not GetMap: GetMap asserts on a bot without a map and this
    // core's MANGOS_ASSERT throws, which killed the server from inside a
    // loot check while a bot was mid-teleport. The null test right below
    // has always been here - it just could never fire.
    Map* map = bot->FindMap();
    if (!map)
        return NULL;

    return map->GetCreature(guid);
}

Creature* PlayerbotAI::GetAnyTypeCreature(ObjectGuid guid) const
{
    if (!guid)
        return NULL;

    // FindMap, not GetMap: GetMap asserts on a bot without a map and this
    // core's MANGOS_ASSERT throws, which killed the server from inside a
    // loot check while a bot was mid-teleport. The null test right below
    // has always been here - it just could never fire.
    Map* map = bot->FindMap();
    if (!map)
        return NULL;

    return map->GetAnyTypeCreature(guid);
}

GameObject* PlayerbotAI::GetGameObject(ObjectGuid guid)
{
    if (!guid)
        return NULL;

    // FindMap, not GetMap: GetMap asserts on a bot without a map and this
    // core's MANGOS_ASSERT throws, which killed the server from inside a
    // loot check while a bot was mid-teleport. The null test right below
    // has always been here - it just could never fire.
    Map* map = bot->FindMap();
    if (!map)
        return NULL;

    return map->GetGameObject(guid);
}

GameObject* PlayerbotAI::GetGameObject(GameObjectDataPair const* gameObjectDataPair)
{
    if (!gameObjectDataPair)
        return NULL;

    ObjectGuid guid(HIGHGUID_GAMEOBJECT, gameObjectDataPair->second.id, gameObjectDataPair->first);

    if (!guid)
        return NULL;

    Map* map = sMapMgr.FindMap(gameObjectDataPair->second.position.mapid);

    if (!map)
        return NULL;

    return map->GetGameObject(guid);
}

WorldObject* PlayerbotAI::GetWorldObject(ObjectGuid guid)
{
    if (!guid)
        return NULL;

    // FindMap, not GetMap: GetMap asserts on a bot without a map and this
    // core's MANGOS_ASSERT throws, which killed the server from inside a
    // loot check while a bot was mid-teleport. The null test right below
    // has always been here - it just could never fire.
    Map* map = bot->FindMap();
    if (!map)
        return NULL;

    return map->GetWorldObject(guid);
}

std::vector<Player*> PlayerbotAI::GetPlayersInGroup()
{
    std::vector<Player*> members;

    Group* group = bot->GetGroup();

    if (!group)
        return members;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();

        if (GetBotAI(member) && !GetBotAI(member)->IsRealPlayer())
            continue;

        members.push_back(ref->getSource());
    }

    return members;
}

void PlayerbotAI::DropQuest(uint32 questIdToDrop)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (questId == questIdToDrop)
        {
            if (Quest const* q = sObjectMgr.GetQuestTemplate(questIdToDrop))
                sPlayerbotAIConfig.logEvent(this, "QuestDropped", q->GetTitle(), std::to_string(questIdToDrop));

            bot->SetQuestSlot(slot, 0);

            //We ignore unequippable quest items in this case, its' still be equipped
            bot->TakeQuestSourceItem(questId, false);

            bot->SetQuestStatus(questId, QUEST_STATUS_NONE);
            bot->getQuestStatusMap()[questId].m_rewarded = false;

            //TODO should probably also remove quest items?

            return;
        }
    }
}

std::vector<const Quest*> PlayerbotAI::GetAllCurrentQuests()
{
    std::vector<const Quest*> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        result.push_back(sObjectMgr.GetQuestTemplate(questId));
    }

    return result;
}

std::vector<const Quest*> PlayerbotAI::GetCurrentIncompleteQuests()
{
    std::vector<const Quest*> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
        {
            result.push_back(sObjectMgr.GetQuestTemplate(questId));
        }
    }

    return result;
}

std::set<uint32> PlayerbotAI::GetAllCurrentQuestIds()
{
    std::set<uint32> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        result.insert(questId);
    }

    return result;
}

std::set<uint32> PlayerbotAI::GetCurrentIncompleteQuestIds()
{
    std::set<uint32> result;

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
        {
            continue;
        }

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
        {
            result.insert(questId);
        }
    }

    return result;
}

const Quest* PlayerbotAI::GetCurrentIncompleteQuestWithId(uint32 pQuestId)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (pQuestId == questId && status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
            return sObjectMgr.GetQuestTemplate(questId);
    }

    return nullptr;
}

bool PlayerbotAI::HasCurrentIncompleteQuestWithId(uint32 pQuestId)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        if (pQuestId == questId && status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_NONE)
            return true;
    }

    return false;
}

/*
* @return vector of pair<quest, count>
*/
std::vector<std::pair<const Quest*, uint32>> PlayerbotAI::GetCurrentQuestsRequiringItemId(uint32 itemId)
{
    std::vector<std::pair<const Quest*, uint32>> result;

    if (!itemId)
    {
        return result;
    }

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;

        QuestStatus status = bot->GetQuestStatus(questId);
        const Quest* quest = sObjectMgr.GetQuestTemplate(questId);
        for (uint8 i = 0; i < std::size(quest->ReqItemId); ++i)
        {
            if (quest->ReqItemId[i] == itemId)
            {
                result.push_back(std::pair(quest, quest->ReqItemCount[i]));
                break;
            }
        }
    }

    return result;
}

const AreaTableEntry* PlayerbotAI::GetCurrentArea()
{
    return GetAreaEntryByAreaID(sServerFacade.GetAreaId(bot));
}

const AreaTableEntry* PlayerbotAI::GetCurrentZone()
{
    return GetAreaEntryByAreaID(sTerrainMgr.GetZoneId(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
}

/*
* @return localized area_name
*/
std::string PlayerbotAI::GetLocalizedAreaName(const AreaTableEntry* entry)
{
    // Penqle's area_name is single char* (no locale array).
    return entry && entry->area_name ? std::string(entry->area_name) : std::string();
}

bool PlayerbotAI::IsInCapitalCity()
{
    AreaTableEntry const* current_area = GetAreaEntryByAreaID(sServerFacade.GetAreaId(bot));
    if (!current_area)
    {
        return false;
    }
    return current_area->flags & AREA_FLAG_CAPITAL;
}

ChatChannelSource PlayerbotAI::GetChatChannelSource(Player* bot, uint32 type, std::string channelName)
{
    if (type == CHAT_MSG_CHANNEL)
    {
        if (channelName == "World")
        {
            return ChatChannelSource::SRC_WORLD;
        }
        else
        {
            ChannelMgr* cMgr = channelMgr(bot->GetTeam());
            if (!cMgr)
            {
                return ChatChannelSource::SRC_UNDEFINED;
            }

            const Channel* channel = cMgr->GetChannel(channelName, bot);

            if (channel)
            {
                switch (channel->GetChannelId())
                {
                case ChatChannelId::GENERAL:
                {
                    return ChatChannelSource::SRC_GENERAL;
                }
                case ChatChannelId::TRADE:
                {
                    return ChatChannelSource::SRC_TRADE;
                }
                case ChatChannelId::LOCAL_DEFENSE:
                {
                    return ChatChannelSource::SRC_LOCAL_DEFENSE;
                }
                case ChatChannelId::WORLD_DEFENSE:
                {
                    return ChatChannelSource::SRC_WORLD_DEFENSE;
                }
                case ChatChannelId::LOOKING_FOR_GROUP:
                {
                    return ChatChannelSource::SRC_LOOKING_FOR_GROUP;
                }
                case ChatChannelId::GUILD_RECRUITMENT:
                {
                    return ChatChannelSource::SRC_GUILD_RECRUITMENT;
                }
                default:
                    return ChatChannelSource::SRC_UNDEFINED;
                }
            }
        }
    }
    else
    {
        switch (type)
        {
        case CHAT_MSG_WHISPER:
        {
            return ChatChannelSource::SRC_WHISPER;
        }
        case CHAT_MSG_SAY:
        {
            return ChatChannelSource::SRC_SAY;
        }
        case CHAT_MSG_YELL:
        {
            return ChatChannelSource::SRC_YELL;
        }
        case CHAT_MSG_GUILD:
        {
            return ChatChannelSource::SRC_GUILD;
        }
        case CHAT_MSG_PARTY:
#ifdef MANGOSBOT_TWO
        case CHAT_MSG_PARTY_LEADER:
#endif
        {
            return ChatChannelSource::SRC_PARTY;
        }
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        {
            return ChatChannelSource::SRC_RAID;
        }
        case CHAT_MSG_EMOTE:
        {
            return ChatChannelSource::SRC_EMOTE;
        }
        case CHAT_MSG_TEXT_EMOTE:
        {
            return ChatChannelSource::SRC_TEXT_EMOTE;
        }
        default:
            return ChatChannelSource::SRC_UNDEFINED;
        }
    }

    return ChatChannelSource::SRC_UNDEFINED;
}

bool PlayerbotAI::SayToGuild(std::string msg, bool likePlayer)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    if (msg.empty())
    {
        return false;
    }

    if (bot->GetGuildId())
    {
        if (Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId()))
        {

            for (auto& player : sRandomPlayerbotMgr.GetPlayers())
            {
                if (player.second->GetGuildId() == bot->GetGuildId())
                {
                    if (likePlayer || (sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3) &&
                        sPlayerbotAIConfig.llmBotToBotChatChance))
                    {
                        WorldPacket packet_template(CMSG_MESSAGECHAT);

                        uint32 lang = LANG_UNIVERSAL;

                        packet_template << CHAT_MSG_GUILD;
                        packet_template << LANG_UNIVERSAL;
                        packet_template << msg;

                        std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(packet_template));

                        bot->GetSession()->QueuePacket(std::move(packetPtr));
                        return true;
                    }
                    break;
                }
            }
            if (!guild->HasRankRight(bot->GetRank(), GR_RIGHT_GCHATSPEAK))
            {
                return false;
            }

            guild->BroadcastToGuild(bot->GetSession(), msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::SayToWorld(std::string msg)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    Channel* worldChannel = cMgr->GetOrCreateChannel("World");
    if (!worldChannel)
        return false;

    if (!worldChannel->IsOn(bot->GetObjectGuid()))
        worldChannel->Join(bot->GetObjectGuid(), "");

    worldChannel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
    return true;
}

bool PlayerbotAI::SayToGeneral(std::string msg)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    AreaTableEntry const* current_zone = GetCurrentZone();
    if (!current_zone)
    {
        return false;
    }

    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        //check for current zone
        if (channel && channel->GetChannelId() == ChatChannelId::GENERAL
            && boost::algorithm::contains(channel->GetName(), GetLocalizedAreaName(current_zone)))
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::SayToTrade(std::string msg)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    //can only be used in major cities (bugged in TBC, players only join City versions if they logout/login within a city)
    if (!IsInCapitalCity())
    {
        return false;
    }

#ifndef MANGOSBOT_ZERO
    //Workaround for TBC - just reroute to General chat (if bot is in city)
    //Trade and GuildRecruitment channels are broken in TBC
    //Currently in magons TBC, if you switch zones, then you join "Trade - <zone>" and "GuildRecruitment - <zone>"
    //which is a core bug, should be "Trade - City" and "GuildRecruitment - City" in both 1.12 and TBC
    //but if you (actual player) logout in a city and log back in - you join "City" versions
    return SayToGeneral(msg);
#else
    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        if (channel && channel->GetChannelId() == ChatChannelId::TRADE
            && boost::algorithm::contains(channel->GetName(), GetLocalizedAreaName(GetAreaEntryByAreaID(ImportantAreaId::CITY))))
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
#endif
}

bool PlayerbotAI::SayToLFG(std::string msg)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

#ifndef MANGOSBOT_ZERO
    //Workaround for TBC - just reroute to General chat
    //LFG requires to be joined in LFG queue, so no point in using it
    return SayToGeneral(msg);
#else
    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        //check for current zone
        if (channel && channel->GetChannelId() == ChatChannelId::LOOKING_FOR_GROUP)
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
#endif
}

bool PlayerbotAI::SayToLocalDefense(std::string msg)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    AreaTableEntry const* current_zone = GetCurrentZone();
    if (!current_zone)
    {
        return false;
    }

    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        //check for current zone
        if (channel && channel->GetChannelId() == ChatChannelId::LOCAL_DEFENSE
            && boost::algorithm::contains(channel->GetName(), GetLocalizedAreaName(current_zone)))
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::SayToWorldDefense(std::string msg)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
#ifdef MANGOSBOT_ZERO
    //check if 11 honor rank
    if (bot->GetHonorRankInfo().rank < 11)
    {
        return false;
    }
#endif
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        if (channel && channel->GetChannelId() == ChatChannelId::WORLD_DEFENSE)
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::SayToGuildRecruitment(std::string msg)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    //check for bot's level? level 60?
    if (msg.empty())
    {
        return false;
    }

    ChannelMgr* cMgr = channelMgr(bot->GetTeam());
    if (!cMgr)
    {
        return false;
    }

    //can only be used in major cities
    if (!IsInCapitalCity())
    {
        return false;
    }

#ifndef MANGOSBOT_ZERO
    //Workaround for TBC - just reroute to General chat (if bot is in city)
    //Trade and GuildRecruitment channels are broken in TBC
    //Currently in magons TBC, if you switch zones, then you join "Trade - <zone>" and "GuildRecruitment - <zone>"
    //which is a core bug, should be "Trade - City" and "GuildRecruitment - City" in both 1.12 and TBC
    //but if you (actual player) logout in a city and log back in - you join "City" versions
    return SayToGeneral(msg);
#else
    for (auto const& [key, channel] : cMgr->GetChannels())
    {
        //check for current zone (can only be used in major cities)
        if (channel && channel->GetChannelId() == ChatChannelId::GUILD_RECRUITMENT
            && boost::algorithm::contains(channel->GetName(), GetLocalizedAreaName(GetAreaEntryByAreaID(ImportantAreaId::CITY))))
        {
            channel->Say(bot, msg.c_str(), LANG_UNIVERSAL);
            return true;
        }
    }

    return false;
#endif
}

bool PlayerbotAI::SayToParty(std::string msg, bool likePlayer)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    if (!bot->GetGroup())
    {
        return false;
    }

    if (likePlayer || (sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3) &&
        sPlayerbotAIConfig.llmBotToBotChatChance))
    {
        for (auto reciever : GetPlayersInGroup())
        {
            if (likePlayer || IsRealPlayer(reciever))
            {
                WorldPacket packet_template(CMSG_MESSAGECHAT);

                uint32 lang = LANG_UNIVERSAL;

                packet_template << CHAT_MSG_PARTY;
                packet_template << LANG_UNIVERSAL;
                packet_template << msg;

                std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(packet_template));

                bot->GetSession()->QueuePacket(std::move(packetPtr));
                return true;
            }
        }
    }

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, msg.c_str(), LANG_UNIVERSAL, CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());

    for (auto reciever : GetPlayersInGroup())
    {
        sServerFacade.SendPacket(reciever, data);
    }

    return true;
}

bool PlayerbotAI::SayToRaid(std::string msg)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    if (!bot->GetGroup() || !bot->GetGroup()->IsRaidGroup())
    {
        return false;
    }

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID, msg.c_str(), LANG_UNIVERSAL, CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());

    for (auto reciever : GetPlayersInGroup())
    {
        sServerFacade.SendPacket(reciever, data);
    }

    return true;
}

bool PlayerbotAI::Yell(std::string msg, bool likePlayer)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    uint32 lang = LANG_UNIVERSAL;
    if (bot->GetTeam() == ALLIANCE)
    {
        lang =  LANG_COMMON;
    }
    else
    {
        lang = LANG_ORCISH;
    }

    if (likePlayer || (sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3) &&
        sPlayerbotAIConfig.llmBotToBotChatChance))
    {
        if (likePlayer || this->HasPlayerNearby(sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_YELL)))
        {
            WorldPacket packet_template(CMSG_MESSAGECHAT);

            packet_template << CHAT_MSG_YELL;
            packet_template << lang;
            packet_template << msg;

            std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(packet_template));

            bot->GetSession()->QueuePacket(std::move(packetPtr));
            return true;
        }
    }

    bot->Yell(msg, lang);

    return true;
}

bool PlayerbotAI::Say(std::string msg, bool likePlayer)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    uint32 lang = LANG_UNIVERSAL;
    if (bot->GetTeam() == ALLIANCE)
    {
        lang =  LANG_COMMON;
    }
    else
    {
        lang = LANG_ORCISH;
    }

    if (likePlayer || (sPlayerbotAIConfig.llmEnabled > 0 && (HasStrategy("ai chat", BotState::BOT_STATE_NON_COMBAT) || sPlayerbotAIConfig.llmEnabled == 3) &&
        sPlayerbotAIConfig.llmBotToBotChatChance))
    {
        if (likePlayer || this->HasPlayerNearby(sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_SAY)))
        {

            WorldPacket packet_template(CMSG_MESSAGECHAT);

            packet_template << CHAT_MSG_SAY;
            packet_template << lang;
            packet_template << msg;

            std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(packet_template));

            bot->GetSession()->QueuePacket(std::move(packetPtr));
            return true;
        }
    }

    bot->Say(msg, lang);

    return true;
}

bool PlayerbotAI::Whisper(std::string msg, std::string receiverName, bool likePlayer)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;
    ObjectGuid receiver = sObjectMgr.GetPlayerGuidByName(receiverName);
    Player* rPlayer = sObjectMgr.GetPlayer(receiver);

    if (!rPlayer)
    {
        return false;
    }

    if (rPlayer == bot)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, msg.c_str(), LANG_UNIVERSAL, bot->GetChatTag(), bot->GetObjectGuid(), bot->GetName());
        rPlayer->GetSession()->SendPacket(data);

        return true;
    }

    if (bot->GetTeam() == ALLIANCE)
    {
        bot->Whisper(msg, LANG_COMMON, receiver);
    }
    else
    {
        bot->Whisper(msg, LANG_ORCISH, receiver);
    }

    return true;
}

bool PlayerbotAI::TellPlayerNoFacing(Player* player, std::string text, PlayerbotSecurityLevel securityLevel, bool isPrivate, bool noRepeat, bool ignoreSilent)
{
    if(!player)
        return false;

    /*
     * AND `ignoreSilent` DOES NOT REACH A BOT NPC. Every caller that passes it
     * is answering a direct question, which is a fair reason to speak over a
     * companion's `+silent` -- and is not a thing that happens to scenery. A
     * bot NPC is mute whatever the caller thinks.
     */
    if (!MaySpeak())
        return false;

    if (!ignoreSilent && HasStrategy("silent", BotState::BOT_STATE_NON_COMBAT))
        return false;

    time_t lastSaid = whispers[text];
    if (!noRepeat || !lastSaid || (time(0) - lastSaid) >= sPlayerbotAIConfig.repeatDelay / 1000)
    {
        whispers[text] = time(0);

        if (m_recordMessages)
        {
            m_recordedMessages.push_back(text);
        }

        std::vector<Player*> recievers;

        ChatMsg type = CHAT_MSG_SYSTEM;

        if (!isPrivate && bot->GetGroup())
        {
            recievers = GetPlayersInGroup();
            if(!recievers.empty())
            {
                type = bot->GetGroup()->IsRaidGroup() ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
            }
        }

        if (type == CHAT_MSG_SYSTEM && HasRealPlayerMaster())
            type = CHAT_MSG_WHISPER;

        if (type == CHAT_MSG_SYSTEM && (sPlayerbotAIConfig.randomBotSayWithoutMaster || HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT)))
            type = CHAT_MSG_SAY;

        if (type == CHAT_MSG_SYSTEM && IsRealPlayer(player))
            type = CHAT_MSG_WHISPER;

        if ((sPlayerbotAIConfig.hasLog("chat_log.csv") && HasStrategy("debug log", BotState::BOT_STATE_NON_COMBAT)) || HasStrategy("debug logname", BotState::BOT_STATE_NON_COMBAT))
        {
            std::ostringstream out;
            out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
            out << bot->GetName() << ",";
            out << std::fixed << std::setprecision(2);

            out << std::to_string(bot->getRace()) << ",";
            out << std::to_string(bot->getClass()) << ",";
            float subLevel = GetLevelFloat();

            out << subLevel << ",";
            out << std::fixed << std::setprecision(2);
            WorldPosition(bot).printWKT(out);

            out << type << ",";
            out << text;

            if (HasStrategy("debug logname", BotState::BOT_STATE_NON_COMBAT))
            {
                std::string fileName = "chat_log_";
                fileName += bot->GetName();
                fileName += ".csv";
                if (!sPlayerbotAIConfig.isLogOpen(fileName))
                    sPlayerbotAIConfig.openLog(fileName, "a", true);
                sPlayerbotAIConfig.log(fileName, out.str().c_str());
            }
            else
                sPlayerbotAIConfig.log("chat_log.csv", out.str().c_str());
        }

        WorldPacket data;

        switch (type)
        {
            case CHAT_MSG_SAY:
            {
                bot->Say(text, (bot->GetTeam() == ALLIANCE ? LANG_COMMON : LANG_ORCISH));
                return true;
            }

            case CHAT_MSG_RAID:
            {
                this->SayToRaid(text.c_str());

                return true;
            }
            case CHAT_MSG_PARTY:
            {
                SayToParty(text.c_str());

                return true;
            }

            case CHAT_MSG_WHISPER:
            {
                if (!IsTellAllowed(player, securityLevel))
                    return false;

                if (!HasRealPlayerMaster() && !IsRealPlayer(player))
                    return false;

                whispers[text] = time(0);

                if (currentChat.second >= time(0))
                   type = currentChat.first;

                if (type == CHAT_MSG_ADDON)
                {
                    text = "BOT\t" + text;

                    ChatHandler::BuildChatPacket(data, type == CHAT_MSG_ADDON ? CHAT_MSG_PARTY : type, text.c_str(), type == CHAT_MSG_ADDON ? LANG_ADDON : LANG_UNIVERSAL, CHAT_TAG_NONE, bot->GetObjectGuid(), bot->GetName());
                    sServerFacade.SendPacket(player, data);
                    return true;
                }

                this->Whisper(text, player->GetName());
                return true;
            }

            default:
                return true;
        }
    }

    return true;
}

bool PlayerbotAI::TellError(Player* player, std::string text, PlayerbotSecurityLevel securityLevel, bool ignoreSilent)
{
    // A bot NPC never volunteers chat. MaySpeak() has the reasoning.
    if (!MaySpeak())
        return false;

    if (!IsTellAllowed(player, securityLevel) || !IsSafe(player) || GetBotAI(player))
        return false;

    if (!ignoreSilent && HasStrategy("silent", BotState::BOT_STATE_NON_COMBAT))
        return false;

    PlayerbotMgr* mgr = GetBotMgr(player);
    if (mgr) mgr->TellError(bot->GetName(), text);

    return false;
}

bool PlayerbotAI::IsTellAllowed(Player* player, PlayerbotSecurityLevel securityLevel)
{
    if (!player || player->IsBeingTeleported())
        return false;

    if (!GetSecurity()->CheckLevelFor(securityLevel, true, player))
        return false;

    if (sPlayerbotAIConfig.whisperDistance && !bot->GetGroup() && sRandomPlayerbotMgr.IsFreeBot(bot) &&
            player->GetSession()->GetSecurity() < SEC_GAMEMASTER &&
            (bot->GetMapId() != player->GetMapId() || sServerFacade.GetDistance2d(bot, player) > sPlayerbotAIConfig.whisperDistance))
        return false;

    return true;
}

bool PlayerbotAI::TellPlayer(Player* player, std::string text, PlayerbotSecurityLevel securityLevel, bool isPrivate, bool ignoreSilent)
{
    if (!TellPlayerNoFacing(player, text, securityLevel, isPrivate, ignoreSilent))
        return false;

    if (player && !player->IsBeingTeleported() && !sServerFacade.isMoving(bot) && !sServerFacade.IsInCombat(bot) && bot->GetMapId() == player->GetMapId() && !bot->IsTaxiFlying() && !bot->IsFlying())
    {
        if (!sServerFacade.IsInFront(bot, player, sPlayerbotAIConfig.sightDistance, EMOTE_ANGLE_IN_FRONT))
            sServerFacade.SetFacingTo(bot, player);

        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
    }

    return true;
}

bool IsRealAura(Player* bot, Aura const* aura, Unit* unit)
{
    if (!aura)
        return false;

    if (!sServerFacade.IsHostileTo(unit, bot))
        return true;

    uint32 stacks = aura->GetStackAmount();
    if (stacks >= aura->GetSpellProto()->StackAmount)
        return true;

    if (aura->GetCaster() == bot || IsPositiveSpell(aura->GetSpellProto()) || aura->IsAreaAura())
        return true;

    return false;
}

bool PlayerbotAI::HasAura(std::string name, Unit* unit, bool maxStack, bool checkIsOwner, int maxAuraAmount, bool hasMyAura, int minDuration, int auraTypeId)
{
    if (!unit)
        return false;

    std::wstring wnamepart;

    std::vector<uint32> ids = chatHelper.SpellIds(name);

    if (ids.empty())
    {
        //sLog.outError("Please add %s to spell list", name.c_str());
        if (!Utf8toWStr(name, wnamepart))
            return 0;

        wstrToLower(wnamepart);
    }

    int auraAmount = 0;

	for (uint32 auraType = SPELL_AURA_BIND_SIGHT; auraType < TOTAL_AURAS; auraType++)
	{
        if (auraTypeId != TOTAL_AURAS && auraType != auraTypeId)
            continue;

		Unit::AuraList const& auras = unit->GetAurasByType((AuraType)auraType);

        if (auras.empty())
            continue;

        for (Unit::AuraList::const_iterator i = auras.begin(); i != auras.end(); i++)
        {
            Aura* aura = *i;
            if (!aura)
                continue;

            if (ids.empty())
            {
                const std::string auraName = aura->GetSpellProto()->SpellName[0];
                if (auraName.empty() || auraName.length() != wnamepart.length() || !Utf8FitTo(auraName, wnamepart))
                    continue;
            }
            else
            {
                if (std::find(ids.begin(), ids.end(), aura->GetSpellProto()->Id) == ids.end())
                    continue;
            }

			if (IsRealAura(bot, aura, unit))
            {
                if (hasMyAura && aura->GetHolder())
                {
                    if (aura->GetHolder()->GetCasterGuid() == bot->GetObjectGuid())
                        return true;
                    else
                        continue;
                }

                if (checkIsOwner && aura->GetHolder())
                {
                    if (aura->GetHolder()->GetCasterGuid() != bot->GetObjectGuid())
                        continue;
                }

                uint32 maxStackAmount = aura->GetSpellProto()->StackAmount;
                uint32 maxProcCharges = aura->GetSpellProto()->procCharges;

                if (maxStack)
                {
                    if (maxStackAmount && aura->GetStackAmount() >= maxStackAmount)
                        auraAmount++;

                    if (maxProcCharges && aura->GetHolder()->GetAuraCharges() >= maxProcCharges)
                        auraAmount++;
                }
                else
                    auraAmount++;

                bool minDurationPassed = true;

                if (minDuration > 0)
                {
                    int32 auraDuration = aura->GetHolder()->GetAuraDuration();
                    minDurationPassed = minDuration <= auraDuration;
                }

                if (maxAuraAmount < 0 && minDurationPassed)
                    return auraAmount > 0;
            }
		}
    }

    if (maxAuraAmount >= 0)
    {
        return auraAmount == maxAuraAmount || (auraAmount > 0 && auraAmount <= maxAuraAmount);
    }

    return false;
}

bool PlayerbotAI::HasAura(uint32 spellId, Unit* unit, bool checkOwner)
{
    Aura* aura = GetAura(spellId, unit, checkOwner);
    return aura != nullptr;
}

Aura* PlayerbotAI::GetAura(uint32 spellId, Unit* unit, bool checkIsOwner)
{
    Aura* aura = nullptr;
    if (spellId != 0 && unit)
    {
        for (uint32 effect = EFFECT_INDEX_0; effect <= EFFECT_INDEX_2; effect++)
        {
            Aura* auraTmp = ((Unit*)unit)->GetAura(spellId, (SpellEffectIndex)effect);
            if (IsRealAura(bot, auraTmp, (Unit*)unit))
            {
                if (checkIsOwner)
                {
                    if (aura->GetHolder() && aura->GetHolder()->GetCasterGuid() == bot->GetObjectGuid())
                    {
                        aura = auraTmp;
                        break;
                    }
                }
                else
                {
                    aura = auraTmp;
                    break;
                }
            }
        }
    }

    return aura;
}

Aura* PlayerbotAI::GetAura(std::string name, Unit* unit, bool checkIsOwner)
{
    if (!name.empty() && unit)
    {
        std::wstring wnamepart;
        std::vector<uint32> ids = chatHelper.SpellIds(name);

        if (ids.empty())
        {
            //sLog.outError("Please add %s to spell list", name.c_str());
            if (!Utf8toWStr(name, wnamepart))
                return 0;

            wstrToLower(wnamepart);
        }

        for (uint32 auraType = SPELL_AURA_BIND_SIGHT; auraType < TOTAL_AURAS; auraType++)
        {
            Unit::AuraList const& auras = unit->GetAurasByType((AuraType)auraType);

            if (auras.empty())
                continue;

            for (Unit::AuraList::const_iterator i = auras.begin(); i != auras.end(); i++)
            {
                Aura* aura = *i;
                if (!aura)
                    continue;

                if (ids.empty())
                {
                    const std::string auraName = aura->GetSpellProto()->SpellName[0];
                    if (auraName.empty() || auraName.length() != wnamepart.length() || !Utf8FitTo(auraName, wnamepart))
                        continue;
                }
                else
                {
                    if (std::find(ids.begin(), ids.end(), aura->GetSpellProto()->Id) == ids.end())
                        continue;
                }

                if (IsRealAura(bot, aura, unit))
                {
                    if (checkIsOwner && aura->GetHolder())
                    {
                        if (aura->GetHolder()->GetCasterGuid() == bot->GetObjectGuid())
                        {
                            return aura;
                        }
                    }
                    else
                    {
                        return aura;
                    }
                }
            }
        }
    }

    return nullptr;
}

std::vector<Aura*> PlayerbotAI::GetAuras(Unit* unit, bool allAuras, bool positive)
{
    std::vector<Aura*> outAuras;
    for (uint32 auraType = SPELL_AURA_BIND_SIGHT; auraType < TOTAL_AURAS; auraType++)
    {
        Unit::AuraList const& auras = unit->GetAurasByType((AuraType)auraType);

        if (auras.empty())
            continue;

        for (Unit::AuraList::const_iterator i = auras.begin(); i != auras.end(); i++)
        {
            Aura* aura = *i;
            if (aura)
            {
                if (allAuras || (positive && aura->IsPositive()) || (!positive && !aura->IsPositive()))
                {
                    outAuras.push_back(aura);
                }
            }
        }
    }

    return outAuras;
}

bool PlayerbotAI::HasAnyAuraOf(Unit* player, ...)
{
    if (!player)
        return false;

    va_list vl;
    va_start(vl, player);

    const char* cur;
    do {
        cur = va_arg(vl, const char*);
        if (cur && HasAura(cur, player)) {
            va_end(vl);
            return true;
        }
    }
    while (cur);

    va_end(vl);
    return false;
}

bool PlayerbotAI::GetSpellRange(std::string name, float* maxRange, float* minRange)
{
    // Copied from Spell::GetMinMaxRange
    const uint32 spellId = aiObjectContext->GetValue<uint32>("spell id", name)->Get();
    if(spellId)
    {
        const SpellEntry* spellInfo = sServerFacade.LookupSpellInfo(spellId);
        if (spellInfo)
        {
            const SpellRangeEntry* spellRangeEntry = sServerFacade.LookupSpellRangeEntry(spellInfo->rangeIndex);
            if(spellRangeEntry)
            {
                float spellMinRange = 0.0f, spellMaxRange = 0.0f, rangeMod = 0.0f;
                if (spellRangeEntry->Flags & SPELL_RANGE_FLAG_MELEE)
                {
                    rangeMod = bot->GetCombinedCombatReach(bot, true, 0.f);
                }
                else
                {
                    float meleeRange = 0.0f;
                    if (spellRangeEntry->Flags & SPELL_RANGE_FLAG_RANGED)
                    {
                        meleeRange = bot->GetCombinedCombatReach(bot, true, 0.f);
                    }

                    spellMinRange = spellRangeEntry->minRange + meleeRange;
                    spellMaxRange = spellRangeEntry->maxRange;
                }

                if (spellInfo->HasAttribute(SPELL_ATTR_USES_RANGED_SLOT))
                {
                    if (Item* rangedWeapon = bot->GetWeaponForAttack(RANGED_ATTACK))
                    {
                        spellMaxRange *= rangedWeapon->GetProto()->RangedModRange * 0.01f;
                    }
                }

                if (Player* modOwner = bot->GetSpellModOwner())
                {
                    modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_RANGE, spellMaxRange);
                }

                spellMaxRange += rangeMod;

                if(maxRange)
                {
                    *maxRange = spellMaxRange;
                }

                if(minRange)
                {
                    *minRange = spellMinRange;
                }

                return true;
            }
        }
    }

    return false;
}

uint32 PlayerbotAI::GetSpellCastDuration(Spell* spell)
{
    uint32 spellDuration = 0;
    if(spell)
    {
        const SpellEntry* const pSpellInfo = spell->m_spellInfo;
        spellDuration = spell->GetCastTime();
        if (IsChanneledSpell(pSpellInfo))
        {
            int32 duration = GetSpellDuration(pSpellInfo);
            if (duration > 0)
            {
                spellDuration += duration;
            }
        }

        spellDuration = ceil(spellDuration);

        // fix Feign Death
        if (pSpellInfo->Id == 5384)
        {
            spellDuration = 1000;
        }
        // fix cannibalize
        else if (pSpellInfo->Id == 20577)
        {
            spellDuration = 10000;
        }

        uint32 globalCooldown = CalculateGlobalCooldown(pSpellInfo->Id);
        if (spellDuration < (int32)globalCooldown)
        {
            spellDuration = globalCooldown;
        }
    }

    return spellDuration + sPlayerbotAIConfig.reactDelay;
}

bool PlayerbotAI::HasSpell(std::string name) const
{
    return HasSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get());
}

bool PlayerbotAI::HasSpell(uint32 spellid) const
{
    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid))
    {
        return true;
    }
    else if (bot->HasSpell(spellid))
    {
        return true;
    }

    return false;
}

SpellCastResult PlayerbotAI::CheckSpellTargetAlignment(SpellEntry const* spellInfo, Unit* target)
{
    // Consider neutral spells (spells that are neither positive or negative (e.g. feign death, hunter traps, ...)
    const std::list<uint32> neutralSpells = { 1499, 5384, 13795, 13809, 13813, 14302, 14303, 14304, 14305, 14310, 14311, 14316, 14317, 27023, 27025, 34600, 49055, 49056, 49066, 49067 };
    const bool neutralSpell = std::find(neutralSpells.begin(), neutralSpells.end(), spellInfo->Id) != neutralSpells.end();
    if (neutralSpell)
        return SPELL_CAST_OK;

    const bool positiveSpell = IsPositiveSpell(spellInfo);
    if (positiveSpell && sServerFacade.IsHostileTo(bot, target))
        return SPELL_FAILED_TARGET_ENEMY;

    if (!positiveSpell && sServerFacade.IsFriendlyTo(bot, target))
        return SPELL_FAILED_TARGET_FRIENDLY;

    return SPELL_CAST_OK;
}

bool PlayerbotAI::CanCastSpell(std::string name, Unit* target, uint8 effectMask, Item* itemTarget, bool ignoreRange, bool ignoreInCombat, bool ignoreMount, SpellCastResult* checkResult)
{
    return CanCastSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get(), target, 0, true, itemTarget, ignoreRange, ignoreInCombat, ignoreMount, checkResult);
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, Unit* target, uint8 effectMask, bool checkHasSpell, Item* itemTarget, bool ignoreRange, bool ignoreInCombat, bool ignoreMount, SpellCastResult* checkResult)
{
    if (!spellid)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid) && pet->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_CAST_OK;
        }

        return true;
    }

    if (bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        // Spells that can be casted while out of control
        const std::list<uint32> ignoreOutOfControllSpells = { 642, 1020, 1499, 1953, 7744, 11958, 13795, 13809, 13813, 14302, 14303, 14304, 14305, 14310, 14311, 14316, 14317, 27023, 27025, 34600, 49055, 49056, 49066, 49067 };
        if (std::find(ignoreOutOfControllSpells.begin(), ignoreOutOfControllSpells.end(), spellid) == ignoreOutOfControllSpells.end())
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_NOT_IN_CONTROL;
            }

            return false;
        }
    }

    if (!target)
        target = bot;

    if (checkHasSpell && !HasSpell(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    if (!bot->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_READY;
        }

        return false;
    }

	SpellEntry const *spellInfo = sServerFacade.LookupSpellInfo(spellid);
	if (!spellInfo)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    // already active next melee swing spell
    if (IsNextMeleeSwingSpell(spellInfo))
    {
        Spell* autorepeatSpell = bot->GetCurrentSpell(CURRENT_MELEE_SPELL);
        if (autorepeatSpell)
        {
            if (autorepeatSpell->m_spellInfo->Id == spellInfo->Id)
            {
                if (checkResult)
                {
                    *checkResult = SPELL_FAILED_SPELL_IN_PROGRESS;
                }

                return false;
            }
        }
    }

	if (!itemTarget)
	{
        // Consider neutral spells (spells that are neither positive or negative (e.g. feign death, hunter traps, ...)
        const std::list<uint32> neutralSpells = { 1499, 5384, 13795, 13809, 13813, 14302, 14303, 14304, 14305, 14310, 14311, 14316, 14317, 27023, 27025, 34600, 49055, 49056, 49066, 49067 };
        const bool neutralSpell = std::find(neutralSpells.begin(), neutralSpells.end(), spellid) != neutralSpells.end();
        if(!neutralSpell)
        {
            const bool positiveSpell = IsPositiveSpell(spellInfo);
            if (positiveSpell && sServerFacade.IsHostileTo(bot, target))
            {
                if (checkResult)
                {
                    *checkResult = SPELL_FAILED_TARGET_ENEMY;
                }

                return false;
            }

            if (!positiveSpell && sServerFacade.IsFriendlyTo(bot, target))
            {
                if (checkResult)
                {
                    *checkResult = SPELL_FAILED_TARGET_FRIENDLY;
                }
                
                return false;
            }
        }

        bool damage = false;
        for (int32 i = EFFECT_INDEX_0; i <= EFFECT_INDEX_2; i++)
        {
            // direct damage
            if (spellInfo->Effect[(SpellEffectIndex)i] == SPELL_EFFECT_SCHOOL_DAMAGE)
            {
                damage = true;
                break;
            }
            // periodic damage
            if (spellInfo->Effect[(SpellEffectIndex)i] == SPELL_EFFECT_APPLY_AURA && spellInfo->EffectBasePoints[i] >= 0)
            {
                if (spellInfo->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE)
                {
                    damage = true;
                    break;
                }
            }
        }

        if (!damage)
        {
            for (int32 i = EFFECT_INDEX_0; i <= EFFECT_INDEX_2; i++)
            {
                bool immune = target->IsImmuneToSpellEffect(spellInfo, (SpellEffectIndex)i, false);
                if (immune)
                {
                    if (checkResult)
                    {
                        *checkResult = SPELL_FAILED_IMMUNE;
                    }

                    return false;
                }
            }
        }

        if (!ignoreRange && bot != target && sServerFacade.GetDistance2d(bot, target) > sPlayerbotAIConfig.sightDistance)
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_OUT_OF_RANGE;
            }

            return false;
        }
	}

	ObjectGuid oldSel = bot->GetSelectionGuid();
	//bot->SetSelectionGuid(target->GetObjectGuid());
	Spell *spell = new Spell(bot, spellInfo, false);

    spell->m_targets.setUnitTarget(target);
    spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get());
    spell->m_targets.setItemTarget(spell->GetCastItem());

    SpellCastResult result = spell->CheckCast(true);
    delete spell;
	//if (oldSel)
	//	bot->SetSelectionGuid(oldSel);

    if (checkResult)
    {
        *checkResult = result;
    }

    switch (result)
    {
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_CAST_OK:
            return true;
        case SPELL_FAILED_OUT_OF_RANGE:
        case SPELL_FAILED_LINE_OF_SIGHT:
            return ignoreRange;
        case SPELL_FAILED_AFFECTING_COMBAT:
            return ignoreInCombat;
        case SPELL_FAILED_NOT_MOUNTED:
            return ignoreMount;
        default:
            return false;
    }
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, GameObject* goTarget, uint8 effectMask, bool checkHasSpell, bool ignoreRange, bool ignoreInCombat, bool ignoreMount, SpellCastResult* checkResult)
{
    if (!spellid)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid) && pet->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_CAST_OK;
        }

        return true;
    }

    if (bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        // Spells that can be casted while out of control
        const std::list<uint32> ignoreOutOfControllSpells = { 642, 1020, 1499, 1953, 7744, 11958, 13795, 13809, 13813, 14302, 14303, 14304, 14305, 14310, 14311, 14316, 14317, 27023, 27025, 34600, 49055, 49056, 49066, 49067 };
        if (std::find(ignoreOutOfControllSpells.begin(), ignoreOutOfControllSpells.end(), spellid) == ignoreOutOfControllSpells.end())
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_NOT_IN_CONTROL;
            }

            return false;
        }
    }

    if (checkHasSpell && !HasSpell(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    if (!bot->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_READY;
        }

        return false;
    }

    SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(spellid);
    if (!spellInfo)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    bool damage = false;
    for (int32 i = EFFECT_INDEX_0; i <= EFFECT_INDEX_2; i++)
    {
        if (spellInfo->Effect[(SpellEffectIndex)i] == SPELL_EFFECT_SCHOOL_DAMAGE)
        {
            damage = true;
            break;
        }
    }

    if (sServerFacade.GetDistance2d(bot, goTarget) > sPlayerbotAIConfig.sightDistance)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_OUT_OF_RANGE;
        }

        return false;
    }

    //ObjectGuid oldSel = bot->GetSelectionGuid();
    bot->SetSelectionGuid(goTarget->GetObjectGuid());
    Spell* spell = new Spell(bot, spellInfo, false);

    spell->m_targets.setGOTarget(goTarget);
    spell->SetCastItem(aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get());
    spell->m_targets.setItemTarget(spell->GetCastItem());

    SpellCastResult result = spell->CheckCast(true);
    delete spell;
    //if (oldSel)
    //    bot->SetSelectionGuid(oldSel);

    if (checkResult)
    {
        *checkResult = result;
    }

    switch (result)
    {
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_CAST_OK:
            return true;
        case SPELL_FAILED_OUT_OF_RANGE:
            return ignoreRange;
        case SPELL_FAILED_AFFECTING_COMBAT:
            return ignoreInCombat;
        case SPELL_FAILED_NOT_MOUNTED:
            return ignoreMount;
        default:
            return false;
    }
}

bool PlayerbotAI::CanCastSpell(uint32 spellid, float x, float y, float z, uint8 effectMask, bool checkHasSpell, Item* itemTarget, bool ignoreRange, bool ignoreInCombat, bool ignoreMount, SpellCastResult* checkResult)
{
    if (!spellid)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellid) && pet->IsSpellReady(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_CAST_OK;
        }

        return true;
    }

    if (bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        // Spells that can be casted while out of control
        const std::list<uint32> ignoreOutOfControllSpells = { 642, 1020, 1499, 1953, 7744, 11958, 13795, 13809, 13813, 14302, 14303, 14304, 14305, 14310, 14311, 14316, 14317, 27023, 27025, 34600, 49055, 49056, 49066, 49067 };
        if (std::find(ignoreOutOfControllSpells.begin(), ignoreOutOfControllSpells.end(), spellid) == ignoreOutOfControllSpells.end())
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_NOT_IN_CONTROL;
            }

            return false;
        }
    }

    if (checkHasSpell && !HasSpell(spellid))
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(spellid);
    if (!spellInfo)
    {
        if (checkResult)
        {
            *checkResult = SPELL_FAILED_NOT_KNOWN;
        }

        return false;
    }

    if (!itemTarget)
    {
        if (sqrt(bot->GetDistance(x,y,z, DIST_CALC_NONE)) > sPlayerbotAIConfig.sightDistance)
        {
            if (checkResult)
            {
                *checkResult = SPELL_FAILED_OUT_OF_RANGE;
            }

            return false;
        }
    }

    Spell* spell = new Spell(bot, spellInfo, false);

    spell->m_targets.setDestination(x, y, z);
    spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellid)->Get());
    spell->m_targets.setItemTarget(spell->GetCastItem());

    SpellCastResult result = spell->CheckCast(true);
    delete spell;

    if (checkResult)
    {
        *checkResult = result;
    }

    switch (result)
    {
        case SPELL_FAILED_NOT_INFRONT:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_UNIT_NOT_INFRONT:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_CAST_OK:
            return true;
        case SPELL_FAILED_OUT_OF_RANGE:
            return ignoreRange;
        case SPELL_FAILED_AFFECTING_COMBAT:
            return ignoreInCombat;
        case SPELL_FAILED_NOT_MOUNTED:
            return ignoreMount;
        default:
            return false;
    }
}

uint8 PlayerbotAI::GetHealthPercent(const Unit& target) const
{
   return (static_cast<float>(target.GetHealth()) / target.GetMaxHealth()) * 100;
}

uint8 PlayerbotAI::GetHealthPercent() const
{
   return GetHealthPercent(*bot);
}

uint8 PlayerbotAI::GetManaPercent(const Unit& target) const
{
   return (static_cast<float>(target.GetPower(POWER_MANA)) / target.GetMaxPower(POWER_MANA)) * 100;
}

uint8 PlayerbotAI::GetManaPercent() const
{
   return GetManaPercent(*bot);
}

bool PlayerbotAI::CastSpell(std::string name, Unit* target, Item* itemTarget, bool waitForSpell, uint32* outSpellDuration)
{
    bool result = CastSpell(aiObjectContext->GetValue<uint32>("spell id", name)->Get(), target, itemTarget, waitForSpell, outSpellDuration);
    if (result)
    {
        aiObjectContext->GetValue<time_t>("last spell cast time", name)->Set(time(0));
    }

    return result;
}

bool PlayerbotAI::CastSpell(uint32 spellId, Unit* target, Item* itemTarget, bool waitForSpell, uint32* outSpellDuration)
{
    if (!spellId)
        return false;

    if (!target)
        target = bot;

    // Catches both "no target given" (defaulted to self above) and "current target"
    // having already resolved to the bot's own Unit* before CastSpell was ever called -
    // the latter sailed past the old !target-only check with a perfectly valid non-null
    // pointer that just happened to be self, so a harmful spell like Fireball could still
    // land on the caster.
    if (target == bot)
    {
        SpellEntry const* fallbackSpellInfo = sServerFacade.LookupSpellInfo(spellId);
        if (fallbackSpellInfo && CheckSpellTargetAlignment(fallbackSpellInfo, bot) != SPELL_CAST_OK)
        {
            sLog.outDetail("PlayerbotAI::CastSpell: %s target resolved to self for harmful spell %u - refusing to self-cast", bot->GetName(), spellId);
            return false;
        }
    }

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellId))
    {
        return CastPetSpell(spellId, target);
    }

    aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(NULL);
    aiObjectContext->GetValue<time_t>("stay time")->Set(0);

    MotionMaster &mm = *bot->GetMotionMaster();

    if (bot->IsFlying() || bot->IsTaxiFlying())
        return false;

	//bot->clearUnitState(UNIT_STAT_CHASE);
	//bot->clearUnitState(UNIT_STAT_FOLLOW);

	bool failWithDelay = false;
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        failWithDelay = true;
    }

	ObjectGuid oldSel = bot->GetSelectionGuid();
	bot->SetSelectionGuid(target->GetObjectGuid());

    WorldObject* faceTo = target;
    if (!sServerFacade.IsInFront(bot, faceTo, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
    {
        sServerFacade.SetFacingTo(bot, faceTo);
        // ServerFacade::SetFacingTo only turns instantly (Unit::SetOrientation) while the bot
        // is stationary; while moving it's a spline-based turn that takes real time to finish,
        // which is what this GCD-delay-and-retry was guarding against. For a stationary bot
        // the facing was just corrected synchronously above, so proceed with this same cast
        // attempt instead of deferring - deferring here against a moving target (wandering
        // wildlife, etc.) meant the target had already drifted again by the retry, so the bot
        // never actually caught up and stayed stuck failing to cast indefinitely.
        if (!HasRealPlayerMaster() && !bot->IsStopped())
            failWithDelay = true;
    }

    if (failWithDelay)
    {
        if(waitForSpell)
        {
            SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        }

        if(outSpellDuration)
        {
            *outSpellDuration = sPlayerbotAIConfig.globalCoolDown;
        }

        return false;
    }

    const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
    Spell *spell = new Spell(bot, pSpellInfo, false);

    SpellCastTargets targets;
    if ((pSpellInfo->Targets & TARGET_FLAG_ITEM) || spellId == 1804)
    {
        spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get());
        targets.setItemTarget(spell->GetCastItem());

        if (bot->GetTradeData())
        {
            bot->GetTradeData()->SetSpell(spellId);
			delete spell;
            return true;
        }
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        WorldLocation aoe = aiObjectContext->GetValue<WorldLocation>("aoe position")->Get();
        if (aoe.coord_x != 0)
            targets.setDestination(aoe.coord_x, aoe.coord_y, aoe.coord_z);
        else if (target && target->GetObjectGuid() != bot->GetObjectGuid())
            targets.setDestination(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.setDestination(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    }
    else
    {
        targets.setUnitTarget(target);
    }

    if (spellId == 1953) // simulate blink coordinates
    {
        float angle = bot->GetOrientation();
        float distance = 20.0f;
        float fx = bot->GetPositionX() + cos(angle) * distance;
        float fy = bot->GetPositionY() + sin(angle) * distance;
        float fz = bot->GetPositionZ();

        float ox, oy, oz;
        bot->GetPosition(ox, oy, oz);
//#ifdef MANGOSBOT_TWO
//        bot->GetMap()->GetHitPosition(ox, oy, oz + max_height, fx, fy, fz, bot->GetPhaseMask(), -0.5f);
//#else
//        bot->GetMap()->GetHitPosition(ox, oy, oz + 2.0f, fx, fy, fz, -0.5f);
//#endif
        bot->UpdateAllowedPositionZ(fx, fy, fz);
        targets.setDestination(fx, fy, fz);
    }

    if (pSpellInfo->Effect[0] == SPELL_EFFECT_OPEN_LOCK ||
        pSpellInfo->Effect[0] == SPELL_EFFECT_SKINNING)
    {
        LootObject loot = *aiObjectContext->GetValue<LootObject>("loot target");
        GameObject* go = GetGameObject(loot.guid);
        if (go && sServerFacade.isSpawned(go))
        {
            targets.setGOTarget(go);
            faceTo = go;
        }
        else
        {
            Unit* creature = GetUnit(loot.guid);
            if (creature)
            {
                targets.setUnitTarget(creature);
                faceTo = creature;
            }
        }
    }

    // Fail the cast if the bot is moving and the spell is a casting/channeled spell
    if (sServerFacade.isMoving(bot) && ((GetSpellCastTime(pSpellInfo, bot, spell) > 0) || (IsChanneledSpell(pSpellInfo) && (GetSpellDuration(pSpellInfo) > 0))))
    {
        // always fail when jumping
        if (IsJumping() || bot->IsFalling())
        {
            spell->cancel();
            return false;
        }

        StopMoving();

        // fail if not with real player to avoid movement glitches
        if (!HasActivePlayerMaster())
        {
            if (waitForSpell)
            {
                SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
            }

            spell->cancel();
            return false;
        }
    }

    for (uint32 j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        uint8 slot = 0;
        switch (pSpellInfo->Effect[j])
        {
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT1: slot = 0; break;
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT2: slot = 1; break;
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT3: slot = 2; break;
        case SPELL_EFFECT_SUMMON_OBJECT_SLOT4: slot = 3; break;
        default: continue;
        }

        if (ObjectGuid guid = bot->m_ObjectSlotGuid[slot])
        {
            Map* const goMap = bot ? bot->FindMap() : nullptr;
            if (GameObject* obj = goMap ? goMap->GetGameObject(guid) : nullptr)
            {
                //Object is not mine because I created an object with same guid on different map. 
                //Make object mine, remove it from my list and give it back to the original owner.
                if (obj->GetOwnerGuid() != bot->GetObjectGuid())
                {
                    ObjectGuid ownerGuid = obj->GetOwnerGuid();
                    obj->SetOwnerGuid(bot->GetObjectGuid());
                    obj->SetLootState(GO_JUST_DEACTIVATED);

                    bot->RemoveGameObject(obj, false, pSpellInfo->Id != obj->GetSpellId());
                    bot->m_ObjectSlotGuid[slot].Clear();

                    obj->SetOwnerGuid(ownerGuid);
                }
            }
        }
    }


    SpellCastResult spellSuccess = spell->SpellStart(&targets);

    if (pSpellInfo->Effect[0] == SPELL_EFFECT_OPEN_LOCK ||
        pSpellInfo->Effect[0] == SPELL_EFFECT_SKINNING)
    {
        if (!spell->m_targets.getItemTarget())
        {
            LootObject loot = *aiObjectContext->GetValue<LootObject>("loot target");
            if (!loot.IsLootPossible(bot))
            {
                spell->cancel();
                //delete spell;
                return false;
            }
        }
    }

    if (spellSuccess != SPELL_CAST_OK)
        return false;

    PlayAttackEmote(6);

    if(waitForSpell)
    {
        WaitForSpellCast(spell);
    }

    if(outSpellDuration)
    {
        *outSpellDuration = GetSpellCastDuration(spell);
    }

    if (spell->GetCastTime() || (IsChanneledSpell(pSpellInfo) && GetSpellDuration(pSpellInfo) > 0))
        aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, target->GetObjectGuid(), time(0));

    aiObjectContext->GetValue<ai::PositionMap&>("position")->Get()["random"].Reset();

    if (oldSel)
        bot->SetSelectionGuid(oldSel);

    if (HasStrategy("debug spell", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting " <<ChatHelper::formatSpell(pSpellInfo);
        TellPlayerNoFacing(GetMaster() ? GetMaster() : bot, out);
    }

    return true;
}

bool PlayerbotAI::CastSpell(uint32 spellId, GameObject* goTarget, Item* itemTarget, bool waitForSpell, uint32* outSpellDuration)
{
    if (!spellId)
        return false;

    aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(NULL);
    aiObjectContext->GetValue<time_t>("stay time")->Set(0);

    MotionMaster& mm = *bot->GetMotionMaster();

    if (bot->IsFlying() || bot->IsTaxiFlying())
        return false;

    //bot->clearUnitState(UNIT_STAT_CHASE);
    //bot->clearUnitState(UNIT_STAT_FOLLOW);

    bool failWithDelay = false;
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        failWithDelay = true;
    }

    WorldObject* faceTo = goTarget;
    if (!sServerFacade.IsInFront(bot, faceTo, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
    {
        sServerFacade.SetFacingTo(bot, faceTo);
        //failWithDelay = true;
    }

    if (failWithDelay)
    {
        if (waitForSpell)
        {
            SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        }

        if (outSpellDuration)
        {
            *outSpellDuration = sPlayerbotAIConfig.globalCoolDown;
        }

        return false;
    }

    const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
    Spell* spell = new Spell(bot, pSpellInfo, false);

    SpellCastTargets targets;
    if (pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        WorldLocation aoe = aiObjectContext->GetValue<WorldLocation>("aoe position")->Get();
        if (aoe.coord_x != 0)
            targets.setDestination(aoe.coord_x, aoe.coord_y, aoe.coord_z);
        else if (goTarget && goTarget->GetObjectGuid() != bot->GetObjectGuid())
            targets.setDestination(goTarget->GetPositionX(), goTarget->GetPositionY(), goTarget->GetPositionZ());
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.setDestination(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    }

    targets.setGOTarget(goTarget);
    spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get());
    targets.setItemTarget(spell->GetCastItem());

    if (goTarget->GetGoType() == GAMEOBJECT_TYPE_CHEST)
    {
        auto context = GetAiObjectContext();
        AI_VALUE(LootObjectStack*, "available loot")->Add(goTarget->GetObjectGuid());
    }

    if (spellId == 1953) // simulate blink coordinates
    {
        float angle = bot->GetOrientation();
        float distance = 20.0f;
        float fx = bot->GetPositionX() + cos(angle) * distance;
        float fy = bot->GetPositionY() + sin(angle) * distance;
        float fz = bot->GetPositionZ();

        float ox, oy, oz;
        bot->GetPosition(ox, oy, oz);
        //#ifdef MANGOSBOT_TWO
        //        bot->GetMap()->GetHitPosition(ox, oy, oz + max_height, fx, fy, fz, bot->GetPhaseMask(), -0.5f);
        //#else
        //        bot->GetMap()->GetHitPosition(ox, oy, oz + 2.0f, fx, fy, fz, -0.5f);
        //#endif
        bot->UpdateAllowedPositionZ(fx, fy, fz);
        targets.setDestination(fx, fy, fz);
    }

    // Fail the cast if the bot is moving and the spell is a casting/channeled spell
    if (sServerFacade.isMoving(bot) && ((GetSpellCastTime(pSpellInfo, bot, spell) > 0) || (IsChanneledSpell(pSpellInfo) && (GetSpellDuration(pSpellInfo) > 0))))
    {
        // always fail when jumping
        if (IsJumping() || bot->IsFalling())
        {
            spell->cancel();
            return false;
        }

        StopMoving();

        // fail if not with real player to avoid movement glitches
        if (!HasActivePlayerMaster())
        {
            if (waitForSpell)
            {
                SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
            }

            spell->cancel();
            return false;
        }
    }

    SpellCastResult spellSuccess = spell->SpellStart(&targets);
    if (spellSuccess != SPELL_CAST_OK)
        return false;

    PlayAttackEmote(6);

    if (waitForSpell)
    {
        WaitForSpellCast(spell);
    }

    if (outSpellDuration)
    {
        *outSpellDuration = GetSpellCastDuration(spell);
    }

    if (spell->GetCastTime() || (IsChanneledSpell(pSpellInfo) && GetSpellDuration(pSpellInfo) > 0))
        aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, goTarget->GetObjectGuid(), time(0));

    aiObjectContext->GetValue<ai::PositionMap&>("position")->Get()["random"].Reset();

    if (HasStrategy("debug spell", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting " << ChatHelper::formatSpell(pSpellInfo);
        TellPlayerNoFacing(GetMaster() ? GetMaster() : bot, out);
    }

    return true;
}

bool PlayerbotAI::CastSpell(uint32 spellId, float x, float y, float z, Item* itemTarget, bool waitForSpell, uint32* outSpellDuration)
{
    if (!spellId)
        return false;

    Pet* pet = bot->GetPet();
    if (pet && pet->HasSpell(spellId))
    {
        return CastPetSpell(spellId, nullptr);
    }

    aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(NULL);
    aiObjectContext->GetValue<time_t>("stay time")->Set(0);

    MotionMaster& mm = *bot->GetMotionMaster();

    if (bot->IsFlying() || bot->IsTaxiFlying())
        return false;

    bot->clearUnitState(UNIT_STAT_CHASE);
    bot->clearUnitState(UNIT_STAT_FOLLOW);

    bool failWithDelay = false;
    if (!bot->IsStandState())
    {
        bot->SetStandState(UNIT_STAND_STATE_STAND);
        failWithDelay = true;
    }

    ObjectGuid oldSel = bot->GetSelectionGuid();

    if (!sServerFacade.isMoving(bot)) sServerFacade.SetFacingTo(bot, bot->GetAngleAt(bot->GetPositionX(), bot->GetPositionY(), x, y));

    if (failWithDelay)
    {
        if (waitForSpell)
        {
            SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        }

        if (outSpellDuration)
        {
            *outSpellDuration = sPlayerbotAIConfig.globalCoolDown;
        }

        return false;
    }

    const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
    Spell* spell = new Spell(bot, pSpellInfo, false);

    SpellCastTargets targets;
    if ((pSpellInfo->Targets & TARGET_FLAG_ITEM) || spellId == 1804)
    {
        spell->SetCastItem(itemTarget ? itemTarget : aiObjectContext->GetValue<Item*>("item for spell", spellId)->Get());
        targets.setItemTarget(spell->GetCastItem());

        if (bot->GetTradeData())
        {
            bot->GetTradeData()->SetSpell(spellId);
            delete spell;
            return true;
        }
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        WorldLocation aoe = aiObjectContext->GetValue<WorldLocation>("aoe position")->Get();
        targets.setDestination(x, y, z);
    }
    else if (pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.setDestination(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    }
    else
    {
        return false;
    }

    if (pSpellInfo->Effect[0] == SPELL_EFFECT_OPEN_LOCK ||
        pSpellInfo->Effect[0] == SPELL_EFFECT_SKINNING)
    {
        return false;
    }

    // Fail the cast if the bot is moving and the spell is a casting/channeled spell
    if (sServerFacade.isMoving(bot) && ((GetSpellCastTime(pSpellInfo, bot, spell) > 0) || (IsChanneledSpell(pSpellInfo) && (GetSpellDuration(pSpellInfo) > 0))))
    {
        // always fail when jumping
        if (IsJumping() || bot->IsFalling())
        {
            spell->cancel();
            return false;
        }

        StopMoving();

        // fail if not with real player to avoid movement glitches
        if (!HasActivePlayerMaster())
        {
            if (waitForSpell)
            {
                SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
            }

            spell->cancel();
            return false;
        }
    }

    spell->SpellStart(&targets);

    if (pSpellInfo->Effect[0] == SPELL_EFFECT_OPEN_LOCK ||
        pSpellInfo->Effect[0] == SPELL_EFFECT_SKINNING)
    {
        if (!spell->m_targets.getItemTarget())
        {
            LootObject loot = *aiObjectContext->GetValue<LootObject>("loot target");
            if (!loot.IsLootPossible(bot))
            {
                spell->cancel();
                //delete spell;
                return false;
            }
        }
    }

    if (waitForSpell)
    {
        WaitForSpellCast(spell);
    }

    if (outSpellDuration)
    {
        *outSpellDuration = GetSpellCastDuration(spell);
    }

    aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, bot->GetObjectGuid(), time(0));
    aiObjectContext->GetValue<ai::PositionMap&>("position")->Get()["random"].Reset();

    if (oldSel)
        bot->SetSelectionGuid(oldSel);

    if (HasStrategy("debug spell", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting " << ChatHelper::formatSpell(pSpellInfo);
        TellPlayerNoFacing(GetMaster() ? GetMaster() : bot, out);
    }

    return true;
}

bool PlayerbotAI::CastPetSpell(uint32 spellId, Unit* target)
{
    Pet* pet = bot->GetPet();
    if (pet && spellId && pet->HasSpell(spellId))
    {
        auto IsAutocastActive = [&pet, &spellId]() -> bool
        {
            for (AutoSpellList::iterator i = pet->m_autospells.begin(); i != pet->m_autospells.end(); ++i)
            {
                if (*i == spellId)
                {
                    return true;
                }
            }

            return false;
        };

        // Send pet spell action packet
        uint8 flag = ACT_PASSIVE;
        if (IsAutocastable(spellId))
        {
            flag = IsAutocastActive() ? ACT_ENABLED : ACT_DISABLED;
        }

        uint32 command = (flag << 24) | spellId;

        WorldPacket data(CMSG_PET_ACTION);
        data << pet->GetObjectGuid();
        data << command;
        data << (target ? target->GetObjectGuid() : ObjectGuid());
        bot->GetSession()->HandlePetAction(data);
        return true;
    }

    return false;
}

bool PlayerbotAI::CanCastVehicleSpell(uint32 spellId, Unit* target)
{
#ifdef MANGOSBOT_TWO
    if (!spellId)
        return false;

    TransportInfo* transportInfo = bot->GetTransportInfo();
    if (!transportInfo || !transportInfo->IsOnVehicle())
        return false;

    Unit* vehicle = (Unit*)transportInfo->GetTransport();

    // do not allow if no spells
    VehicleSeatEntry const* seat = transportInfo ? vehicle->GetVehicleInfo()->GetSeatEntry(transportInfo->GetTransportSeat()) : nullptr;
    if (seat && !seat->HasFlag(SEAT_FLAG_CAN_CAST))
        return false;

    bool canControl = seat ? (seat->HasFlag(SEAT_FLAG_CAN_CONTROL)) : false;

    if (!vehicle)
        return false;

    Unit* spellTarget = target;

    if (!spellTarget)
        spellTarget = vehicle;

    if (!spellTarget)
        return false;

#ifdef MANGOS
    if (vehicle->HasSpellCooldown(spellId))
        return false;
#endif
#ifdef CMANGOS
    if (!vehicle->IsSpellReady(spellId))
        return false;
#endif

    SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(spellId);
    if (!spellInfo)
        return false;

    // check BG siege position set in BG Tactics
    PositionEntry siegePos = GetAiObjectContext()->GetValue<ai::PositionMap&>("position")->Get()["bg siege"];

    // do not cast spell on self if spell is location based
    if (!(siegePos.isSet() || spellTarget != vehicle) && spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        return false;

    uint32 CastingTime = !IsChanneledSpell(spellInfo) ? GetSpellCastTime(spellInfo, vehicle) : GetSpellDuration(spellInfo);

    if (CastingTime && vehicle->IsMoving())
        return false;

    if (vehicle != spellTarget && sServerFacade.GetDistance2d(vehicle, spellTarget) > 120.0f)
        return false;

    if (!target && siegePos.isSet())
    {
        if (sServerFacade.GetDistance2d(vehicle, siegePos.x, siegePos.y) > 120.0f)
            return false;
    }

    Spell* spell = new Spell(vehicle, spellInfo, false);

    WorldLocation dest;
    if (siegePos.isSet())
        dest = WorldLocation(bot->GetMapId(), siegePos.x, siegePos.y, siegePos.z, 0);
    else if (spellTarget != vehicle)
        dest = WorldLocation(spellTarget->GetMapId(), spellTarget->GetPosition());

    if (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        spell->m_targets.setDestination(dest.coord_x, dest.coord_y, dest.coord_z);
    else if (spellTarget != vehicle)
    {
        spell->m_targets.setUnitTarget(spellTarget);
    }

    SpellCastResult result = spell->CheckCast(true);
    delete spell;

    switch (result)
    {
    case SPELL_FAILED_NOT_INFRONT:
    case SPELL_FAILED_NOT_STANDING:
    case SPELL_FAILED_UNIT_NOT_INFRONT:
    case SPELL_FAILED_MOVING:
    case SPELL_FAILED_TRY_AGAIN:
    case SPELL_CAST_OK:
        return true;
    default:
        return false;
    }
#endif
    return false;
}

bool PlayerbotAI::CastVehicleSpell(uint32 spellId, Unit* target, float projectileSpeed, bool needTurn)
{
#ifdef MANGOSBOT_TWO
    if (!spellId)
        return false;

    TransportInfo* transportInfo = bot->GetTransportInfo();
    if (!transportInfo || !transportInfo->IsOnVehicle())
        return false;

    Unit* vehicle = (Unit*)transportInfo->GetTransport();

    // do not allow if no spells
    VehicleSeatEntry const* seat = transportInfo ? vehicle->GetVehicleInfo()->GetSeatEntry(transportInfo->GetTransportSeat()) : nullptr;
    if (!seat->HasFlag(SEAT_FLAG_CAN_CAST))
        return false;

    bool canControl = seat ? (seat->HasFlag(SEAT_FLAG_CAN_CONTROL)) : false;
    bool canTurn = seat ? (seat->HasFlag(SEAT_FLAG_ALLOW_TURNING)) : false;

    if (!vehicle)
        return false;

    Unit* spellTarget = target;

    if (!spellTarget)
        spellTarget = vehicle;

    if (!spellTarget)
        return false;

    SpellEntry const* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);

    // check BG siege position set in BG Tactics
    PositionEntry siegePos = GetAiObjectContext()->GetValue<ai::PositionMap&>("position")->Get()["bg siege"];
    if (!target && siegePos.isSet())
    {
        if (sServerFacade.GetDistance2d(vehicle, siegePos.x, siegePos.y) > 120.0f)
            return false;
    }

    // do not cast spell on self if spell is location based
    if (!(siegePos.isSet() || spellTarget != vehicle) && pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
        return false;

    if (canControl)
    {
        //aiObjectContext->GetValue<LastMovement&>("last movement")->Get().Set(NULL);
        //aiObjectContext->GetValue<time_t>("stay time")->Set(0);
    }

    MotionMaster& mm = *vehicle->GetMotionMaster();

    //bot->clearUnitState(UNIT_STAT_CHASE);
    //bot->clearUnitState(UNIT_STAT_FOLLOW);

    //ObjectGuid oldSel = bot->GetSelectionGuid();
    //bot->SetSelectionGuid(target->GetObjectGuid());

    // turn vehicle if target is not in front
    bool failWithDelay = false;
    if (spellTarget != vehicle && (canControl || canTurn) && needTurn)
    {
        if (!sServerFacade.IsInFront(vehicle, spellTarget, 100.0f, CAST_ANGLE_IN_FRONT))
        {
            vehicle->SetFacingToObject(spellTarget);
            failWithDelay = true;
        }
    }

    if (siegePos.isSet() && (canControl || canTurn))
    {
        vehicle->SetFacingTo(vehicle->GetAngle(siegePos.x, siegePos.y));
    }

    if (failWithDelay)
    {
        SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        return false;
    }

    Spell* spell = new Spell(vehicle, pSpellInfo, false);

    SpellCastTargets targets;
    if ((spellTarget != vehicle || siegePos.isSet()) && pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
    {
        WorldLocation dest;
        if (spellTarget != vehicle)
            dest = WorldLocation(spellTarget->GetMapId(), spellTarget->GetPosition());
        else if (siegePos.isSet())
            dest = WorldLocation(bot->GetMapId(), siegePos.x + frand(-5.0f, 5.0f), siegePos.y + frand(-5.0f, 5.0f), siegePos.z, 0.0f);
        else
            return false;

        targets.setDestination(dest.coord_x, dest.coord_y, dest.coord_z);
        targets.setSpeed(projectileSpeed);
        float distanceToDest = sqrt(vehicle->GetPosition().GetDistance(Position(dest.coord_x, dest.coord_y, dest.coord_z, 0.0f)));
        float elev = 0.01f;
        if (distanceToDest < 25.0f)
            elev = 0.04f;
        else if (distanceToDest < 55.0f)
            elev = 0.22f;
        else if (distanceToDest < 85.0f)
            elev = 0.42f;
        else if (distanceToDest < 95.0f)
            elev = 0.70f;
        else if (distanceToDest < 110.0f)
            elev = 0.88f;
        else
            elev = 1.0f;

        targets.setElevation(elev);
    }
    if (pSpellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
    {
        targets.setSource(vehicle->GetPositionX(), vehicle->GetPositionY(), vehicle->GetPositionZ());
    }

    if (target && !(pSpellInfo->Targets & TARGET_FLAG_DEST_LOCATION))
    {
        targets.setUnitTarget(spellTarget);
    }

#ifdef MANGOS
    spell->prepare(&targets);
#endif
#ifdef CMANGOS
    spell->SpellStart(&targets);
#endif

    if (canControl && !vehicle->IsStopped() && spell->GetCastTime())
    {
        vehicle->StopMoving();
        SetAIInternalUpdateDelay(sPlayerbotAIConfig.globalCoolDown);
        spell->cancel();
        //delete spell;
        return false;
    }

    WaitForSpellCast(spell);

    //aiObjectContext->GetValue<LastSpellCast&>("last spell cast")->Get().Set(spellId, target->GetObjectGuid(), time(0));
    //aiObjectContext->GetValue<ai::PositionMap&>("position")->Get()["random"].Reset();

    if (HasStrategy("debug spell", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Casting Vehicle Spell" << ChatHelper::formatSpell(pSpellInfo);
        TellPlayerNoFacing(GetMaster() ? GetMaster() : bot, out);
    }

    return true;
#endif
    return false;
}

bool PlayerbotAI::IsInVehicle(bool canControl, bool canCast, bool canAttack, bool canTurn, bool fixed, std::string vehicleName)
{
#ifdef MANGOSBOT_TWO
    TransportInfo* transportInfo = bot->GetTransportInfo();
    if (!transportInfo || !transportInfo->GetTransport() || !transportInfo->IsOnVehicle())
        return false;

    // get vehicle
    Unit* vehicle = (Unit*)transportInfo->GetTransport();
    if (!vehicle || !vehicle->IsAlive())
        return false;

    if (!vehicleName.empty())
    {
        std::wstring wnamepart;

        if (!Utf8toWStr(vehicleName, wnamepart))
            return 0;

        wstrToLower(wnamepart);
        char firstSymbol = tolower(vehicleName[0]);
        int spellLength = wnamepart.length();

        const char* name = vehicle->GetName();
        if (tolower(name[0]) != firstSymbol || strlen(name) != spellLength || !Utf8FitTo(name, wnamepart))
            return false;
    }

    if (!vehicle->GetVehicleInfo())
        return false;

    // get seat
    VehicleSeatEntry const* seat = vehicle->GetVehicleInfo()->GetSeatEntry(transportInfo->GetTransportSeat());
    if (!seat)
        return false;

    if (!(canControl || canCast || canAttack || canTurn || fixed))
        return true;

    if (canControl)
        return seat->HasFlag(SEAT_FLAG_CAN_CONTROL) && (vehicle->GetVehicleInfo()->GetVehicleEntry()->m_flags & VEHICLE_FLAG_FIXED_POSITION) == 0;

    if (canCast)
        return seat->HasFlag(SEAT_FLAG_CAN_CAST);

    if (canAttack)
        return seat->HasFlag(SEAT_FLAG_CAN_ATTACK);

    if (canTurn)
        return seat->HasFlag(SEAT_FLAG_ALLOW_TURNING);

    if (fixed)
        return vehicle->GetVehicleInfo()->GetVehicleEntry()->m_flags & VEHICLE_FLAG_FIXED_POSITION;

#endif
    return false;
}

void PlayerbotAI::WaitForSpellCast(Spell *spell)
{
    SetAIInternalUpdateDelay(GetSpellCastDuration(spell));
}

void PlayerbotAI::InterruptSpell(bool withMeleeAndAuto)
{
    for (int type = CURRENT_MELEE_SPELL; type < CURRENT_CHANNELED_SPELL; type++)
    {
        if (!withMeleeAndAuto)
        {
            if (type == CURRENT_MELEE_SPELL || type == CURRENT_AUTOREPEAT_SPELL)
                continue;
        }
        Spell* currentSpell = bot->GetCurrentSpell((CurrentSpellTypes)type);
        if (currentSpell && currentSpell->CanBeInterrupted())
        {
            bot->InterruptSpell((CurrentSpellTypes)type);
            SpellInterrupted(currentSpell->m_spellInfo->Id);
        }
    }
}

bool PlayerbotAI::RemoveAura(const std::string& name)
{
    const Aura* aura = GetAura(name, bot);
    if (aura)
    {
        const uint32 spellId = aura->GetSpellProto()->Id;
        if (spellId > 0)
        {
            bot->RemoveAurasDueToSpell(spellId);
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::IsInterruptableSpellCasting(Unit* target, std::string spell, uint8 effectMask)
{
	uint32 spellid = aiObjectContext->GetValue<uint32>("spell id", spell)->Get();
	if (!spellid || !target->IsNonMeleeSpellCasted(true))
		return false;

	SpellEntry const *spellInfo = sServerFacade.LookupSpellInfo(spellid);
	if (!spellInfo)
		return false;

	for (int32 i = EFFECT_INDEX_0; i <= EFFECT_INDEX_2; i++)
	{
		if ((spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_COMBAT) && spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
			return true;

		if ((spellInfo->Effect[i] == SPELL_EFFECT_INTERRUPT_CAST) &&
			!target->IsImmuneToSpellEffect(spellInfo, (SpellEffectIndex)i, true))
			return true;

        if ((spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA) && spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MOD_SILENCE)
            return true;
	}

	return false;
}

bool PlayerbotAI::HasAuraToDispel(Unit* target, uint32 dispelType)
{
    bool isFriend = sServerFacade.IsFriendlyTo(bot, target);
	for (uint32 type = SPELL_AURA_NONE; type < TOTAL_AURAS; ++type)
	{
		Unit::AuraList const& auras = target->GetAurasByType((AuraType)type);
		for (Unit::AuraList::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
		{
			const Aura* aura = *itr;
			const SpellEntry* entry = aura->GetSpellProto();
			uint32 spellId = entry->Id;

			bool isPositiveSpell = IsPositiveSpell(spellId);
			if (isPositiveSpell && isFriend)
				continue;

			if (!isPositiveSpell && !isFriend)
				continue;

			if (sPlayerbotAIConfig.dispelAuraDuration && aura->GetAuraDuration() && aura->GetAuraDuration() < (int32)sPlayerbotAIConfig.dispelAuraDuration)
			    return false;

			if (canDispel(entry, dispelType))
				return true;
		}
	}
	return false;
}

#ifndef WIN32
inline int strcmpi(const char* s1, const char* s2)
{
    for (; *s1 && *s2 && (toupper(*s1) == toupper(*s2)); ++s1, ++s2);
    return *s1 - *s2;
}
#endif

bool PlayerbotAI::canDispel(const SpellEntry* entry, uint32 dispelType)
{
    if (entry->Dispel != dispelType)
        return false;

    return entry->SpellName[0].empty() ||
        (strcmpi(entry->SpellName[0].c_str(), "demon skin") &&
        strcmpi(entry->SpellName[0].c_str(), "mage armor") &&
        strcmpi(entry->SpellName[0].c_str(), "frost armor") &&
        strcmpi(entry->SpellName[0].c_str(), "wavering will") &&
        strcmpi(entry->SpellName[0].c_str(), "chilled") &&
        strcmpi(entry->SpellName[0].c_str(), "mana tap") &&
        strcmpi(entry->SpellName[0].c_str(), "ice armor"));
}

bool PlayerbotAI::IsHealSpell(const SpellEntry* spell)
{
    // Holy Light/Flash of Light
    if (spell->SpellFamilyName == SPELLFAMILY_PALADIN)
    {
        if (spell->SpellIconID == 70 ||
            spell->SpellIconID == 242)
            return true;
    }

    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch (spell->Effect[i])
        {
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
                return true;
            case SPELL_EFFECT_APPLY_AURA:
            case SPELL_EFFECT_APPLY_AREA_AURA_FRIEND:
            case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
            case SPELL_EFFECT_APPLY_AREA_AURA_PET:
            {
                switch (spell->EffectApplyAuraName[i])
                {
                    case SPELL_AURA_PERIODIC_HEAL:
                        return true;
                }
                break;
            }
        }
    }
    return false;
}

bool PlayerbotAI::IsMiningNode(const GameObject* go)
{
    if (go)
    {
        const GameObjectInfo* goInfo = go->GetGOInfo();
        if (goInfo)
        {
            const LockEntry* lockInfo = sLockStore.LookupEntry(goInfo->GetLockId());
            if (lockInfo)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (lockInfo->Type[i] == LOCK_KEY_SKILL)
                    {
                        const uint32 skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                        if (skillId == SKILL_MINING)
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool PlayerbotAI::IsHerb(const GameObject* go)
{
    if (go)
    {
        const GameObjectInfo* goInfo = go->GetGOInfo();
        if (goInfo)
        {
            const LockEntry* lockInfo = sLockStore.LookupEntry(goInfo->GetLockId());
            if (lockInfo)
            {
                for (int i = 0; i < 8; ++i)
                {
                    if (lockInfo->Type[i] == LOCK_KEY_SKILL)
                    {
                        const uint32 skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                        if (skillId == SKILL_HERBALISM)
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool PlayerbotAI::HasSpellItems(uint32 spellId, const Item* castItem) const
{
    const SpellEntry* spellEntry = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
    if (spellEntry)
    {
        if (HasCheat(BotCheatMask::item))
        {
            return true;
        }
        else
        {
            if (!bot->CanNoReagentCast(spellEntry))
            {
                for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                {
                    if (spellEntry->Reagent[i] <= 0)
                    {
                        continue;
                    }

                    uint32 itemid = spellEntry->Reagent[i];
                    uint32 itemcount = spellEntry->ReagentCount[i];

                    // if CastItem is also spell reagent
                    if (castItem && castItem->GetEntry() == itemid)
                    {
                        const ItemPrototype* proto = castItem->GetProto();
                        if (!proto)
                        {
                            return false;
                        }

                        for (uint8 s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                        {
                            // CastItem will be used up and does not count as reagent
                            int32 charges = castItem->GetSpellCharges(s);
                            if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                            {
                                ++itemcount;
                                break;
                            }
                        }
                    }

                    if (!bot->HasItemCount(itemid, itemcount))
                    {
                        return false;
                    }
                }
            }

            // check totem-item requirements (items presence in inventory)
            uint32 totems = MAX_SPELL_TOTEMS;
            for (auto i : spellEntry->Totem)
            {
                if (i != 0)
                {
                    if (bot->HasItemCount(i, 1))
                    {
                        totems -= 1;
                    }
                }
                else
                {
                    totems -= 1;
                }
            }

            if (totems != 0)
            {
                return false;
            }
        }
    }

    return false;
}

void PlayerbotAI::DurabilityLoss(Item* item, double percent)
{
    if (item)
    {
        const uint32 pCurrDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        const uint32 pMaxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (pMaxDurability)
        {
            if (!HasCheat(BotCheatMask::repair))
            {
                // Break item
                uint32 pDurabilityLoss = std::max(uint32(pMaxDurability * percent), 1U);
                bot->DurabilityPointsLoss(item, pDurabilityLoss);
            }
            else if (pCurrDurability < pMaxDurability)
            {
                // Repair if broken
#ifdef MANGOSBOT_ZERO
                bot->DurabilityRepair(item->GetPos(), false, 0.0f);
#else
                bot->DurabilityRepair(item->GetPos(), false, 0.0f, false);
#endif
            }
        }
    }
}

bool IsAlliance(uint8 race)
{
    return race == RACE_HUMAN || race == RACE_DWARF || race == RACE_NIGHTELF ||
#ifndef MANGOSBOT_ZERO
           race == RACE_DRAENEI ||
#endif
           race == RACE_GNOME;
}

uint32 PlayerbotAI::GetFixedBotNumber(BotTypeNumber typeNumber, uint32 maxNum, float cyclePerMin, bool ignoreGuid)
{
    uint8 seedNumber = uint8(typeNumber);
    std::mt19937 rng(seedNumber);
    uint32 randseed = rng();                                       //Seed random number
    uint32 randnum = randseed;                                     //Semi-random.
    
    if (!ignoreGuid)
        randnum += bot->GetGUIDLow();                              //but fixed number for each bot.

    if (cyclePerMin > 0)
    {
        uint32 cycle = floor(WorldTimer::getMSTime() / (1000));    //Semi-random number adds 1 each second.
        cycle = cycle * cyclePerMin / 60;                          //Cycles cyclePerMin per minute.
        randnum += cycle;                                          //Make the random number cylce.
    }
    randnum = (randnum % (maxNum+1));                              //Loops the randomnumber at maxNum. Bassically removes all the numbers above 99.
    return randnum;                                                //Now we have a number unique for each bot between 0 and maxNum that increases by cyclePerMin.
}

GrouperType PlayerbotAI::GetGrouperType()
{
    AiObjectContext* context = GetAiObjectContext();
    int32 grouperOverride = AI_VALUE2(int32, "manual saved int", "grouper override");

    if (grouperOverride >= 0)
        return GrouperType(grouperOverride);

    uint32 maxGroupType = sPlayerbotAIConfig.randomBotRaidNearby ? 100 : 95;
    uint32 grouperNumber = GetFixedBotNumber(BotTypeNumber::GROUPER_TYPE_NUMBER, maxGroupType, 0);

    //20% solo
    //50% member
    //20% leader
    //10% raider

    if (grouperNumber < 20 && !HasRealPlayerMaster())
        return GrouperType::SOLO;
    if (grouperNumber < 75)
        return GrouperType::MEMBER;
    if (grouperNumber < 80 || bot->GetLevel() < 3)
        return GrouperType::LEADER_2;
    if (grouperNumber < 85 || bot->GetLevel() < 5)
        return GrouperType::LEADER_3;
    if (grouperNumber < 90 || bot->GetLevel() < 7)
        return GrouperType::LEADER_4;
    if (grouperNumber <= 95 || bot->GetLevel() < 9)
        return GrouperType::LEADER_5;
#ifdef MANGOSBOT_ZERO
    if (grouperNumber <= 97)
        return GrouperType::RAIDER_20;
#else
    if (grouperNumber <= 97)
        return GrouperType::RAIDER_10;
#endif
   return GrouperType::RAIDER_MAX;
}

GuilderType PlayerbotAI::GetGuilderType()
{
    AiObjectContext* context = GetAiObjectContext();
    int32 guilderOverride = AI_VALUE2(int32, "manual saved int", "guilder override");

    if (guilderOverride >= 0)
        return GuilderType(guilderOverride);

    uint32 grouperNumber = GetFixedBotNumber(BotTypeNumber::GUILDER_TYPE_NUMBER, 100, 0);

    if (grouperNumber < 20 && !HasRealPlayerMaster())
        return GuilderType::SOLO;
    if (grouperNumber < 30)
        return GuilderType::TINY;
    if (grouperNumber < 40)
        return GuilderType::SMALL;
    if (grouperNumber < 60)
        return GuilderType::MEDIUM;
    if (grouperNumber < 80)
        return GuilderType::LARGE;

    return GuilderType::MASSIVE;
}

uint32 PlayerbotAI::GetMaxPreferedGuildSize()
{
    uint32 maxSize = (uint8)GetGuilderType();

    if (!bot->GetGuildId()) return maxSize;

    Guild* guild =sGuildMgr.GetGuildById(bot->GetGuildId());

    uint32 maxRank = guild->GetRanksSize();

    MemberSlot* botMember = guild->GetMemberSlot(bot->GetObjectGuid());

    if (!botMember->RankId) return maxSize;

    uint32 memberMod = (botMember->RankId * 20) / maxRank;

    return (maxSize * (100 - memberMod)) / 100;
}

bool PlayerbotAI::HasPlayerNearby(WorldPosition pos, float range)
{
    if (!range)
        range = pos.getVisibilityDistance();

    float sqRange = range * range;
    bool nearPlayer = false;
    for (auto& i : sRandomPlayerbotMgr.GetPlayers())
    {
        Player* player = i.second;
        if (!player->IsGameMaster() || player->isGMVisible())
        {
            if (player->GetMapId() != bot->GetMapId())
                continue;

            if (pos.sqDistance(WorldPosition(player)) < sqRange)
                nearPlayer = true;

            // if player is far check farsight/cinematic camera
            Camera& viewPoint = player->GetCamera();
            WorldObject* viewObj = viewPoint.GetBody();
            if (viewObj && viewObj != player)
            {
                if (pos.sqDistance(WorldPosition(viewObj)) < sqRange)
                    nearPlayer = true;
            }
        }
    }

    return nearPlayer;
}

bool PlayerbotAI::HasPlayerNearby(float range)
{
    return HasPlayerNearby(bot, range);
}

bool PlayerbotAI::HasManyPlayersNearby(uint32 trigerrValue, float range)
{
    float sqRange = range * range;
    uint32 found = 0;

    for (auto& i : sRandomPlayerbotMgr.GetPlayers())
    {
        Player* player = i.second;
        if ((!player->IsGameMaster() || player->isGMVisible()) && sServerFacade.GetDistance2d(player, bot) < sqRange)
        {
            found++;

            if (found >= trigerrValue)
                return true;
        }
    }
    return false;
}

bool PlayerbotAI::ChannelHasRealPlayer(std::string channelName)
{
    if (ChannelMgr* cMgr = channelMgr(bot->GetTeam()))
    {

        if (Channel* chn = cMgr->GetChannel(channelName, bot))
        {
            ChannelAcces* chna = reinterpret_cast<ChannelAcces*>(chn);

            for (auto& player : sRandomPlayerbotMgr.GetPlayers())
                if (chna->IsOn(player.second->GetObjectGuid()))
                    return true;
        }
    }

    return false;
}

/*
enum ActivityType
{
    GRIND_ACTIVITY = 1,
    RPG_ACTIVITY = 2,
    TRAVEL_ACTIVITY = 3,
    OUT_OF_PARTY_ACTIVITY = 4,
    PACKET_ACTIVITY = 5,
    DETAILED_MOVE_ACTIVITY = 6,
    PARTY_ACTIVITY = 7
    ALL_ACTIVITY = 8
};

   General function to check if a bot is allowed to be active or not.
   This function should be checked first before doing heavy-workload.


*/

ActivePiorityType PlayerbotAI::GetPriorityType()
{
    //First priority - priorities disabled or has player master. Always active.
    if (sPlayerbotAIConfig.disableActivityPriorities || HasRealPlayerMaster())
        return ActivePiorityType::HAS_REAL_PLAYER_MASTER;

    //Self bot in a group with a bot master.
    if (IsRealPlayer())
        return ActivePiorityType::IS_REAL_PLAYER;

    Group* group = bot->GetGroup();
    if (group)
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->getSource();

            if (!member || !member->IsInWorld())
                continue;

            if (member == bot)
                continue;

            if (!GetBotAI(member) || (GetBotAI(member) && GetBotAI(member)->HasRealPlayerMaster()))
                return ActivePiorityType::IN_GROUP_WITH_REAL_PLAYER;
        }
    }

    if (bot->IsBeingTeleported()) //We might end up in a bg so stay active.
        return ActivePiorityType::IN_BATTLEGROUND;

    if (WorldPosition(bot).isBg())
        return ActivePiorityType::IN_BATTLEGROUND;

    AiObjectContext* context = GetAiObjectContext();

    if (AI_VALUE2(bool, "manual bool", "is running test"))
        return ActivePiorityType::IS_RUNNING_TEST;

    if (!WorldPosition(bot).isOverworld())
    {
        if (!sPlayerbotAIConfig.enableMinimalMove)
            return ActivePiorityType::IN_INSTANCE;
        else
        {
            LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
            if (lastMove.lastPath.empty())
                return ActivePiorityType::IN_INSTANCE;
        }
    }

    if (HasPlayerNearby())
        return ActivePiorityType::VISIBLE_FOR_PLAYER;

    if (sPlayerbotAIConfig.guildOrderAlwaysActive && bot->IsInWorld() && bot->GetGuildId())
    {
        AiObjectContext* context = GetAiObjectContext();
        GuildOrder guildOrder = AI_VALUE(GuildOrder, "guild order");
        if (guildOrder.IsValid())
            return ActivePiorityType::IS_ALWAYS_ACTIVE;
    }

    if (sServerFacade.IsInCombat(bot))
        return ActivePiorityType::IN_COMBAT;

    if (HasPlayerNearby(WorldPosition(bot).getVisibilityDistance() + sPlayerbotAIConfig.reactDistance))
        return ActivePiorityType::NEARBY_PLAYER;

    if (sPlayerbotAIConfig.IsFreeAltBot(bot) || HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
        return ActivePiorityType::IS_ALWAYS_ACTIVE;

    if (bot->InBattleGroundQueue())
        return ActivePiorityType::IN_BG_QUEUE;

    bool isLFG = false;
#ifdef MANGOSBOT_TWO
    /*if (group)
    {
    if (sLFGMgr.GetQueueInfo(group->GetObjectGuid()))
    {
    isLFG = true;
    }
    }
    if (sLFGMgr.GetQueueInfo(bot->GetObjectGuid()))
    {
    isLFG = true;
    }*/
#endif

    if (isLFG)
        return ActivePiorityType::IN_LFG;

    if (sPlayerbotAIConfig.enableMinimalMove)
    {
        AiObjectContext* context = GetAiObjectContext();
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (lastMove.lastPath.empty() && !urand(0, 5))
            return ActivePiorityType::NO_PATH;
    }

    //If has real players - slow down continents without player
    //This means we first disable bots in a different continent/area.
    if (sRandomPlayerbotMgr.GetPlayers().empty())
        return ActivePiorityType::IN_EMPTY_SERVER;

    // friends always active
    for (auto& i : sRandomPlayerbotMgr.GetPlayers())
    {
        Player* player = i.second;
        if (!player || !player->IsInWorld())
            continue;

        if (player->GetSocial()->HasFriend(bot->GetObjectGuid()))
            return ActivePiorityType::PLAYER_FRIEND;
    }

    // real guild always active if member+
    if (IsInRealGuild())
        return ActivePiorityType::PLAYER_GUILD;

    Map* const realMap = bot->FindMap();
    if (bot->IsBeingTeleported() || !bot->IsInWorld() || !realMap ||
        !realMap->HasRealPlayers())
        return ActivePiorityType::IN_INACTIVE_MAP;

    if (!bot->GetMap()->HasActiveZone(bot->GetZoneId()))
        return ActivePiorityType::IN_ACTIVE_MAP;

    return ActivePiorityType::IN_ACTIVE_AREA;
}

//Returns the lower and upper bracket for bots to be active.
//Ie. 10,20 means all bots in this bracket will be inactive below 10% activityMod, all bots in this bracket will be active above 20% activityMod and scale between those values.
std::pair<uint32, uint32> PlayerbotAI::GetPriorityBracket(ActivePiorityType type)
{
    switch (type)
    {
    case ActivePiorityType::HAS_REAL_PLAYER_MASTER:
    case ActivePiorityType::IS_REAL_PLAYER:
    case ActivePiorityType::IN_GROUP_WITH_REAL_PLAYER:
    case ActivePiorityType::VISIBLE_FOR_PLAYER:
    case ActivePiorityType::IN_BATTLEGROUND:
    case ActivePiorityType::IS_RUNNING_TEST:
        return { 0,0 };
    case ActivePiorityType::IN_INSTANCE:
        return { 0,5 };
    case ActivePiorityType::IS_ALWAYS_ACTIVE:
        return { 0,0 };
    case ActivePiorityType::IN_COMBAT:
    {
        if (sPlayerbotAIConfig.limitCombatActivity)
            return { 99,100 };

        return { 0,10 };
    }
    case ActivePiorityType::IN_BG_QUEUE:
        return { 0,20 };
    case ActivePiorityType::IN_LFG:
        return { 0,30 };
    case ActivePiorityType::NEARBY_PLAYER:
        return { 0,40 };
    case ActivePiorityType::PLAYER_FRIEND:
    case ActivePiorityType::PLAYER_GUILD:
        return { 0,50 };
    case ActivePiorityType::NO_PATH:
        return { 50, 99};
    case ActivePiorityType::IN_ACTIVE_AREA:
    case ActivePiorityType::IN_EMPTY_SERVER:
        return { 50,100 }; //Note lower 100 means multiply by activity percentage.
    case ActivePiorityType::IN_ACTIVE_MAP:
        return { 70,100 };
    case ActivePiorityType::IN_INACTIVE_MAP:
        return { 80,100 };
    default :
        return { 90, 100 };
    }

    return { 90, 100 };
}

bool PlayerbotAI::AllowActive(ActivityType activityType)
{
    ActivePiorityType type = GetPriorityType();

    if (activityType == DETAILED_MOVE_ACTIVITY)
    {
        switch (type)
        {
        case ActivePiorityType::HAS_REAL_PLAYER_MASTER:
        case ActivePiorityType::IS_REAL_PLAYER:
        case ActivePiorityType::IN_GROUP_WITH_REAL_PLAYER:
        case ActivePiorityType::IN_INSTANCE:
        case ActivePiorityType::VISIBLE_FOR_PLAYER:
        case ActivePiorityType::IN_COMBAT:
        case ActivePiorityType::NEARBY_PLAYER:
        case ActivePiorityType::IS_RUNNING_TEST:
            return true;
            break;
        case ActivePiorityType::IS_ALWAYS_ACTIVE:
        case ActivePiorityType::IN_BG_QUEUE:
        case ActivePiorityType::IN_LFG:
        case ActivePiorityType::PLAYER_FRIEND:
        case ActivePiorityType::PLAYER_GUILD:
        case ActivePiorityType::NO_PATH:
        case ActivePiorityType::IN_ACTIVE_AREA:
        case ActivePiorityType::IN_EMPTY_SERVER:
        case ActivePiorityType::IN_ACTIVE_MAP:
        case ActivePiorityType::IN_INACTIVE_MAP:
        default:
            break;
        }
    }
    else if (activityType == REACT_ACTIVITY)
    {
        switch (type)
        {
        case ActivePiorityType::HAS_REAL_PLAYER_MASTER:
        case ActivePiorityType::IS_REAL_PLAYER:
        case ActivePiorityType::IN_GROUP_WITH_REAL_PLAYER:
        case ActivePiorityType::IN_INSTANCE:
        case ActivePiorityType::IS_ALWAYS_ACTIVE:
        case ActivePiorityType::IS_RUNNING_TEST:
        case ActivePiorityType::IN_COMBAT:
            return true;
        case ActivePiorityType::VISIBLE_FOR_PLAYER:
            if (sPlayerbotAIConfig.forceActiveWhenNearPlayer)
                return true;
            break;
        case ActivePiorityType::NEARBY_PLAYER:
        case ActivePiorityType::IN_BG_QUEUE:
        case ActivePiorityType::IN_LFG:
        case ActivePiorityType::PLAYER_FRIEND:
        case ActivePiorityType::PLAYER_GUILD:
        case ActivePiorityType::NO_PATH:
        case ActivePiorityType::IN_ACTIVE_AREA:
        case ActivePiorityType::IN_EMPTY_SERVER:
        case ActivePiorityType::IN_ACTIVE_MAP:
        case ActivePiorityType::IN_INACTIVE_MAP:
        default:
            break;
        }
    }

    std::pair<uint8, uint8> priorityBracket = GetPriorityBracket(type);

    float activityPercentage = sRandomPlayerbotMgr.getActivityPercentage(); //Activity between 0 and 100.

    if (!priorityBracket.second) //No scaling
        return true;

    if (priorityBracket.first >= activityPercentage)
        return false;
    if (priorityBracket.second <= activityPercentage && priorityBracket.second < 100)
        return true;

    float activePerc = (activityPercentage - priorityBracket.first) / (priorityBracket.second - priorityBracket.first);

    activePerc *= (priorityBracket.second == 100) ? sPlayerbotAIConfig.botActiveAlone : 100;

    uint32 ActivityNumber = GetFixedBotNumber(BotTypeNumber::ACTIVITY_TYPE_NUMBER, 100, activePerc * 0.01f); //The last number if the amount it cycles per min. Currently set to 1% of the active bots.

    return ActivityNumber <= (activePerc);           //The given percentage of bots should be active and rotate 1% of those active bots each minute.
}

bool PlayerbotAI::AllowActivity(ActivityType activityType, bool checkNow)
{
    if (!allowActiveCheckTimer[activityType])
        allowActiveCheckTimer[activityType] = time(NULL);

    if (!checkNow && time(NULL) < (allowActiveCheckTimer[activityType] + 5))
        return allowActive[activityType];

    bool allowed = AllowActive(activityType);
    allowActive[activityType] = allowed;
    allowActiveCheckTimer[activityType] = time(NULL);
    return allowed;
}

bool PlayerbotAI::HasCheat(BotCheatMask mask) const
{
    if (((uint32)mask & (uint32)cheatMask) != 0)
        return true;

    if (sRandomPlayerbotMgr.IsRandomBot(bot))
    {
        if (((uint32)mask & sPlayerbotAIConfig.rndBotCheatMask) != 0)
            return true;
    }
    else
    {
        if (((uint32)mask & sPlayerbotAIConfig.botCheatMask) != 0)
            return true;
    }

    return false;
}

bool PlayerbotAI::IsOpposing(Player* player)
{
    return IsOpposing(player->getRace(), bot->getRace());
}

bool PlayerbotAI::IsOpposing(uint8 race1, uint8 race2)
{
    return (IsAlliance(race1) && !IsAlliance(race2)) || (!IsAlliance(race1) && IsAlliance(race2));
}

void PlayerbotAI::RemoveShapeshift()
{
    RemoveAura("bear form");
    RemoveAura("dire bear form");
    RemoveAura("moonkin form");
    RemoveAura("travel form");
    RemoveAura("cat form");
    RemoveAura("flight form");
    RemoveAura("swift flight form");
    RemoveAura("aquatic form");
    RemoveAura("ghost wolf");
    RemoveAura("tree of life");
}

uint32 PlayerbotAI::GetEquipGearScore(Player* player, bool withBags, bool withBank)
{
    std::vector<uint32> gearScore(EQUIPMENT_SLOT_END);
    uint32 twoHandScore = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            _fillGearScoreData(player, item, &gearScore, twoHandScore);
    }

    if (withBags)
    {
        // check inventory
        for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore);
        }

        // check bags
        for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            if (Bag* pBag = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* item2 = pBag->GetItemByPos(j))
                        _fillGearScoreData(player, item2, &gearScore, twoHandScore);
                }
            }
        }
    }

    if (withBank)
    {
        for (uint8 i = BANK_SLOT_ITEM_START; i < BANK_SLOT_ITEM_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                _fillGearScoreData(player, item, &gearScore, twoHandScore);
        }

        for (uint8 i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (item->IsBag())
                {
                    Bag* bag = (Bag*)item;
                    for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    {
                        if (Item* item2 = bag->GetItemByPos(j))
                            _fillGearScoreData(player, item2, &gearScore, twoHandScore);
                    }
                }
            }
        }
    }

    uint8 count = EQUIPMENT_SLOT_END - 2;   // ignore body and tabard slots
    uint32 sum = 0;

    // check if 2h hand is higher level than main hand + off hand
    if (gearScore[EQUIPMENT_SLOT_MAINHAND] + gearScore[EQUIPMENT_SLOT_OFFHAND] < twoHandScore * 2)
    {
        gearScore[EQUIPMENT_SLOT_OFFHAND] = 0;  // off hand is ignored in calculations if 2h weapon has higher score
        --count;
        gearScore[EQUIPMENT_SLOT_MAINHAND] = twoHandScore;
    }

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
       sum += gearScore[i];
    }

    if (count)
    {
        uint32 res = uint32(sum / count);
        return res;
    }
    else
        return 0;
}

uint32 PlayerbotAI::GetEquipStatsValue(Player* player)
{
    uint32 statsValue = 0;
    uint32 specId = sRandomItemMgr.GetPlayerSpecId(player);

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            statsValue += sRandomItemMgr.GetStatWeight(item->GetProto()->ItemId, specId);
    }

    return statsValue;
}

void PlayerbotAI::_fillGearScoreData(Player *player, Item* item, std::vector<uint32>* gearScore, uint32& twoHandScore)
{
    if (!item)
        return;

    if (player->CanUseItem(item->GetProto()) != EQUIP_ERR_OK)
        return;

    uint8 type   = item->GetProto()->InventoryType;
    uint32 level = item->GetProto()->ItemLevel;

    switch (type)
    {
        case INVTYPE_2HWEAPON:
            twoHandScore = std::max(twoHandScore, level);
            break;
        case INVTYPE_WEAPON:
        case INVTYPE_WEAPONMAINHAND:
            (*gearScore)[SLOT_MAIN_HAND] = std::max((*gearScore)[SLOT_MAIN_HAND], level);
            break;
        case INVTYPE_SHIELD:
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_HOLDABLE:
            (*gearScore)[EQUIPMENT_SLOT_OFFHAND] = std::max((*gearScore)[EQUIPMENT_SLOT_OFFHAND], level);
            break;
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_RANGED:
        case INVTYPE_QUIVER:
        case INVTYPE_RELIC:
            (*gearScore)[EQUIPMENT_SLOT_RANGED] = std::max((*gearScore)[EQUIPMENT_SLOT_RANGED], level);
            break;
        case INVTYPE_HEAD:
            (*gearScore)[EQUIPMENT_SLOT_HEAD] = std::max((*gearScore)[EQUIPMENT_SLOT_HEAD], level);
            break;
        case INVTYPE_NECK:
            (*gearScore)[EQUIPMENT_SLOT_NECK] = std::max((*gearScore)[EQUIPMENT_SLOT_NECK], level);
            break;
        case INVTYPE_SHOULDERS:
            (*gearScore)[EQUIPMENT_SLOT_SHOULDERS] = std::max((*gearScore)[EQUIPMENT_SLOT_SHOULDERS], level);
            break;
        case INVTYPE_BODY:
            (*gearScore)[EQUIPMENT_SLOT_BODY] = std::max((*gearScore)[EQUIPMENT_SLOT_BODY], level);
            break;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
            (*gearScore)[EQUIPMENT_SLOT_CHEST] = std::max((*gearScore)[EQUIPMENT_SLOT_CHEST], level);
            break;
        case INVTYPE_WAIST:
            (*gearScore)[EQUIPMENT_SLOT_WAIST] = std::max((*gearScore)[EQUIPMENT_SLOT_WAIST], level);
            break;
        case INVTYPE_LEGS:
            (*gearScore)[EQUIPMENT_SLOT_LEGS] = std::max((*gearScore)[EQUIPMENT_SLOT_LEGS], level);
            break;
        case INVTYPE_FEET:
            (*gearScore)[EQUIPMENT_SLOT_FEET] = std::max((*gearScore)[EQUIPMENT_SLOT_FEET], level);
            break;
        case INVTYPE_WRISTS:
            (*gearScore)[EQUIPMENT_SLOT_WRISTS] = std::max((*gearScore)[EQUIPMENT_SLOT_WRISTS], level);
            break;
        case INVTYPE_HANDS:
            (*gearScore)[EQUIPMENT_SLOT_HANDS] = std::max((*gearScore)[EQUIPMENT_SLOT_HANDS], level);
            break;
        // equipped gear score check uses both rings and trinkets for calculation, assume that for bags/banks it is the same
        // with keeping second highest score at second slot
        case INVTYPE_FINGER:
        {
            if ((*gearScore)[EQUIPMENT_SLOT_FINGER1] < level)
            {
                (*gearScore)[EQUIPMENT_SLOT_FINGER2] = (*gearScore)[EQUIPMENT_SLOT_FINGER1];
                (*gearScore)[EQUIPMENT_SLOT_FINGER1] = level;
            }
            else if ((*gearScore)[EQUIPMENT_SLOT_FINGER2] < level)
                (*gearScore)[EQUIPMENT_SLOT_FINGER2] = level;
            break;
        }
        case INVTYPE_TRINKET:
        {
            if ((*gearScore)[EQUIPMENT_SLOT_TRINKET1] < level)
            {
                (*gearScore)[EQUIPMENT_SLOT_TRINKET2] = (*gearScore)[EQUIPMENT_SLOT_TRINKET1];
                (*gearScore)[EQUIPMENT_SLOT_TRINKET1] = level;
            }
            else if ((*gearScore)[EQUIPMENT_SLOT_TRINKET2] < level)
                (*gearScore)[EQUIPMENT_SLOT_TRINKET2] = level;
            break;
        }
        case INVTYPE_CLOAK:
            (*gearScore)[EQUIPMENT_SLOT_BACK] = std::max((*gearScore)[EQUIPMENT_SLOT_BACK], level);
            break;
        default:
            break;
    }
}

std::string PlayerbotAI::BotStateToString(BotState state)
{
    switch (state)
    {
        case BotState::BOT_STATE_COMBAT: return "Combat";
        case BotState::BOT_STATE_NON_COMBAT: return "Non Combat";
        case BotState::BOT_STATE_DEAD: return "Dead";
        case BotState::BOT_STATE_REACTION: return "Reaction";
        default: return "";
    }
}

std::string PlayerbotAI::GetDefaultMovementStrategy()
{
    // Player master -> follow
    if (HasActivePlayerMaster())
        return "follow";

    // Bot/no master -> wander
    return "wander";
}

void PlayerbotAI::EnsureDefaultMovementStrategy(Player* requester)
{
    std::string movement = GetDefaultMovementStrategy();

    for (BotState state : { BotState::BOT_STATE_REACTION, BotState::BOT_STATE_NON_COMBAT })
    {
        if (HasStrategy("stay", state))
            ChangeStrategy("-stay", state);

        if (!HasStrategy(movement, state))
            ChangeStrategy("+" + movement, state);
    }

    if (requester)
    {
        TellPlayerNoFacing(
            requester,
            "Welcome back!",
            PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL,
            false
        );
    }
}

std::string PlayerbotAI::HandleRemoteCommand(std::string command)
{
    if (command == "state")
    {
        switch (currentState)
        {
        case BotState::BOT_STATE_COMBAT:
            return "combat";
        case BotState::BOT_STATE_DEAD:
            return "dead";
        case BotState::BOT_STATE_NON_COMBAT:
            return "non-combat";
        default:
            return "unknown";
        }
    }
    else if (command == "position")
    {
        std::ostringstream out; out << bot->GetPositionX() << " " << bot->GetPositionY() << " " << bot->GetPositionZ() << " " << bot->GetMapId() << " " << bot->GetOrientation();
        uint32 area = sServerFacade.GetAreaId(bot);
        if (const AreaTableEntry* areaEntry = GetAreaEntryByAreaID(area))
        {
            if (AreaTableEntry const* zoneEntry = areaEntry->zone ? GetAreaEntryByAreaID(areaEntry->zone) : areaEntry)
                out << " |" << zoneEntry->area_name[0] << "|";
        }
        return out.str();
    }
    else if (command == "tpos")
    {
        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target) {
            return "";
        }

        std::ostringstream out; out << target->GetPositionX() << " " << target->GetPositionY() << " " << target->GetPositionZ() << " " << target->GetMapId() << " " << target->GetOrientation();
        return out.str();
    }
    else if (command == "target")
    {
        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target) {
            return "";
        }

        return target->GetName();
    }
    else if (command == "hp")
    {
        int pct = (int)((static_cast<float> (bot->GetHealth()) / bot->GetMaxHealth()) * 100);
        std::ostringstream out; out << pct << "%";

        Unit* target = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (!target) {
            return out.str();
        }

        pct = (int)((static_cast<float> (target->GetHealth()) / target->GetMaxHealth()) * 100);
        out << " / " << pct << "%";
        return out.str();
    }
    else if (command == "combat")
    {
        std::ostringstream out;

        bool unitFlagInCombat = bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IN_COMBAT);
        out << "UNIT_FLAG_IN_COMBAT: " << (unitFlagInCombat ? "SET" : "clear");

        out << ", IsInCombat(): " << (bot->IsInCombat() ? "true" : "false");

        out << ", CMaNGOS attackers: " << bot->getAttackers().size();

        Unit* victim = bot->GetVictim();
        out << ", victim: " << (victim ? victim->GetName() : "none");

        out << " | BotAI: current target: ";
        Unit* aiTarget = *GetAiObjectContext()->GetValue<Unit*>("current target");
        if (aiTarget)
        {
            out << aiTarget->GetName() << " (" << aiTarget->GetObjectGuid().GetCounter() << ")";
            bool isInvalid = GetAiObjectContext()->GetValue<bool>("invalid target", "current target")->Get();
            out << ", invalid: " << (isInvalid ? "YES" : "no");
        }
        else
        {
            out << "none";
        }

        bool hasAttackers = GetAiObjectContext()->GetValue<bool>("has attackers")->Get();
        out << ", has attackers: " << (hasAttackers ? "true" : "false");

        out << " | Selection: " << bot->GetSelectionGuid().GetCounter();

        return out.str();
    }
    else if (command == "strategy")
    {
        return currentEngine->ListStrategies();
    }
    else if (command == "action")
    {
        return currentEngine->GetLastAction();
    }
    else if (command == "values")
    {
        return GetAiObjectContext()->FormatValues();
    }
    else if (command == "travel")
    {
        std::ostringstream out;

        TravelTarget* target = GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();

        if (target->GetGroupmember() && target->GetGroupmember().GetPlayer())
            out << target->GetGroupmember().GetPlayer()->GetName() << "'s ";

        if (target->GetDestination()) {
            out << "Target: " << target->GetDestination()->GetTitle();
        }

        if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE)
        {
            out << "\nStatus: ";
            if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
                out << "ready";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
                out << "preparing";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL)
                out << (target->IsForced() ? "forced traveling" : "traveling");
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
                out << "working";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN)
                out << "cooldown";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << "expired";

            if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << " [for " << (target->GetTimeLeft() / 1000) << "s]";

            if (target->GetRetryCount(true) || target->GetRetryCount(false))
                out << "(retry " << target->GetRetryCount(true) << "/" << target->GetRetryCount(false) << ")";           
        }

        return out.str();
    }
    else if (command == "traveldetail")
    {
        std::ostringstream out;

        TravelTarget* target = GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();

        if (target->GetGroupmember() && target->GetGroupmember().GetPlayer())
            out << target->GetGroupmember().GetPlayer()->GetName() << "'s ";

        if (target->GetDestination()) {
            out << target->GetDestination()->GetShortName() << " travel target";

            out << "\nTarget: " << target->GetDestination()->GetTitle();

            out << "\nDistance " << round(target->GetDestination()->DistanceTo(bot)) << "y";

            if (*target->GetPosition())
            {
                out << "\nLocation: " << target->GetPosition()->getAreaName();
                out << " (" << round(target->GetPosition()->distance(bot)) << "y)";
            }
        }
        
        if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_NONE)
        {
            out << "\nStatus: ";
            if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
                out << "ready";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
                out << "preparing";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL)
                out << (target->IsForced() ? "forced traveling" : "traveling");
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
                out << "working";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_COOLDOWN)
                out << "cooldown";
            else if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << "expired";

            if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << " [for " << (target->GetTimeLeft() / 1000) << "s]";

            if (target->GetRetryCount(true) || target->GetRetryCount(false))
                out << "(retry " << target->GetRetryCount(true) << "/" << target->GetRetryCount(false) << ")";

            if (target->GetConditions().size())
            {
                out << "\nConditions: ";

                for (auto& condition : target->GetConditions())
                {
                    AiObjectContext* context = GetAiObjectContext();
                    out << condition;
                    if (AI_VALUE(bool, condition))
                        out << " (true)";
                    else
                        out << " (false)";

                    if (condition != target->GetConditions().back())
                        out << ", ";
                }
            }
        }

        return out.str();
    }
    else if (command.find("budget") == 0)
    {
        std::string sub;

        if (command.size() > 7)
            sub = command.substr(7);

        std::ostringstream out;

        AiObjectContext* context = GetAiObjectContext();

        if (sub.empty())
        {
            out << "Current money: " << ChatHelper::formatMoney(bot->GetMoney()) << " free to use:" << ChatHelper::formatMoney(AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::anything)) << "\n";
            out << "Purpose: Needed | Available\n";
        }

        for (uint32 i = 1; i < (uint32)NeedMoneyFor::anything; i++)
        {
            NeedMoneyFor needMoneyFor = NeedMoneyFor(i);

            std::unordered_map<NeedMoneyFor, std::string> needStrMap =
            { { NeedMoneyFor::none, "nothing"},
              { NeedMoneyFor::repair, "repair"},
              { NeedMoneyFor::ammo, "ammo"},
              { NeedMoneyFor::spells, "spells"},
              { NeedMoneyFor::travel, "travel"},
              { NeedMoneyFor::consumables, "consumables"},
              { NeedMoneyFor::gear, "gear"},
              { NeedMoneyFor::guild, "guild"},
              { NeedMoneyFor::tradeskill, "tradeskill"},
              { NeedMoneyFor::skilltraining, "skilltraining"},
              { NeedMoneyFor::ah, "ah"},
              { NeedMoneyFor::mount, "mount"},
              { NeedMoneyFor::anything, "anything"}};

            if (!sub.empty() && needStrMap.at(needMoneyFor).find(sub) == std::string::npos)
                continue;

            out <<  needStrMap.at(needMoneyFor);

            out << ": " << ChatHelper::formatMoney(AI_VALUE2(uint32, "money needed for", i)) << " | " << ChatHelper::formatMoney(AI_VALUE2(uint32, "free money for", i));

            if (i != (uint32)NeedMoneyFor::mount)
                out << "\n";
        }

        return out.str();
    }
    std::ostringstream out; out << "invalid command: " << command;
    return out.str();
}

bool PlayerbotAI::HasSkill(SkillType skill)
{
    return bot->HasSkill(skill) && bot->GetSkillValue(skill) > 0;
}

bool ChatHandler::HandlePlayerbotCommand(char* args)
{
    return PlayerbotMgr::HandlePlayerbotMgrCommand(this, args);
}

bool ChatHandler::HandleRandomPlayerbotCommand(char* args)
{
    return RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(this, args);
}


/*
 * DRESSING A BOT NPC FROM A SAVED LOOK.
 *
 * The dressing room on the website stores a look as its own canonical query
 * string -- `race=1&sex=0&skin=4&...&chest=1184&legs=2076` -- in
 * tw_web.web_fashion (website/sql/006_fashion.sql). `.npcbot dress <name> <id>`
 * reads one of those rows and puts it on a bot NPC.
 *
 * WHY AN ID AND NOT THE STRING ITSELF, which was the first idea. A full outfit
 * query measures 230 characters, and `.npcbot dress Orcy ` in front of it makes
 * 249 against a 255-character chat box. That is not "it fits"; it is "it fits
 * until somebody adds a slot or a display id gains a sixth digit", and a
 * truncated query string still PARSES -- it just quietly loses the last item.
 * A wrong outfit with no error is the worst failure available here. The raw
 * string is still accepted, because it is how this gets tested without the
 * website in the loop, but the id is the form meant for people.
 *
 * READING ANOTHER DATABASE is the pattern the bug tracker already set: every
 * database here is on one MariaDB instance, so a qualified SELECT over the
 * connection this process already holds to tw_logs costs nothing. It carries
 * the same trap, and website/sql/007_fashion_grant.sql must be applied BEFORE
 * this binary runs -- a table the connection cannot see answers
 * ER_NO_SUCH_TABLE, which DatabaseMysql.cpp turns into ASSERT(false). Hence the
 * information_schema probe, cached, exactly as BugTableReachable does it.
 *
 * WHAT IT CANNOT DO: RACE AND SEX. A look carries them, but they are fixed when
 * a character is created and this command refuses a mismatch rather than
 * dressing a tauren in something drawn on a gnome. Choosing race and sex is a
 * character-creation decision, which is the seam where making an NPC from
 * scratch on the website is a genuinely different feature from this one.
 */
namespace
{
    char const* const FASHION_DB = "tw_web";

    // Slot names as the dressing room spells them, in EQUIPMENT_SLOT order
    // where one exists. The inventory types are the same table
    // tools/model-browser/lib/character.js keys its slots by -- the two have to
    // agree, because one picks the picture and the other picks the item.
    struct DressSlot
    {
        char const* name;
        uint8       slot;
        uint32      invTypes[4];   // 0-terminated
    };

    DressSlot const DRESS_SLOTS[] =
    {
        { "head",     EQUIPMENT_SLOT_HEAD,      {  1, 0, 0, 0 } },
        { "shoulder", EQUIPMENT_SLOT_SHOULDERS, {  3, 0, 0, 0 } },
        { "shirt",    EQUIPMENT_SLOT_BODY,      {  4, 0, 0, 0 } },
        { "chest",    EQUIPMENT_SLOT_CHEST,     {  5, 20, 0, 0 } },
        { "waist",    EQUIPMENT_SLOT_WAIST,     {  6, 0, 0, 0 } },
        { "legs",     EQUIPMENT_SLOT_LEGS,      {  7, 0, 0, 0 } },
        { "feet",     EQUIPMENT_SLOT_FEET,      {  8, 0, 0, 0 } },
        { "wrist",    EQUIPMENT_SLOT_WRISTS,    {  9, 0, 0, 0 } },
        { "hands",    EQUIPMENT_SLOT_HANDS,     { 10, 0, 0, 0 } },
        { "back",     EQUIPMENT_SLOT_BACK,      { 16, 0, 0, 0 } },
        { "tabard",   EQUIPMENT_SLOT_TABARD,    { 19, 0, 0, 0 } },
        { "mainhand", EQUIPMENT_SLOT_MAINHAND,  { 13, 17, 21, 0 } },
        { "offhand",  EQUIPMENT_SLOT_OFFHAND,   { 13, 14, 22, 23 } },
        { "ranged",   EQUIPMENT_SLOT_RANGED,    { 15, 25, 26, 0 } },
    };

    int s_fashionTableState = -1;

    bool FashionTableReachable()
    {
        if (s_fashionTableState >= 0)
            return s_fashionTableState == 1;

        s_fashionTableState = 0;
        if (QueryResult* result = LogsDatabase.PQuery(
                "SELECT COUNT(*) FROM information_schema.tables "
                "WHERE table_schema = '%s' AND table_name = 'web_fashion'", FASHION_DB))
        {
            if (result->Fetch()[0].GetUInt32() > 0)
                s_fashionTableState = 1;
            delete result;
        }

        if (s_fashionTableState != 1)
            sLog.outError("Bot NPCs: %s.web_fashion is not readable, so `.npcbot dress <id>` cannot "
                          "work. Apply website/sql/007_fashion_grant.sql.", FASHION_DB);

        return s_fashionTableState == 1;
    }

    // For error messages. A number tells somebody nothing about what to go and
    // draw; "Orc" tells them exactly.
    std::string RaceName(uint32 race)
    {
        if (ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race))
            if (entry->name[0])
                return entry->name[0];

        return "race " + std::to_string(race);
    }

    char const* SexName(uint32 sex) { return sex ? "female" : "male"; }

    struct BotNpcLook
    {
        uint32 race, sex, skin, face, hair, hairColor, facialHair;
        bool   haveRace, haveSex;
        // slot index -> display id
        std::map<uint8, uint32> items;

        BotNpcLook() : race(0), sex(0), skin(0), face(0), hair(0), hairColor(0),
                       facialHair(0), haveRace(false), haveSex(false) {}
    };

    // `a=1&b=2` into the struct. Anything unrecognised is IGNORED rather than
    // refused: the dressing room puts `glow`, `v` and `name` in the same string
    // and none of them mean anything to a character standing in a world. `hold`
    // does, but it is not an item -- BotNpc_SheathFor reads it straight off the
    // look, so nothing about this parse has to carry it.
    bool ParseLook(std::string const& query, BotNpcLook& out)
    {
        size_t at = 0;
        bool sawAnything = false;

        while (at <= query.size())
        {
            size_t amp = query.find('&', at);
            std::string pair = query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);

            size_t eq = pair.find('=');
            if (eq != std::string::npos)
            {
                std::string key = pair.substr(0, eq);
                std::string val = pair.substr(eq + 1);

                // Every value this cares about is a number. A non-numeric one is
                // somebody else's parameter (a look's name), not a failure.
                bool numeric = !val.empty();
                for (size_t i = 0; i < val.size(); ++i)
                    if (!isdigit((unsigned char)val[i]))
                        numeric = false;

                if (numeric)
                {
                    uint32 n = uint32(atoi(val.c_str()));

                    if      (key == "race")      { out.race = n; out.haveRace = true; sawAnything = true; }
                    else if (key == "sex")       { out.sex = n;  out.haveSex = true;  sawAnything = true; }
                    else if (key == "skin")      { out.skin = n;       sawAnything = true; }
                    else if (key == "face")      { out.face = n;       sawAnything = true; }
                    else if (key == "hair")      { out.hair = n;       sawAnything = true; }
                    else if (key == "haircolor") { out.hairColor = n;  sawAnything = true; }
                    else if (key == "facial")    { out.facialHair = n; sawAnything = true; }
                    else
                    {
                        for (size_t s = 0; s < sizeof(DRESS_SLOTS) / sizeof(DRESS_SLOTS[0]); ++s)
                        {
                            if (key == DRESS_SLOTS[s].name)
                            {
                                if (n)
                                    out.items[DRESS_SLOTS[s].slot] = n;
                                sawAnything = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (amp == std::string::npos)
                break;
            at = amp + 1;
        }

        return sawAnything;
    }

    // A look stores item_template.display_id, because that is what a picture
    // needs. A character needs a real item, so this picks ANY item wearing that
    // display in a type that fits the slot -- which one does not matter, since
    // nobody loots a bot NPC and the point is the silhouette.
    //
    // Ordered by entry so the same look always resolves to the same items:
    // without it two dressings of one look could differ, and the difference
    // would be invisible until somebody inspected the NPC.
    uint32 ItemForDisplay(uint32 displayId, DressSlot const& slot)
    {
        std::ostringstream types;
        for (size_t i = 0; i < 4 && slot.invTypes[i]; ++i)
        {
            if (i)
                types << ",";
            types << slot.invTypes[i];
        }

        uint32 entry = 0;
        if (QueryResult* result = WorldDatabase.PQuery(
                "SELECT entry FROM item_template WHERE display_id = %u AND inventory_type IN (%s) "
                "ORDER BY entry LIMIT 1", displayId, types.str().c_str()))
        {
            entry = result->Fetch()[0].GetUInt32();
            delete result;
        }

        return entry;
    }
}

/*
 * Puts `look` on `bot`. Returns false and fills `error` only for things that
 * stop the whole job; a single slot that cannot be resolved is reported in
 * `skipped` and the rest of the outfit still goes on, because half a suit of
 * armour is more useful than a refusal and the person can see which piece is
 * missing.
 */
static bool DressBotNpc(Player* bot, BotNpcLook const& look, std::string& error, std::vector<std::string>& skipped)
{
    // Named on both sides. "a different race" leaves somebody guessing which of
    // the two is wrong and what to do about it; naming them says go and draw it
    // on an Orc.
    if (look.haveRace && look.race != bot->GetRace())
    {
        error = "that look is a " + RaceName(look.race) + " and this is a " + RaceName(bot->GetRace()) +
                " -- race is fixed when a character is made, so it cannot be dressed on";
        return false;
    }

    if (look.haveSex && look.sex != bot->GetGender())
    {
        error = std::string("that look is ") + SexName(look.sex) + " and this is " + SexName(bot->GetGender()) +
                " -- gender is fixed when a character is made, so it cannot be dressed on";
        return false;
    }

    // The customisation half. These are the same four bytes Player::Create
    // writes at character creation, so setting them here is the same operation
    // the barber shop would be if this client had one.
    bot->SetByteValue(PLAYER_BYTES, 0, uint8(look.skin));
    bot->SetByteValue(PLAYER_BYTES, 1, uint8(look.face));
    bot->SetByteValue(PLAYER_BYTES, 2, uint8(look.hair));
    bot->SetByteValue(PLAYER_BYTES, 3, uint8(look.hairColor));
    bot->SetByteValue(PLAYER_BYTES_2, 0, uint8(look.facialHair));

    for (size_t s = 0; s < sizeof(DRESS_SLOTS) / sizeof(DRESS_SLOTS[0]); ++s)
    {
        DressSlot const& ds = DRESS_SLOTS[s];

        std::map<uint8, uint32>::const_iterator want = look.items.find(ds.slot);

        Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, ds.slot);

        // A slot the look leaves empty is EMPTIED, not left alone. Dressing is
        // "wear this outfit", not "add these pieces" -- otherwise whatever the
        // factory last rolled shows through the gaps and the result is nobody's
        // design.
        if (want == look.items.end())
        {
            if (oldItem)
                bot->DestroyItem(oldItem->GetBagSlot(), oldItem->GetSlot(), true);
            continue;
        }

        /*
         * ALREADY WEARING THAT LOOK? Then nothing to do, and this is checked on
         * the DISPLAY rather than on the item entry so it can be checked BEFORE
         * ItemForDisplay runs a query. That matters because dressing is now
         * idempotent by design -- it runs on every login and every reload -- and
         * the common case must cost nothing.
         *
         * It also fixes a real leak. The old check compared item entries after
         * resolving, so any path where the comparison failed re-equipped from
         * scratch: EquipNewItem makes a NEW item_instance, the old row is left
         * behind, and Orcy had accumulated 32 orphaned items across a handful
         * of dressings before anybody counted.
         */
        if (oldItem && oldItem->GetProto() && oldItem->GetProto()->DisplayInfoID == want->second)
            continue;

        uint32 itemId = ItemForDisplay(want->second, ds);
        if (!itemId)
        {
            skipped.push_back(std::string(ds.name) + " (no item with display " + std::to_string(want->second) + ")");
            sLog.outString("[BOTNPC DRESS] %s: %s display %u -> NO ITEM",
                           bot->GetName(), ds.name, want->second);
            continue;
        }

        /*
         * FORCED ON, past every rule about who is allowed to wear it.
         *
         * A COSTUME IS NOT A LOADOUT. This character never fights, never levels
         * and never loots; the only thing its clothes do is be seen. So the
         * questions CanUseItem asks -- is this your class, your race, your
         * level, do you have the proficiency -- are all questions about a
         * wearer, and there is no wearer here, only a mannequin. Refusing a
         * mage's robe because the body underneath is a warrior would make the
         * dressing room a liar: it drew the robe, and the whole point of
         * choosing an NPC's look on a website is that you get the look you
         * chose.
         *
         * Neither CanEquipUnseenItem nor CanEquipNewItem can be used for that.
         * Both end in Player::CanUseItem, which enforces AllowableClass,
         * AllowableRace, RequiredLevel and the armour/weapon proficiency skill
         * no matter what `not_loading` says (Player.cpp:12513) -- so a bot NPC
         * silently lost any piece drawn for another class, and the only clue
         * was one line of "cannot be worn there" among the skipped.
         *
         * FindEquipSlot is still asked, and is the one check that survives,
         * because it answers a different kind of question: does this item go in
         * this slot AT ALL. A tabard in a boot slot is not a permission problem,
         * it is a nonsense, and forcing it would put an item somewhere no
         * renderer looks.
         *
         * Safe because EquipItem does not re-validate: the stats it applies are
         * meaningless on something that never rolls a die, and the visible-item
         * fields -- the only part that matters -- are set the same either way.
         */
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
        uint8 eslot = proto ? bot->FindEquipSlot(proto, ds.slot, true) : uint8(NULL_SLOT);

        if (eslot == NULL_SLOT)
        {
            skipped.push_back(std::string(ds.name) + " (does not go in that slot)");
            sLog.outString("[BOTNPC DRESS] %s: %s item %u (invtype %u) -> NO SLOT (wanted %u)",
                           bot->GetName(), ds.name, itemId,
                           proto ? proto->InventoryType : 0, uint32(ds.slot));
            continue;
        }

        if (oldItem)
            bot->DestroyItem(oldItem->GetBagSlot(), oldItem->GetSlot(), true);

        uint16 dest = uint16((INVENTORY_SLOT_BAG_0 << 8) | eslot);

        if (!bot->EquipNewItem(dest, itemId, true))
        {
            skipped.push_back(std::string(ds.name) + " (equip refused)");
            sLog.outString("[BOTNPC DRESS] %s: %s item %u -> EQUIP REFUSED at slot %u",
                           bot->GetName(), ds.name, itemId, uint32(eslot));
        }
        else
        {
            // Only what CHANGED. Dressing runs on every pass of the bot
            // manager, so a line per slot per pass would be two hundred lines a
            // minute saying nothing happened.
            sLog.outString("[BOTNPC DRESS] %s: %s display %u -> item %u at slot %u",
                           bot->GetName(), ds.name, want->second, itemId, uint32(eslot));
        }
    }

    return true;
}

/*
 * `.add_dressing_room <id>`: the items of a saved dressing room look, in your
 * bags. GM only, because they are conjured for free.
 *
 * The same look and the same display -> item resolution `.npcbot dress` uses,
 * so one look always means the same items whichever way it is fetched. Lowest
 * entry wins, which can be an item with real stats or a test item; for a GM
 * trying an outfit on, that is fine.
 *
 * Race, sex and customisation are ignored. They are the doll, not the clothes,
 * and a look drawn on an orc is still a set of items a gnome can carry. Unlike
 * `.npcbot dress` it does NOT force anything on: the items go in the bags and
 * wearing them is up to the ordinary rules.
 *
 * Kept here rather than in Commands.cpp because DRESS_SLOTS, ParseLook and
 * ItemForDisplay live in this file's anonymous namespace; PlayerbotStubs.cpp
 * carries the no-module stub.
 */
bool ComfyCommands::HandleAddDressingRoomCommand(ChatHandler* handler, char* args)
{
    Player* me = handler->m_session ? handler->m_session->GetPlayer() : nullptr;
    if (!me)
    {
        handler->SendSysMessage("In game only.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    std::string given = args ? args : "";
    while (!given.empty() && isspace((unsigned char)given[0]))
        given.erase(0, 1);
    while (!given.empty() && isspace((unsigned char)given[given.size() - 1]))
        given.erase(given.size() - 1);

    if (given.empty())
    {
        handler->SendSysMessage("Usage: .add_dressing_room <saved look id>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // An id, or a raw query string for testing without the website: the same
    // two forms `.npcbot dress` takes.
    bool isId = true;
    for (size_t i = 0; i < given.size(); ++i)
        if (!isdigit((unsigned char)given[i]))
            isId = false;

    std::string query;
    std::string lookName;

    if (isId)
    {
        if (!FashionTableReachable())
        {
            handler->SendSysMessage("Saved looks are not readable. Apply website/sql/007_fashion_grant.sql.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 lookId = uint32(atoi(given.c_str()));
        if (QueryResult* result = LogsDatabase.PQuery(
                "SELECT `look`, `name` FROM `%s`.`web_fashion` WHERE id = %u", FASHION_DB, lookId))
        {
            query = result->Fetch()[0].GetCppString();
            lookName = result->Fetch()[1].GetCppString();
            delete result;
        }

        if (query.empty())
        {
            handler->PSendSysMessage("No saved look %u.", lookId);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (lookName.empty())
            lookName = "look " + given;
    }
    else
    {
        query = given;
        lookName = "that look";
    }

    BotNpcLook look;
    if (!ParseLook(query, look) || look.items.empty())
    {
        handler->SendSysMessage("That look has no items in it.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    uint32 added = 0;
    std::vector<std::string> skipped;

    for (size_t s = 0; s < sizeof(DRESS_SLOTS) / sizeof(DRESS_SLOTS[0]); ++s)
    {
        DressSlot const& ds = DRESS_SLOTS[s];

        std::map<uint8, uint32>::const_iterator want = look.items.find(ds.slot);
        if (want == look.items.end())
            continue;

        uint32 itemId = ItemForDisplay(want->second, ds);
        if (!itemId)
        {
            skipped.push_back(std::string(ds.name) + " (no item)");
            continue;
        }

        ItemPosCountVec dest;
        if (me->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) != EQUIP_ERR_OK || dest.empty())
        {
            skipped.push_back(std::string(ds.name) + " (bags full)");
            continue;
        }

        Item* item = me->StoreNewItem(dest, itemId, true, Item::GenerateItemRandomPropertyId(itemId));
        if (!item)
        {
            skipped.push_back(std::string(ds.name) + " (not created)");
            continue;
        }

        // Unbound, the same as .additem does for yourself, so a piece can be
        // handed on.
        item->SetBinding(false);
        me->SendNewItem(item, 1, false, true);
        ++added;
    }

    handler->PSendSysMessage("%s: %u item%s added.", lookName.c_str(), added, added == 1 ? "" : "s");

    if (!skipped.empty())
    {
        std::string line = "Skipped: ";
        for (size_t i = 0; i < skipped.size(); ++i)
        {
            if (i)
                line += ", ";
            line += skipped[i];
        }
        handler->SendSysMessage(line.c_str());
    }

    return true;
}

/*
 * The core's way in. Declared in game/BotNpc.h and stubbed in
 * src/game/PlayerbotStubs.cpp -- the core never includes playerbot headers, so
 * `reload bot_npc` reaches the dressing through this and nothing else.
 */
bool BotNpc_Dress(Player* bot, std::string const& look, std::string& error)
{
    if (!bot)
    {
        error = "no character";
        return false;
    }

    BotNpcLook parsed;
    if (!ParseLook(look, parsed))
    {
        error = "that does not read as a look";
        return false;
    }

    std::vector<std::string> skipped;
    if (!DressBotNpc(bot, parsed, error, skipped))
        return false;

    // Drawn, put away, or the bow out. Part of the look and the only part of
    // it that is not an item, so it lands here rather than in DressBotNpc --
    // and it has to land somewhere on this path, because `.npcbot dress`
    // reaches neither login nor the reload that also apply it. BotNpc.h.
    bot->SetSheath(BotNpc_SheathFor(look));

    bot->SaveToDB();
    return true;
}

/*
 * PUT A DESIGN'S NAME ON THE CHARACTER WEARING IT.
 *
 * THE CHARACTER IS NOT IN WORLD WHEN THIS MATTERS, which is the whole reason it
 * exists. PlayerbotHolder::CreateBot builds a Player, saves it, then logs the
 * throwaway session out and DELETES the object -- so the `place` that just
 * called it holds a guid and nothing else, and the rename that used to sit
 * there was written as `if (bot) ...` and therefore never once ran. The body
 * stood up as "Discipleone" and stayed that way until somebody happened to run
 * `reload bot_npc`, which is the only other thing that renames (BotNpcMgr::
 * ApplyToLoggedIn). Bug 20.
 *
 * SO THE ROW IS WRITTEN DIRECTLY -- AND INSIDE A TRANSACTION KEYED ON THE GUID,
 * which is not decoration. A bare PExecute goes on the general delay queue
 * while Player::SaveToDB's transaction goes to worker `guid % WorkerThreads`
 * (Database::AddToSerialDelayQueue), so the two are on different threads and
 * the UPDATE can land BEFORE the REPLACE INTO that created the row -- losing
 * the rename, intermittently, which is the worst way to lose one. Opening a
 * transaction with the same serial id puts this behind that save in the same
 * queue, in order, by construction.
 *
 * The in-memory path is kept for the case where the character IS loaded,
 * because then the row is not the truth: SaveToDB writes the whole thing from
 * the Player, so a bare UPDATE under a live character is a value waiting to be
 * overwritten.
 *
 * THE CACHE IS THE HALF THAT IS EASY TO FORGET. GetPlayerGuidByName answers
 * from sObjectMgr's cache, so a body findable only by its stripped name is a
 * body the next `place` will not adopt -- it will make a second one instead.
 * The old name is read back OUT of the cache rather than assumed, because
 * ObjectMgr::LoadPlayerCacheData puts names through normalizePlayerName and
 * that lowercases everything after the first letter: "BigREDDog" goes in as
 * "Bigreddog", and erasing the wrong key leaves the stale one pointing at us.
 */
static void BotNpc_RenameCharacter(uint32 guidLow, Player* bot, std::string const& oldName, std::string const& newName)
{
    if (!guidLow || newName.empty())
        return;

    std::string cached = oldName;
    if (PlayerCacheData const* data = sObjectMgr.GetPlayerDataByGUID(guidLow))
        cached = data->sName;

    if (bot)
    {
        if (bot->GetName() == newName)
            return;

        std::string const was = bot->GetName();
        bot->SetName(newName);
        sObjectMgr.ChangePlayerNameInCache(guidLow, cached.empty() ? was : cached, newName);
        bot->SaveToDB();
        return;
    }

    if (cached == newName)
        return;

    std::string safe = newName;
    CharacterDatabase.escape_string(safe);

    // BeginTransaction REFUSES if this thread already has one open, and taking
    // that as read would be worse than not trying: the PExecute below would
    // join somebody else's transaction and the Commit would close it early.
    if (CharacterDatabase.BeginTransaction(guidLow))
    {
        CharacterDatabase.PExecute("UPDATE characters SET name = '%s' WHERE guid = %u", safe.c_str(), guidLow);
        CharacterDatabase.CommitTransaction();
    }
    else
    {
        // Ordering is what the transaction bought, so give it up loudly rather
        // than quietly: this one can lose the race described above.
        sLog.outError("BotNpc_RenameCharacter: no transaction available for guid %u; renaming to %s unordered.",
                      guidLow, newName.c_str());
        CharacterDatabase.PExecute("UPDATE characters SET name = '%s' WHERE guid = %u", safe.c_str(), guidLow);
    }

    sObjectMgr.ChangePlayerNameInCache(guidLow, cached, newName);
}

/*
 * .npcbot -- turn one of your characters into a standing NPC.
 *
 * `.bot add` already logs a character in as a bot, and it is the wrong verb for
 * this: it says nothing about making an NPC, gives you no way to place one, and
 * drops the character wherever it last logged out -- so the usual first
 * experience is "I ran the command and nothing appeared". That is a command for
 * debugging bots. This is a command for building a world, and it is separate on
 * purpose.
 *
 *   .npcbot add <name>        make <name> an NPC standing HERE, now and after
 *                             every restart
 *   .npcbot move <name>       restand it where you are
 *   .npcbot menu <name> <id>  point it at a gossip_menu
 *   .npcbot remove <name>     back to an ordinary character, logged out
 *   .npcbot list              what exists and where
 *
 * THREE THINGS HAVE TO AGREE for one of these to survive a restart, which is
 * why this is a command rather than a line of SQL:
 *
 *   1. tw_char.bot_npc -- the binding: flags, menu, and the spot. Read at
 *      startup by BotNpcMgr and applied in Player::LoadFromDB.
 *   2. the "always" value on the bot store -- what makes the playerbot module
 *      log the character back in unprompted. PlayerbotAIConfig::
 *      loadFreeAltBotAccounts rebuilds freeAltBots from it at every startup and
 *      RandomPlayerbotMgr::LoginFreeBots does the logging in.
 *   3. freeAltBots itself -- the in-memory list, so it also appears NOW rather
 *      than only after the next restart.
 *
 * Miss (2) and the NPC is perfect until the server bounces, then gone. Miss (3)
 * and the command appears to do nothing, for as long as somebody is willing to
 * stand there waiting.
 *
 * RandomBotAutologin MUST STAY 1 in aiplayerbot.conf. LoginFreeBots sits behind
 * that gate, so with it off every NPC here is a silent no-show -- the same trap
 * CLAUDE.md already records for hired companions.
 */

bool ComfyCommands::HandleNpcBotCommand(ChatHandler* handler, char* args)
{
    Player* me = handler->m_session ? handler->m_session->GetPlayer() : nullptr;

    char* sub = handler->ExtractLiteralArg(&args);
    std::string action = sub ? sub : "";

    // ------------------------------------------------------------- list
    if (action == "list")
    {
        std::map<uint32, BotNpcEntry> const& all = sBotNpcMgr.All();
        if (all.empty())
        {
            handler->SendSysMessage("No bot NPCs. Design one on the website, then .npcbot place <name>.");
            return true;
        }

        for (const auto& itr : all)
        {
            BotNpcEntry const& e = itr.second;

            std::vector<BotNpcSpawn const*> copies = sBotNpcMgr.SpawnsOf(e.id);

            if (copies.empty())
            {
                handler->PSendSysMessage("%s (#%u) -- %s %s, not placed", e.name.c_str(), e.id,
                                SexName(e.gender), RaceName(e.race).c_str());
                continue;
            }

            handler->PSendSysMessage("%s (#%u) -- %s %s, menu %u",
                            e.name.c_str(), e.id, SexName(e.gender), RaceName(e.race).c_str(),
                            e.gossipMenuId);

            // One line per body: a design may stand in several places.
            for (BotNpcSpawn const* c : copies)
            {
                Player* inWorld = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, c->characterGuid), false);
                handler->PSendSysMessage("  map %u at %.0f %.0f %.0f -- %s", c->mapId, c->x, c->y, c->z,
                                inWorld ? "standing" : "not logged in");
            }
        }
        return true;
    }

    if (action == "add")
    {
        handler->SendSysMessage("`.npcbot add` is gone. A bot NPC is a DESIGN now -- made on the website, "
                       "named, raced and dressed there -- and `.npcbot place <name>` is what stands "
                       "one up.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // `del` is `unplace` by another name: it is the word people reach for, and
    // what it does is exactly unplace -- the body goes, the design stays.
    if (action == "del")
        action = "unplace";

    if (action != "place" && action != "move" && action != "reset" && action != "save" &&
        action != "unplace" && action != "menu" && action != "dress" && action != "idle" &&
        action != "emote")
    {
        handler->SendSysMessage("Usage: .npcbot place|move|reset|save|dress|menu|idle|emote|unplace <name or #id>, or .npcbot list");
        handler->SendSysMessage("  .npcbot place <name>      stand the design called <name> here, for good");
        handler->SendSysMessage("  .npcbot move <name>       bring it here for now; its post does not change");
        handler->SendSysMessage("  .npcbot reset <name>      send it back to its post");
        handler->SendSysMessage("  .npcbot save <name>       make where it is standing now its post");
        handler->SendSysMessage("  .npcbot dress <name>      re-wear the look on its design");
        handler->SendSysMessage("  .npcbot menu <name> <id>  give it a gossip_menu to say");
        handler->SendSysMessage("  .npcbot idle <name>       what it does unprompted; add a trigger to run one now");
        handler->SendSysMessage("  .npcbot emote <name>      what it answers emotes with; add an emote id to run one now");
        handler->SendSysMessage("  .npcbot unplace [name]    put the body away, the design stays; also .npcbot del");
        handler->SendSysMessage("Leave out the name on place, move, reset, save, dress and unplace to use the bot NPC you have selected.");
        // Not .npcbot subcommands, but they are the other half of giving an NPC
        // somewhere to walk, and this is where somebody looks for that.
        handler->SendSysMessage("With the NPC selected:");
        handler->SendSysMessage("  .spot <name>              save where you stand as \"<NPC>: <name>\" for it to walk to");
        handler->SendSysMessage("  .spotfrom                 how far ahead and right of its post you are standing");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // QUOTED OR PLAIN. A character name can contain a space -- nothing in
    // normalizePlayerName forbids one, and the column is the only real limit at
    // twelve characters -- so `.npcbot place "Help Elowen"` has to work.
    // ExtractLiteralArg stops dead at the quote and returns nothing, which came
    // out as "Which one?" and looked like the design was missing rather than
    // like the name never reached the lookup.
    char* nameArg = handler->ExtractQuotedOrLiteralArg(&args);
    BotNpcEntry const* found = nullptr;

    if (!nameArg || !*nameArg)
    {
        /*
         * NO NAME MEANS THE SELECTION, for the verbs that take nothing after
         * the name. You are usually standing in front of the one you mean, and
         * a selected character is unambiguous where a shared name is not.
         * GetSelectedPlayer answers with YOU when nothing is selected, which
         * finds no design, so that falls to the same refusal.
         *
         * NOT menu, idle or emote: their own argument would be read as the NPC
         * (`.npcbot emote 17` is design #17, not emote 17 on the selection).
         * `place` works on a selection only to re-post one already standing --
         * a design with no body has nothing to select.
         */
        if (action == "unplace" || action == "move" || action == "reset" || action == "save" ||
            action == "dress" || action == "place")
        {
            Player* target = handler->GetSelectedPlayer();
            found = target ? sBotNpcMgr.Get(target->GetGUIDLow()) : nullptr;

            if (!found)
            {
                handler->PSendSysMessage("Select a bot NPC, or .npcbot %s <name or #id>.", action.c_str());
                handler->SetSentErrorMessage(true);
                return false;
            }
        }
        else
        {
            handler->PSendSysMessage("Which one? .npcbot %s <name or #id>   (quote a name with a space)", action.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
    }

    /*
     * BY NAME OR BY #ID. Two designs may share a name since 2026-09-21 -- the
     * same NPC standing in two places, saying two different things -- and the
     * id is the only way to tell them apart. `#12` or a bare `12`; a name that
     * matches more than one design is REFUSED with the ids listed, never
     * resolved to whichever came first, because picking silently is how you
     * end up moving the wrong one of a pair that look identical in game.
     */
    if (!found)
    {
        char const* idText = nameArg[0] == '#' ? nameArg + 1 : nameArg;
        bool isId = *idText != 0;
        for (char const* c = idText; *c; ++c)
            if (!isdigit((unsigned char)*c))
                isId = false;

        if (isId)
        {
            found = sBotNpcMgr.FindById(uint32(atoi(idText)));
            if (!found)
            {
                handler->PSendSysMessage("No bot NPC #%s. .npcbot list shows them.", idText);
                handler->SetSentErrorMessage(true);
                return false;
            }
        }
        else
        {
            std::vector<BotNpcEntry const*> matches = sBotNpcMgr.FindByName(nameArg);
            if (matches.empty())
            {
                handler->PSendSysMessage("No bot NPC called %s. Design one on the website first.", nameArg);
                handler->SetSentErrorMessage(true);
                return false;
            }

            if (matches.size() > 1)
            {
                handler->PSendSysMessage("%u bot NPCs are called %s. Say which: .npcbot %s #<id>",
                                uint32(matches.size()), nameArg, action.c_str());
                for (BotNpcEntry const* m : matches)
                {
                    std::vector<BotNpcSpawn const*> copies = sBotNpcMgr.SpawnsOf(m->id);
                    if (copies.empty())
                        handler->PSendSysMessage("  #%u: not placed", m->id);
                    for (BotNpcSpawn const* c : copies)
                        handler->PSendSysMessage("  #%u: map %u at %.0f %.0f %.0f", m->id, c->mapId, c->x, c->y, c->z);
                }
                handler->SetSentErrorMessage(true);
                return false;
            }

            found = matches.front();
        }
    }

    // A working copy: everything below edits this and hands it to Save(), so a
    // half-applied change never reaches the row.
    BotNpcEntry entry = *found;
    std::string name = entry.name;

    // What to TYPE to reach this one again, for the hints below: its name while
    // that is unambiguous, its #id once another design shares the name.
    std::string handle;
    if (sBotNpcMgr.FindByName(name).size() > 1)
        handle = "#" + std::to_string(entry.id);
    else if (name.find(' ') != std::string::npos)
        handle = "\"" + name + "\"";
    else
        handle = name;

    /*
     * WHICH BODY. One design may stand in several places since sql/custom/113
     * (the same NPC in every starting zone), so a name says which NPC and not
     * which copy of it. The selection says which copy, and it wins whenever it
     * is one of this design's; a design with exactly one copy needs no
     * selection at all, which keeps every single NPC working the way it did.
     *
     * `chosen` stays null when there are several and none is selected. The
     * verbs that act on ONE body (unplace, move, reset, save, and place on a
     * selection) refuse then and list the copies; the rest pick for
     * themselves, below.
     */
    std::vector<BotNpcSpawn const*> const spawns = sBotNpcMgr.SpawnsOf(entry.id);
    BotNpcSpawn const* chosen = nullptr;

    if (Player* selected = me ? handler->GetSelectedPlayer() : nullptr)
        if (BotNpcSpawn const* s = sBotNpcMgr.GetSpawn(selected->GetGUIDLow()))
            if (s->designId == entry.id)
                chosen = s;

    // `place` needs to know: on a selected copy it re-posts that copy, and
    // otherwise it stands up a new one.
    bool const chosenBySelection = chosen != nullptr;

    if (!chosen && spawns.size() == 1)
        chosen = spawns.front();

    auto bodyOf = [](BotNpcSpawn const* s) -> Player*
    {
        return s ? sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, s->characterGuid), false) : nullptr;
    };

    // Every copy in world, for the verbs that change what the DESIGN says and
    // so have to reach all of them: menu and dress.
    std::vector<Player*> standing;
    for (BotNpcSpawn const* s : spawns)
        if (Player* p = bodyOf(s))
            if (p->IsInWorld())
                standing.push_back(p);

    Player* bot = bodyOf(chosen);

    // Idle and emote run a rule on one body to watch it. With no copy chosen,
    // the nearest one standing on your map, else any one standing.
    if (!bot && !standing.empty())
    {
        bot = standing.front();
        if (me)
        {
            float best = -1.0f;
            for (Player* p : standing)
            {
                if (p->GetMapId() != me->GetMapId())
                    continue;
                float d = me->GetDistance(p);
                if (best < 0.0f || d < best)
                {
                    best = d;
                    bot = p;
                }
            }
        }
    }

    // The refusal for a verb that needs ONE body and was not told which.
    auto needOneCopy = [&]() -> bool
    {
        if (spawns.empty())
        {
            handler->PSendSysMessage("%s is not placed. .npcbot place %s stands it here.", name.c_str(), handle.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (chosen)
            return true;

        handler->PSendSysMessage("%s stands in %u places. Select the one you mean:", name.c_str(), uint32(spawns.size()));
        for (BotNpcSpawn const* s : spawns)
            handler->PSendSysMessage("  map %u at %.0f %.0f %.0f", s->mapId, s->x, s->y, s->z);
        handler->SetSentErrorMessage(true);
        return false;
    };

    // ---------------------------------------------------------- unplace
    if (action == "unplace")
    {
        if (!needOneCopy())
            return false;

        uint32 wasGuid = chosen->characterGuid;
        bot = bodyOf(chosen);

        // THE DESIGN SURVIVES. Unplacing gives up the body, not the name, the
        // race, the look or the words -- place it again and it is the same NPC.
        // Deleting the design itself is the website's job, because that is
        // where it was made. With several copies this puts away the one you
        // selected and leaves the others standing.
        sBotNpcMgr.RemoveSpawn(wasGuid);
        chosen = nullptr;

        // Off the login list. No "always" event is touched: bot NPCs are kept
        // online by their own row, not by that flag -- see
        // PlayerbotAIConfig::loadBotNpcs for why leaning on it never survived a
        // restart. Clearing character_guid above is the whole of the unplace.
        sPlayerbotAIConfig.freeAltBots.remove_if(
            [wasGuid](std::pair<uint32, uint32> i) { return i.second == wasGuid; });

        if (bot)
        {
            // Flag off before the logout, so somebody standing there is not left
            // with a talk cursor on a character that is going.
            bot->SetUInt32Value(UNIT_NPC_FLAGS, 0);
            bot->SetBotNpcGossipMenuId(0);
            sRandomPlayerbotMgr.LogoutPlayerBot(wasGuid);
        }

        if (spawns.size() > 1)
            handler->PSendSysMessage("This %s is put away. %u more still standing.", name.c_str(), uint32(spawns.size() - 1));
        else
            handler->PSendSysMessage("%s is put away. The design is still there; .npcbot place %s brings it back.",
                            name.c_str(), handle.c_str());
        return true;
    }

    // ------------------------------------------------------------- menu
    if (action == "menu")
    {
        uint32 menuId;
        if (!handler->ExtractUInt32(&args, menuId))
        {
            handler->PSendSysMessage("Which menu? .npcbot menu %s <gossip_menu id>", handle.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        entry.gossipMenuId = menuId;
        sBotNpcMgr.Save(entry);

        // The menu is the design's, so every copy says it.
        for (Player* p : standing)
            p->SetBotNpcGossipMenuId(menuId);

        handler->PSendSysMessage("%s now shows menu %u.", name.c_str(), menuId);
        return true;
    }

    // ------------------------------------------------------------- idle
    /*
     * WHAT IT DOES UNPROMPTED, and a way to watch one without waiting for it.
     *
     * A loop on a forty second period is forty seconds per attempt to see
     * whether the third line reads right, and the `near` and `after_talk`
     * triggers need a second person or a lot of walking. This runs the rule
     * where it stands, now.
     *
     * THE TARGET IS THE ONE THE TRIGGER WOULD REALLY PASS, not always you: a
     * loop runs with nobody, because that is what it does in the world, and a
     * script written against a target it will not have in the field should fail
     * here too.
     */
    if (action == "idle")
    {
        BotNpcIdleRules const* rules = sBotNpcMgr.IdleRules(entry.id);

        static char const* const kindNames[BOT_NPC_IDLE_TRIGGERS] = { "loop", "near", "after_talk", "custom" };

        char* which = handler->ExtractLiteralArg(&args);
        std::string wanted = which ? which : "";

        if (wanted.empty())
        {
            if (!rules)
            {
                handler->PSendSysMessage("%s does nothing unprompted. Give it something on /admin/npcbots.", name.c_str());
                return true;
            }

            for (uint32 i = 0; i < BOT_NPC_IDLE_TRIGGERS; ++i)
            {
                BotNpcIdleRule const& rule = (*rules)[i];
                if (!rule.Runs())
                    continue;

                if (i == BOT_NPC_IDLE_CUSTOM)
                {
                    /*
                     * WHETHER IT IS STILL GOING IS THE WHOLE REPORT for this
                     * one. A custom scene is started once and never put back on
                     * a clock, so "it ended" is a real state and the only way
                     * to see it: nothing logs a loop reaching its last line.
                     */
                    bool running = false;
                    if (bot && bot->GetMap())
                        if (LuaScene* scene = bot->GetMap()->GetLuaScene())
                            running = scene->IsRunningFor(bot->GetObjectGuid(), rule.sceneId);

                    handler->PSendSysMessage("  custom -- scene %u, %s", rule.sceneId,
                                    !bot ? "not in world"
                                         : (running ? "running" : "NOT running -- it ended or never started"));
                }
                else if (i == BOT_NPC_IDLE_NEAR)
                    handler->PSendSysMessage("  near -- script %u, within %.0f yards, at most every %us",
                                    rule.scriptId, rule.radius, rule.period);
                else
                    handler->PSendSysMessage("  %s -- script %u, every %us", kindNames[i], rule.scriptId, rule.period);
            }

            handler->PSendSysMessage("Run one now: .npcbot idle %s <loop|near|after_talk|custom>", handle.c_str());
            return true;
        }

        uint32 trigger = BOT_NPC_IDLE_TRIGGERS;
        for (uint32 i = 0; i < BOT_NPC_IDLE_TRIGGERS; ++i)
            if (wanted == kindNames[i])
                trigger = i;

        if (trigger == BOT_NPC_IDLE_TRIGGERS)
        {
            handler->PSendSysMessage("No trigger called %s. It is one of loop, near, after_talk, custom.", wanted.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!bot)
        {
            handler->PSendSysMessage("%s is not in world. .npcbot place %s first.", name.c_str(), handle.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!rules || !(*rules)[trigger].Runs())
        {
            handler->PSendSysMessage("%s has no %s behaviour switched on.", name.c_str(), wanted.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        /*
         * CUSTOM IS A RESTART, NOT A RUN. It is already going, or it ended and
         * you want it back; either way the answer is stop and start, which is
         * exactly what a `reload bot_npc` does to it. Doing it by hand is the
         * fast loop while somebody is writing the scene.
         */
        if (trigger == BOT_NPC_IDLE_CUSTOM)
        {
            uint32 const sceneId = (*rules)[trigger].sceneId;

            auto data = LuaScene::Data();
            if (data->scripts.find(sceneId) == data->scripts.end())
            {
                handler->PSendSysMessage("Its custom behaviour points at dialog_lua %u, which is not loaded. "
                                "Run `reload dialog_scenes` from the console.", sceneId);
                handler->SetSentErrorMessage(true);
                return false;
            }

            if (LuaScene* scene = bot->GetMap()->GetLuaScene())
            {
                scene->StopFor(bot->GetObjectGuid(), sceneId);
                scene->Run(sceneId, data->scripts.at(sceneId), "custom", bot->GetObjectGuid(), ObjectGuid());
            }

            handler->PSendSysMessage("%s: restarted its custom behaviour (scene %u).", name.c_str(), sceneId);
            return true;
        }

        uint32 const scriptId = (*rules)[trigger].scriptId;
        if (sGenericScripts.find(scriptId) == sGenericScripts.end())
        {
            handler->PSendSysMessage("Its %s behaviour points at generic_scripts %u, which is not loaded. "
                            "Run `reload generic_scripts` from the console.", wanted.c_str(), scriptId);
            handler->SetSentErrorMessage(true);
            return false;
        }

        bot->GetMap()->ScriptsStart(sGenericScripts, scriptId, bot->GetObjectGuid(),
                                    trigger == BOT_NPC_IDLE_LOOP || !me
                                        ? ObjectGuid()
                                        : me->GetObjectGuid());

        handler->PSendSysMessage("%s: running its %s behaviour (script %u).", name.c_str(), wanted.c_str(), scriptId);
        return true;
    }

    // ------------------------------------------------------------ emote
    /*
     * WHAT IT ANSWERS AN EMOTE WITH, and running one without a second person.
     *
     * A separate subcommand rather than a fifth name under `idle`, because it
     * is a LIST: `.npcbot idle <name> emote` would have to ask which one, and a
     * subcommand that sometimes takes another argument is a subcommand people
     * get wrong. sql/custom/108.
     *
     * The bare form lists them. With a number it runs that rule NOW, ignoring
     * both the cooldown and busyUntil -- somebody typing this has asked for it,
     * and waiting out a sixty second cooldown to see whether the third line
     * reads right is the whole reason the command exists.
     */
    if (action == "emote")
    {
        BotNpcEmoteRules const* emotes = sBotNpcMgr.EmoteRules(entry.id);

        char* which = handler->ExtractLiteralArg(&args);
        std::string wanted = which ? which : "";

        if (wanted.empty())
        {
            if (!emotes || emotes->empty())
            {
                handler->PSendSysMessage("%s answers no emotes. Add one on /admin/npcbots/%u.", name.c_str(), entry.id);
                return true;
            }

            for (BotNpcEmoteRule const& r : *emotes)
                handler->PSendSysMessage("  emote %u -- script %u, at most every %us", r.emoteId, r.scriptId, r.period);

            handler->PSendSysMessage("Run one now: .npcbot emote %s <emote>", handle.c_str());
            return true;
        }

        uint32 const textEmote = uint32(atoi(wanted.c_str()));

        BotNpcEmoteRule const* match = nullptr;
        if (emotes)
            for (BotNpcEmoteRule const& r : *emotes)
                if (r.emoteId == textEmote)
                    match = &r;

        if (!match)
        {
            handler->PSendSysMessage("%s has no answer for emote %u.", name.c_str(), textEmote);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!bot)
        {
            handler->PSendSysMessage("%s is not in world. .npcbot place %s first.", name.c_str(), handle.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (sGenericScripts.find(match->scriptId) == sGenericScripts.end())
        {
            handler->PSendSysMessage("Its answer to emote %u points at generic_scripts %u, which is not loaded. "
                            "Run `reload generic_scripts` from the console.", textEmote, match->scriptId);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // THE TARGET IS YOU, which is the one the trigger would really pass: an
        // emote always has somebody behind it, unlike `loop`. A script written
        // against a target it will not have in the field should fail here too.
        bot->GetMap()->ScriptsStart(sGenericScripts, match->scriptId, bot->GetObjectGuid(),
                                    me ? me->GetObjectGuid() : ObjectGuid());

        handler->PSendSysMessage("%s: answering emote %u (script %u).", name.c_str(), textEmote, match->scriptId);
        return true;
    }

    // ------------------------------------------------------------ dress
    if (action == "dress")
    {
        if (!bot)
        {
            handler->PSendSysMessage("%s is not in world. .npcbot place %s first.", name.c_str(), handle.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        char* rest = handler->ExtractQuotedOrLiteralArg(&args);
        std::string given = rest ? rest : "";
        std::string query;

        /*
         * NO ARGUMENT MEANS THE LOOK ON THE DESIGN, which is the one the
         * dressing room wrote when it was opened "for this NPC". That is the
         * everyday case: change the outfit on the website, then one word in
         * game to see it. A saved-look id or a raw query string still work, and
         * BOTH are written back onto the design -- what an NPC is wearing and
         * what its design says it wears must not be allowed to drift.
         */
        if (given.empty())
        {
            query = entry.look;

            if (query.empty())
            {
                handler->PSendSysMessage("%s has no look yet. Dress it up on the website.", name.c_str());
                handler->SetSentErrorMessage(true);
                return false;
            }
        }
        else
        {
            bool isId = true;
            for (size_t i = 0; i < given.size(); ++i)
                if (!isdigit((unsigned char)given[i]))
                    isId = false;

            if (isId)
            {
                if (!FashionTableReachable())
                {
                    handler->SendSysMessage("Saved looks are not readable. Apply website/sql/007_fashion_grant.sql.");
                    handler->SetSentErrorMessage(true);
                    return false;
                }

                uint32 lookId = uint32(atoi(given.c_str()));

                if (QueryResult* result = LogsDatabase.PQuery(
                        "SELECT `look` FROM `%s`.`web_fashion` WHERE id = %u", FASHION_DB, lookId))
                {
                    query = result->Fetch()[0].GetCppString();
                    delete result;
                }

                if (query.empty())
                {
                    handler->PSendSysMessage("No saved look %u.", lookId);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
            }
            else
            {
                query = given;
            }
        }

        BotNpcLook look;
        if (!ParseLook(query, look))
        {
            handler->SendSysMessage("That does not read as a look.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string error;
        std::vector<std::string> skipped;

        if (!DressBotNpc(bot, look, error, skipped))
        {
            handler->PSendSysMessage("Cannot dress %s: %s.", name.c_str(), error.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        bot->SaveToDB();

        if (entry.look != query)
        {
            entry.look = query;
            sBotNpcMgr.Save(entry);
        }

        // What is on the character now. Without this the row still claims the
        // previous outfit is worn, and the next pass would dress it all over
        // again -- which is exactly the loop that orphaned items.
        sBotNpcMgr.MarkLookApplied(bot->GetGUIDLow(), query);

        // The look is the design's, so every other copy standing wears it too.
        // One not in world picks it up at login, since its look_applied now
        // differs from the design's look.
        for (Player* p : standing)
        {
            if (p == bot)
                continue;

            std::string otherError;
            std::vector<std::string> otherSkipped;
            if (DressBotNpc(p, look, otherError, otherSkipped))
            {
                p->SaveToDB();
                sBotNpcMgr.MarkLookApplied(p->GetGUIDLow(), query);
            }
        }

        if (skipped.empty())
        {
            handler->PSendSysMessage("%s is wearing it.", name.c_str());
        }
        else
        {
            handler->PSendSysMessage("%s is wearing it, except:", name.c_str());
            for (const auto& s : skipped)
                handler->PSendSysMessage("  %s", s.c_str());
        }

        return true;
    }

    /*
     * ------------------------------------------------- move, reset, save
     *
     * THREE VERBS FOR WHAT USED TO BE ONE, split 2026-09-16 when walking from
     * the post arrived. `.npcbot move` used to move AND save, which was fine
     * while the post only decided where an NPC stood after a restart. Now it is
     * also what every "Walks from its post" line and every WalkOffset is
     * measured from, so moving an NPC to look at something and saving its post
     * have to be separate decisions:
     *
     *   place   stand it here and make this its post (and make a body, the
     *           first time)
     *   move    bring it here for now. The post does not change, so its walks
     *           still measure from home and a restart puts it back
     *   reset   send it back to its post, facing the way the post faces
     *   save    make where it is standing NOW its post -- for when it has
     *           walked somewhere better than where it was put
     *
     * `move` and `save` differ in whose position they take: move uses yours,
     * save uses the NPC's.
     */
    if (action == "move" || action == "reset" || action == "save")
    {
        if (!needOneCopy())
            return false;

        bot = bodyOf(chosen);
        BotNpcSpawn post = *chosen;

        if (!bot || !bot->IsInWorld())
        {
            handler->PSendSysMessage("%s is not in world right now.", name.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (action == "save")
        {
            post.mapId = bot->GetMapId();
            post.x     = bot->GetPositionX();
            post.y     = bot->GetPositionY();
            post.z     = bot->GetPositionZ();
            // ITS OWN facing, not yours the way place takes it: this is the
            // NPC's pose being kept, and it is already facing the way it is
            // facing.
            post.o     = bot->GetOrientation();
            sBotNpcMgr.SaveSpawn(post);

            handler->PSendSysMessage("%s: this is its post now.", name.c_str());
            return true;
        }

        if (action == "move" && !me)
        {
            handler->SendSysMessage(".npcbot move brings it to you, so run it in game.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Settle it before moving it, for the same reason place does: stopping
        // the AI does not undo what it was in the middle of, and a walk still
        // queued would carry it straight back off the mark.
        bot->CombatStop(true);
        bot->GetMotionMaster()->Clear(false, true);
        bot->GetMotionMaster()->MoveIdle();

        if (action == "reset")
        {
            bot->TeleportTo(post.mapId, post.x, post.y, post.z, post.o);
            handler->PSendSysMessage("%s is back on its post.", name.c_str());
            return true;
        }

        // move: facing the way you face, the way place stands it, and nothing saved.
        bot->TeleportTo(me->GetMapId(), me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
                        me->GetOrientation());
        handler->PSendSysMessage("%s is here for now. .npcbot save keeps it, .npcbot reset sends it home.", name.c_str());
        return true;
    }

    // -------------------------------------------------------------- place
    if (!me)
    {
        handler->SendSysMessage(".npcbot place needs somewhere to stand it -- run it in game.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    /*
     * PLACING A DESIGN THAT HAS NO BODY MAKES ONE.
     *
     * This is the half the website cannot do. Creating a character means
     * allocating a guid, and guids are REUSED -- three crashes in this repo
     * came from rows outliving the character they named -- so it belongs in the
     * process that owns the allocator, not in a web request writing rows behind
     * its back.
     *
     * CreateBot is the companions' own machinery and takes the same key=value
     * string `.bot create` does; ChatHelper::parseRace and parseGender both
     * accept a bare number, so a design's race and gender go straight in.
     * Class is warrior and level 60 for no better reason than that an NPC never
     * fights or levels -- it wants a body that can hold armour, and a level 60
     * warrior can hold all of it.
     */
    BotNpcSpawn spawn;
    spawn.designId = entry.id;

    // On a SELECTED copy, place re-posts it where you stand, as it always did
    // for an NPC with one body. Anything else stands up a new copy: that is how
    // one design comes to be in every starting zone.
    if (chosen && chosenBySelection)
    {
        spawn = *chosen;
        bot = bodyOf(chosen);
    }
    else
    {
        bot = nullptr;

        /*
         * A CHARACTER OF THAT NAME ALREADY THERE IS ADOPTED, NOT REFUSED.
         *
         * The first version refused, and that made unplace a one-way door:
         * unplace keeps the character on purpose -- a design can be made from
         * somebody's real alt, and putting one away must never delete it -- so
         * the very next `place` found the name taken and had nothing to
         * suggest. "There is already a character called Orcy" is a true
         * sentence and a useless one when the Orcy in question is the body this
         * design was wearing five seconds ago.
         *
         * Adopting is safe because the website refuses to create a design whose
         * name matches an existing character (lib/npcbots.js). So a match here
         * is almost always our own former body -- and where it is not, adopting
         * is still what somebody typing `.npcbot place Marla` meant: there is a
         * Marla, stand her up.
         */
        /*
         * BUT NEVER ANOTHER DESIGN'S BODY. Two designs may share a name, and the
         * first one's character answers to that name too -- adopting it would
         * put two designs on one body, each re-dressing and re-posting it over
         * the other. A character some other design already wears is skipped,
         * and with nothing else to adopt this one gets a body of its own.
         */
        /*
         * AND NEVER A BODY ALREADY STANDING, this design's included. With
         * copies, the character of that name may be the copy in the next zone
         * over, and adopting it would move it here rather than add one. So
         * only a character that is nobody's spawn is up for adoption, which is
         * what an unplaced former body is.
         */
        auto adoptable = [](uint32 guidLow)
        {
            return sBotNpcMgr.GetSpawn(guidLow) == nullptr;
        };

        ObjectGuid existing = sObjectMgr.GetPlayerGuidByName(name);
        if (existing && !adoptable(existing.GetCounter()))
            existing = ObjectGuid();

        /*
         * AND ASK THE TABLE IF THE CACHE SAID NO. GetPlayerGuidByName goes
         * through normalizePlayerName, which caps at fifteen characters and
         * fails above it -- so a body called "Innkeeper Allison" is invisible
         * to it, placing would not adopt the character already standing there,
         * and CreateBot would make a SECOND one. That is how a stray "Help"
         * came to exist the first time round. Every row, not LIMIT 1: with a
         * twin standing somewhere the first row may well be its body.
         */
        if (!existing)
        {
            std::string safe = name;
            CharacterDatabase.escape_string(safe);

            if (QueryResult* result = CharacterDatabase.PQuery(
                    "SELECT guid FROM characters WHERE name = '%s' ORDER BY guid", safe.c_str()))
            {
                do
                {
                    uint32 guidLow = result->Fetch()[0].GetUInt32();
                    if (adoptable(guidLow))
                    {
                        existing = ObjectGuid(HIGHGUID_PLAYER, guidLow);
                        break;
                    }
                } while (result->NextRow());
                delete result;
            }
        }

        if (existing)
        {
            spawn.characterGuid = existing.GetCounter();
            bot = sObjectMgr.GetPlayer(existing, false);

            handler->PSendSysMessage("Using the character %s that is already there.", name.c_str());
        }
        else
        {
            /*
             * CREATEBOT CANNOT BE TOLD A NAME WITH A SPACE IN IT. Its arguments
             * are one space-separated key=value string, so `name=Helper Elowe`
             * splits and it makes a character called "Helper", silently
             * dropping the rest -- exactly what happened the first time, which
             * left a stray body standing about.
             *
             * So it gets the name with the spaces taken out, and the design's
             * real name goes on immediately afterwards. A bot NPC's name is
             * whatever its row says; the character is only ever wearing it.
             */
            std::string createName = name;
            createName.erase(std::remove(createName.begin(), createName.end(), ' '), createName.end());

            /*
             * A TWIN CANNOT BE CREATED UNDER ITS OWN NAME: CreateBot refuses a
             * name any character already has, and the first design's body has
             * it. So it is born under a random bot name -- valid by
             * construction -- and renamed below like every other body.
             */
            {
                std::string safe = createName;
                CharacterDatabase.escape_string(safe);
                if (QueryResult* result = CharacterDatabase.PQuery(
                        "SELECT 1 FROM characters WHERE name = '%s' LIMIT 1", safe.c_str()))
                {
                    delete result;
                    createName.clear();
                }
            }

            std::ostringstream param;
            if (!createName.empty())
                param << "name=" << createName << " ";
            param << "race=" << entry.race
                  << " gender=" << entry.gender
                  << " class=1 level=60"
                  // Born without the warrior kit, so the login pass dresses a
                  // bare body and nobody sees the shield come off.
                  << " naked=1";

            std::list<std::string> messages;
            ObjectGuid made;
            sRandomPlayerbotMgr.CreateBot(nullptr, param.str(), messages, made);

            if (!made)
            {
                handler->PSendSysMessage("Could not make a character for %s:", name.c_str());
                for (const auto& m : messages)
                    handler->PSendSysMessage("  %s", m.c_str());
                handler->SetSentErrorMessage(true);
                return false;
            }

            spawn.characterGuid = made.GetCounter();
            bot = sObjectMgr.GetPlayer(made, false);

            // And put the real name on, cache included -- GetPlayerGuidByName
            // answers from that cache, and a body findable only by its stripped
            // name is a body the next `place` will not adopt.
            // Read back what it was really born as: a twin's name was picked
            // by CreateBot, not by us.
            std::string bornAs = createName;
            if (PlayerCacheData const* data = sObjectMgr.GetPlayerDataByGUID(spawn.characterGuid))
                bornAs = data->sName;
            BotNpc_RenameCharacter(spawn.characterGuid, bot, bornAs, name);

            handler->PSendSysMessage("Made a body for %s.", name.c_str());
        }
    }

    if (!entry.npcFlags)
        entry.npcFlags = UNIT_NPC_FLAG_GOSSIP;

    spawn.mapId = me->GetMapId();
    spawn.x     = me->GetPositionX();
    spawn.y     = me->GetPositionY();
    spawn.z     = me->GetPositionZ();
    // Facing the way the placer faces. This used to add a half-turn on the
    // belief that GetOrientation is the direction you look FROM; it is the
    // direction you look, so every NPC stood facing backwards. Stand where it
    // should be and look where it should look.
    spawn.o     = me->GetOrientation();

    sBotNpcMgr.Save(entry);
    sBotNpcMgr.SaveSpawn(spawn);

    /*
     * IT APPEARS NOW; the ROW is what makes it survive a restart.
     *
     * This used to also write always=ACTIVE on the bot event store and rely on
     * that to bring it back. It never worked once: RandomPlayerbotMgr::Remove
     * deletes every event row for a bot on its way out, so the flag was gone
     * before the next startup could read it, and no startup ever logged
     * "Enabling always online" for a bot NPC. PlayerbotAIConfig::loadBotNpcs
     * reads tw_char.bot_npc instead, which this feature owns.
     *
     * So the push below is only about NOW -- it saves waiting for a restart.
     */
    uint32 accountId = sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER, spawn.characterGuid));

    uint32 wantGuid = spawn.characterGuid;
    bool queued = std::find_if(sPlayerbotAIConfig.freeAltBots.begin(), sPlayerbotAIConfig.freeAltBots.end(),
                               [wantGuid](std::pair<uint32, uint32> i) { return i.second == wantGuid; })
                  != sPlayerbotAIConfig.freeAltBots.end();

    if (!queued)
        sPlayerbotAIConfig.freeAltBots.push_back(std::make_pair(accountId, wantGuid));

    char const* saySomething = entry.gossipMenuId ? "" : " -- it has nothing to say yet";

    if (bot)
    {
        // Settle it before moving it. The AI stops the moment the design says
        // it is an NPC, but stopping the AI does not undo what it was in the
        // middle of: a bot caught mid-fight would arrive still swinging, with a
        // target nothing will now release, because the code that would have
        // released it is the code that just stopped.
        bot->CombatStop(true);
        bot->GetMotionMaster()->Clear(false, true);
        bot->GetMotionMaster()->MoveIdle();

        if (PlayerbotAI* botAi = GetBotAI(bot))
            botAi->Reset(true);

        bot->SetUInt32Value(UNIT_NPC_FLAGS, entry.npcFlags);
        bot->SetBotNpcGossipMenuId(entry.gossipMenuId);
        BotNpc_ApplySubname(bot, entry.subname);
        bot->TeleportTo(spawn.mapId, spawn.x, spawn.y, spawn.z, spawn.o);

        // Dress it on the way in, so placing a design is one step and not two.
        if (!entry.look.empty())
        {
            BotNpcLook look;
            std::string error;
            std::vector<std::string> skipped;

            if (ParseLook(entry.look, look))
            {
                if (DressBotNpc(bot, look, error, skipped))
                    sBotNpcMgr.MarkLookApplied(spawn.characterGuid, entry.look);
                else
                    handler->PSendSysMessage("  (could not dress it: %s)", error.c_str());
            }
        }

        bot->SaveToDB();

        handler->PSendSysMessage("%s is standing here%s.", name.c_str(), saySomething);
    }
    else
    {
        // LoginFreeBots picks it up on the random manager's next pass, and
        // Player::LoadFromDB puts it on its mark -- so no teleport here.
        handler->PSendSysMessage("%s will be along shortly, and stands here from now on%s.",
                        name.c_str(), saySomething);
    }

    uint32 const copies = uint32(sBotNpcMgr.SpawnsOf(entry.id).size());
    if (copies > 1)
        handler->PSendSysMessage("%s now stands in %u places. Select one to move or unplace it.", name.c_str(), copies);

    return true;
}


//Dummy handler until Chat.h can be modified.
bool ChatHandler::HandleAhBotCommand(char* args)
{
    return false;
}

float PlayerbotAI::GetRange(std::string type)
{
    float val = 0;

    if (type == "follow" && bot->GetGroup() && bot->GetGroup()->IsRaidGroup())
        type = "followraid";

    if (aiObjectContext) val = aiObjectContext->GetValue<float>("range", type)->Get();
    if (abs(val) >= 0.1f) return val;

    if (type == "spell") return sPlayerbotAIConfig.spellDistance;
    if (type == "shoot") return sPlayerbotAIConfig.shootDistance;
    if (type == "flee") return sPlayerbotAIConfig.fleeDistance;
    if (type == "heal") return sPlayerbotAIConfig.healDistance;
    if (type == "follow") return sPlayerbotAIConfig.followDistance;
    if (type == "guard") return sPlayerbotAIConfig.sightDistance;
    if (type == "followraid") return sPlayerbotAIConfig.raidFollowDistance;
    if (type == "wandermin") return sPlayerbotAIConfig.wanderMinDistance;
    if (type == "wandermax") return sPlayerbotAIConfig.wanderMaxDistance;
    if (type == "attack") return 0;
    return 0;
}

//Copy from reputation GetFactionReaction
ReputationRank PlayerbotAI::GetFactionReaction(FactionTemplateEntry const* thisTemplate, FactionTemplateEntry const* otherTemplate)
{
    MANGOS_ASSERT(thisTemplate)
        MANGOS_ASSERT(otherTemplate)

        // Original logic begins

        if (otherTemplate->factionGroupMask & thisTemplate->enemyGroupMask)
            return REP_HOSTILE;

    if (thisTemplate->enemyFaction[0] && otherTemplate->faction)
    {
        for (unsigned int i : thisTemplate->enemyFaction)
        {
            if (i == otherTemplate->faction)
                return REP_HOSTILE;
        }
    }

    if (otherTemplate->factionGroupMask & thisTemplate->friendGroupMask)
        return REP_FRIENDLY;

    if (thisTemplate->friendFaction[0] && otherTemplate->faction)
    {
        for (unsigned int i : thisTemplate->friendFaction)
        {
            if (i == otherTemplate->faction)
                return REP_FRIENDLY;
        }
    }

    if (thisTemplate->factionGroupMask & otherTemplate->friendGroupMask)
        return REP_FRIENDLY;

    if (otherTemplate->friendFaction[0] && thisTemplate->faction)
    {
        for (unsigned int i : otherTemplate->friendFaction)
        {
            if (i == thisTemplate->faction)
                return REP_FRIENDLY;
        }
    }
    return REP_NEUTRAL;
}

bool PlayerbotAI::AddAura(Unit* unit, uint32 spellId)
{
    // number or [name] Shift-click form |color|Hspell:spell_id|h[name]|h|r or Htalent form

    SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellId);
    if (!spellInfo)
        return false;

    if (!IsSpellAppliesAura(spellInfo, (1 << EFFECT_INDEX_0) | (1 << EFFECT_INDEX_1) | (1 << EFFECT_INDEX_2)) &&
        !IsSpellHaveEffect(spellInfo, SPELL_EFFECT_PERSISTENT_AREA_AURA))
    {
        return false;
    }

    SpellAuraHolder* holder = CreateSpellAuraHolder(spellInfo, unit, unit, unit);

    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        uint8 eff = spellInfo->Effect[i];
        if (eff >= MAX_SPELL_EFFECTS)
            continue;
        if (IsAreaAuraEffect(eff) ||
            eff == SPELL_EFFECT_APPLY_AURA ||
            eff == SPELL_EFFECT_PERSISTENT_AREA_AURA)
        {
            int32 basePoints = spellInfo->CalculateSimpleValue(SpellEffectIndex(i));
            int32 damage = basePoints;
            Aura* aur = CreateAura(spellInfo, SpellEffectIndex(i), &damage, holder, unit);
            holder->AddAura(aur, SpellEffectIndex(i));
        }
    }
    if (!unit->AddSpellAuraHolder(holder))
        holder = nullptr;

    return true;
}

void PlayerbotAI::InventoryIterateItems(IterateItemsVisitor* visitor, IterateItemsMask mask)
{
    if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BAGS)
        InventoryIterateItemsInBags(visitor);

    if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_EQUIP)
        InventoryIterateItemsInEquip(visitor);

    if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BANK)
        InventoryIterateItemsInBank(visitor);

    if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BUYBACK)
        InventoryIterateItemsInBuyBack(visitor);
}

std::vector<Bag*> PlayerbotAI::GetEquippedAnyBags()
{
    std::vector<Bag*> bags;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            bags.push_back(bag);
        }
    }

    return bags;
}

std::vector<Bag*> PlayerbotAI::GetEquippedQuivers()
{
    std::vector<Bag*> bags;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (bag->GetProto()->Class == ITEM_CLASS_QUIVER)
            {
                bags.push_back(bag);
            }
        }
    }

    return bags;
}

std::vector<Item*> PlayerbotAI::GetInventoryAndEquippedItems()
{
    std::vector<Item*> items;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    items.push_back(pItem);
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; slot++)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            items.push_back(pItem);
        }
    }

    return items;
}

std::vector<Item*> PlayerbotAI::GetInventoryItems()
{
    std::vector<Item*> items;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    items.push_back(pItem);
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            items.push_back(pItem);
        }
    }

    return items;
}

uint32 PlayerbotAI::GetInventoryItemsCountWithId(uint32 itemId)
{
    uint32 count = 0;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    if (pItem->GetProto()->ItemId == itemId)
                    {
                        count += pItem->GetCount();
                    }
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->ItemId == itemId)
            {
                count += pItem->GetCount();
            }
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->ItemId == itemId)
            {
                count += pItem->GetCount();
            }
        }
    }

    return count;
}

bool PlayerbotAI::HasItemInInventory(uint32 itemId)
{

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            {
                if (Item* pItem = pBag->GetItemByPos(j))
                {
                    if (pItem->GetProto()->ItemId == itemId)
                    {
                        return true;
                    }
                }
            }
        }
    }

    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->ItemId == itemId)
            {
                return true;
            }
        }
    }

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem->GetProto()->ItemId == itemId)
            {
                return true;
            }
        }
    }

    return false;
}

/*
* @return true if has stacks which are not full for these items
*/
bool PlayerbotAI::HasNotFullStacksInBagsForLootItems(LootItemList &lootItemList)
{
    for (auto lootItem : lootItemList)
    {
        for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* pItem = pBag->GetItemByPos(j))
                    {
                        if (pItem->GetProto()->ItemId == lootItem.itemId
                            && pItem->GetCount() < pItem->GetMaxStackCount())
                        {
                            return true;
                        }
                    }
                }
            }
        }

        for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (pItem->GetProto()->ItemId == lootItem.itemId
                    && pItem->GetCount() < pItem->GetMaxStackCount())
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool PlayerbotAI::HasQuestItemsInWOLootList(WorldObject* wo)
{
    if (!wo)
        return false;

    LootItemList lootItemList = {};

    // Penqle's loot is on Creature/GameObject; dispatch via cast.
    Loot* woLoot = nullptr;
    if (wo->IsCreature()) woLoot = ((Creature*)wo)->m_loot;
    else if (wo->IsGameObject()) woLoot = ((GameObject*)wo)->m_loot;
    if (woLoot)
        woLoot->GetLootItemsListFor(bot, lootItemList);

    if (HasQuestItemsInLootList(lootItemList))
    {
        return true;
    }

    return false;
}

bool PlayerbotAI::HasQuestItemsInLootList(LootItemList &lootItemList)
{
    for (auto lootItem : lootItemList)
    {
        if (lootItem.lootItemType == LOOTITEM_TYPE_QUEST)
        {
            return true;
        }
    }

    return false;
}

bool PlayerbotAI::CanLootSomethingFromWO(WorldObject* wo)
{
    if (!wo)
        return false;

    ObjectGuid guid = wo->GetObjectGuid();
    if (guid.IsCreature())
    {
        Creature* creature = GetCreature(guid);
        if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
        {
            if (creature->m_loot->GetGoldAmount() > 0)
            {
                return true;
            }
            LootItemList lootItemList = {};

            if (creature->m_loot)
                creature->m_loot->GetLootItemsListFor(bot, lootItemList);

            if (HasNotFullStacksInBagsForLootItems(lootItemList))
            {
                return true;
            }
        }
    }
    else if (wo->IsGameObject())
    {
        GameObject* go = GetGameObject(guid);
        if (go)
        {
            LootItemList lootItemList = {};

            if (go->m_loot)
                go->m_loot->GetLootItemsListFor(bot, lootItemList);

            if (HasNotFullStacksInBagsForLootItems(lootItemList))
            {
                return true;
            }
        }
    }

    return false;
}

void PlayerbotAI::InventoryIterateItemsInBags(IterateItemsVisitor* visitor)
{
    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            if (!visitor->Visit(pItem))
                return;

    for (int i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            if (!visitor->Visit(pItem))
                return;

    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                if (Item* pItem = pBag->GetItemByPos(j))
                    if (!visitor->Visit(pItem))
                        return;
}

void PlayerbotAI::InventoryIterateItemsInEquip(IterateItemsVisitor* visitor)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; slot++)
    {
        Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!pItem)
            continue;

        if (!visitor->Visit(pItem))
            return;
    }
}

void PlayerbotAI::InventoryIterateItemsInBank(IterateItemsVisitor* visitor)
{
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; slot++)
    {
        Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!pItem)
            continue;

        if (!visitor->Visit(pItem))
            return;
    }

    for (int i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pBag)
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* pItem = pBag->GetItemByPos(j))
                    {
                        if (!pItem)
                            continue;

                        if (!visitor->Visit(pItem))
                            return;
                    }
                }
            }
        }
    }

}

void PlayerbotAI::InventoryIterateItemsInBuyBack(IterateItemsVisitor* visitor)
{
    for (uint8 slot = BUYBACK_SLOT_START; slot < BUYBACK_SLOT_END; slot++)
    {
        Item* pItem = bot->GetItemFromBuyBackSlot(slot);
        if (!pItem)
            continue;

        if (!visitor->Visit(pItem))
            return;
    }
}

bool compare_items(const ItemPrototype* proto1, const ItemPrototype* proto2)
{
    if (proto1->Class != proto2->Class)
        return proto1->Class > proto2->Class;

    if (proto1->SubClass != proto2->SubClass)
        return proto1->SubClass < proto2->SubClass;

    if (proto1->Quality != proto2->Quality)
        return proto1->Quality < proto2->Quality;

    if (proto1->ItemLevel != proto2->ItemLevel)
        return proto1->ItemLevel > proto2->ItemLevel;

    return false;
}

bool compare_items_by_level(const Item* item1, const Item* item2)
{
    return compare_items(item1->GetProto(), item2->GetProto());
}

void PlayerbotAI::InventoryTellItems(Player* player, std::map<uint32, int> itemMap, std::map<uint32, bool> soulbound)
{
    std::list<ItemPrototype const*> items;
    for (std::map<uint32, int>::iterator i = itemMap.begin(); i != itemMap.end(); i++)
    {
        items.push_back(sObjectMgr.GetItemPrototype(i->first));
    }

    items.sort(compare_items);

    uint32 oldClass = -1;
    for (std::list<ItemPrototype const*>::iterator i = items.begin(); i != items.end(); i++)
    {
        ItemPrototype const* proto = *i;

        if (proto->Class != oldClass)
        {
            oldClass = proto->Class;
            switch (proto->Class)
            {
            case ITEM_CLASS_CONSUMABLE:
                TellPlayer(player, "--- consumable ---");
                break;
            case ITEM_CLASS_CONTAINER:
                TellPlayer(player, "--- container ---");
                break;
            case ITEM_CLASS_WEAPON:
                TellPlayer(player, "--- weapon ---");
                break;
            case ITEM_CLASS_ARMOR:
                TellPlayer(player, "--- armor ---");
                break;
            case ITEM_CLASS_REAGENT:
                TellPlayer(player, "--- reagent ---");
                break;
            case ITEM_CLASS_PROJECTILE:
                TellPlayer(player, "--- projectile ---");
                break;
            case ITEM_CLASS_TRADE_GOODS:
                TellPlayer(player, "--- trade goods ---");
                break;
            case ITEM_CLASS_RECIPE:
                TellPlayer(player, "--- recipe ---");
                break;
            case ITEM_CLASS_QUIVER:
                TellPlayer(player, "--- quiver ---");
                break;
            case ITEM_CLASS_QUEST:
                TellPlayer(player, "--- quest items ---");
                break;
            case ITEM_CLASS_KEY:
                TellPlayer(player, "--- keys ---");
                break;
            case ITEM_CLASS_MISC:
                TellPlayer(player, "--- other ---");
                break;
            }
        }

        InventoryTellItem(player, proto, itemMap[proto->ItemId], soulbound[proto->ItemId]);
    }
}

void PlayerbotAI::InventoryTellItem(Player* player, ItemPrototype const* proto, int count, bool soulbound)
{
    std::ostringstream out;
    out << GetChatHelper()->formatItem(proto, count);
    if (soulbound)
        out << " (soulbound)";
    TellPlayer(player, out.str());
}

#define VISIT_MASK(visitmask) \
    InventoryIterateItems(&visitor, visitmask);\
    found.insert(visitor.GetResult().begin(), visitor.GetResult().end())

#define VISIT \
    InventoryIterateItems(&visitor, mask);\
    found.insert(visitor.GetResult().begin(), visitor.GetResult().end())

#define RETURN_SORT_FOUND \
    std::list<Item*> result; \
    for (std::set<Item*>::iterator i = found.begin(); i != found.end(); ++i)\
        result.push_back(*i); \
    result.sort(compare_items_by_level); \
    return result

#define RETURN_FOUND \
    std::list<Item*> result; \
    for (std::set<Item*>::iterator i = found.begin(); i != found.end(); ++i)\
        result.push_back(*i); \
    return result


std::list<Item*> PlayerbotAI::InventoryParseItems(std::string text, IterateItemsMask mask)
{
    if (text.empty())
    {
        std::list<Item*> result;
        return result;
    }

    AiObjectContext* context = aiObjectContext;

    std::set<Item*> found;
    size_t pos = text.find(" ");
    int count = pos != std::string::npos ? atoi(text.substr(pos + 1).c_str()) : 999;

    //Look for item id's in the command.
    ItemIds ids = GetChatHelper()->parseItems(text);
    if (!ids.empty() && text.find("usage ") == std::string::npos)
    {
        for (ItemIds::iterator i = ids.begin(); i != ids.end(); i++)
        {
            FindItemByIdVisitor visitor(*i);
            VISIT;
        }

        //We want to stop looking if we found items from links or ids. However if the command is like "all 3" itemId 3 will be found. If so keep looking for more items.
        if (text.find("Hfound:") != -1 || text.find("Hitem:") != -1 || pos == std::string::npos)
        {
            RETURN_SORT_FOUND;
        }
    }

    if (text == "all" || text == "*")
    {
        FindAllItemVisitor visitor;
        VISIT_MASK(mask);
        RETURN_SORT_FOUND;
    }
    else if (text == "equip")
    {
        if (!((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_EQUIP))
            return {};

        FindAllItemVisitor visitor;
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_EQUIP);
        RETURN_SORT_FOUND;
    }
    else if (text == "inventory")
    {
        if (!((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BAGS))
            return {};

        FindAllItemVisitor visitor;
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
        RETURN_SORT_FOUND;
    }
    else if (text == "bank")
    {
        if (!((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BANK))
            return {};

        FindAllItemVisitor visitor;
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BANK);
        RETURN_SORT_FOUND;
    }
    else if (text == "buyback")
    {
        if (!((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BUYBACK))
            return {};

        FindAllItemVisitor visitor;
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BUYBACK);
        RETURN_SORT_FOUND;
    }
    else if (text == "food" || text == "conjured food")
    {
        FindFoodVisitor visitor(bot, 11, (text == "conjured food"));
        VISIT;
    }
    else if (text == "drink" || text == "water" || text == "conjured drink" || text == "conjured water")
    {
        FindFoodVisitor visitor(bot, 59, (text == "conjured drink" || text == "conjured water"));
        VISIT;
    }
    else if (text == "mana potion")
    {
        FindPotionVisitor visitor(bot, SPELL_EFFECT_ENERGIZE);
        VISIT;
    }
    else if (text == "healing potion")
    {
        FindPotionVisitor visitor(bot, SPELL_EFFECT_HEAL);
        VISIT;
    }
    else if (text == "mount")
    {
        FindMountVisitor visitor(bot);
        VISIT;
    }
    else if (text == "pet")
    {
        FindPetVisitor visitor(bot);
        VISIT;
    }
    else if (text == "ammo")
    {
        Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (pItem)
        {
            FindAmmoVisitor visitor(bot, pItem->GetProto()->SubClass);
            VISIT;
        }
    }
    else if (text == "recipe")
    {
        FindRecipeVisitor visitor(bot);
        VISIT;
    }
    else if (text == "open")
    {
        FindOpenItemVisitor visitor(bot);
        VISIT;
    }
    else if (text == "quest")
    {
        FindQuestItemVisitor visitor(bot);
        VISIT;
    }
    else if (text.find("usage ") == 0)
    {
        FindItemUsageVisitor visitor(bot, ItemUsage(stoi(text.substr(6))));
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    }
    else if (text == "tradeskill")
    {
        FindItemUsageVisitor visitor(bot, ItemUsage::ITEM_USAGE_SKILL);
        VISIT;
    }
    else if (text == "use")
    {
        FindItemUsageVisitor visitor(bot, ItemUsage::ITEM_USAGE_USE);
        VISIT;
    }
    else if (text == "ah")
    {
        FindItemUsageVisitor visitor(bot, ItemUsage::ITEM_USAGE_AH);
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);

        visitor = FindItemUsageVisitor(bot, ItemUsage::ITEM_USAGE_BROKEN_AH);
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    }
    else if (text == "vendor")
    {
        bool vendorAH = AI_VALUE(uint8, "bag space") > 80 && !urand(0, 10);
        FindVendorItemsVisitor visitor(bot, vendorAH);
        VISIT_MASK(IterateItemsMask::ITERATE_ITEMS_IN_BAGS);      
    }

    uint32 quality = GetChatHelper()->parseItemQuality(text);
    if (quality != MAX_ITEM_QUALITY)
    {
        FindItemsByQualityVisitor visitor(quality, count);
        VISIT;
    }

    uint32 itemClass = MAX_ITEM_CLASS, itemSubClass = 0;
    GetChatHelper()->parseItemClass(text, &itemClass, &itemSubClass);
    if (itemClass != MAX_ITEM_CLASS)
    {
        FindItemsByClassVisitor visitor(itemClass, itemSubClass);
        VISIT;
    }

    uint32 fromSlot = GetChatHelper()->parseSlot(text);
    if (fromSlot != EQUIPMENT_SLOT_END)
    {
        if ((uint8)mask & (uint8)IterateItemsMask::ITERATE_ITEMS_IN_EQUIP)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, fromSlot);
            if (item)
                found.insert(item);
        }

        FindItemBySlotVisitor visitor(bot, fromSlot);
        VISIT;
    }

    ItemIds outfit = InventoryFindOutfitItems(text);
    if (!outfit.empty())
    {
        FindItemByIdsVisitor visitor(outfit);
        VISIT;
    }

    if (found.size() == 0 && quality == MAX_ITEM_QUALITY && itemClass == MAX_ITEM_CLASS && fromSlot == EQUIPMENT_SLOT_END && outfit.empty())
    {
        FindNamedItemVisitor visitor(bot, text);
        VISIT;
    }

    RETURN_SORT_FOUND;
}

uint32 PlayerbotAI::InventoryGetItemCount(FindItemVisitor* visitor, IterateItemsMask mask)
{
    InventoryIterateItems(visitor, mask);
    uint32 count = 0;
    std::list<Item*>& items = visitor->GetResult();
    for (std::list<Item*>::iterator i = items.begin(); i != items.end(); ++i)
    {
        Item* item = *i;
        count += item->GetCount();
    }
    return count;
}

ItemIds PlayerbotAI::InventoryFindOutfitItems(std::string name)
{
    AiObjectContext* context = GetAiObjectContext();
    std::list<std::string>& outfits = AI_VALUE(std::list<std::string>&, "outfit list");
    for (std::list<std::string>::iterator i = outfits.begin(); i != outfits.end(); ++i)
    {
        std::string outfit = *i;
        if (name == InventoryParseOutfitName(outfit))
            return InventoryParseOutfitItems(outfit);
    }
    return std::set<uint32>();
}

void PlayerbotAI::AccelerateRespawn(Creature* creature, float accelMod)
{
    if (!creature)
        return;

    if (!sPlayerbotAIConfig.respawnModForPlayerBots && HasRealPlayerMaster())
        return;

    if (!sPlayerbotAIConfig.respawnModForInstances && !WorldPosition(creature).isOverworld())
        return;

    AiObjectContext* context = aiObjectContext;
    if (!accelMod)
    {
        if (!sPlayerbotAIConfig.respawnModHostile && !sPlayerbotAIConfig.respawnModNeutral)
            return;

        uint32 playersNr = AI_VALUE_LAZY(std::list<ObjectGuid>, "nearest friendly players").size()+1;


        if (playersNr <= sPlayerbotAIConfig.respawnModThreshold)
            return;

        playersNr = std::min(playersNr - sPlayerbotAIConfig.respawnModThreshold, sPlayerbotAIConfig.respawnModMax);

        accelMod = playersNr * (creature->CanAttackOnSight(bot) ? sPlayerbotAIConfig.respawnModHostile : sPlayerbotAIConfig.respawnModNeutral) * 0.01f;
    }

    if (!accelMod)
        return;

    uint32 m_respawnDelay;
    uint32 m_corpseAccelerationDecayDelay;

    if (accelMod >= 1)
    {
        m_respawnDelay = 0;
        m_corpseAccelerationDecayDelay = 0;
    }
    else
    {
        CreatureData const* data = sObjectMgr.GetCreatureData(creature->GetDbGuid());

        if (!data)
            return;

        m_respawnDelay = data->GetRandomRespawnTime() * IN_MILLISECONDS;
        m_corpseAccelerationDecayDelay = MINIMUM_LOOTING_TIME;

        uint32 totalDelay = m_respawnDelay + m_corpseAccelerationDecayDelay;

        if (m_respawnDelay < totalDelay * accelMod)
        {
            if ((m_corpseAccelerationDecayDelay- ((totalDelay * accelMod) - m_respawnDelay)) < 0)
                sLog.outError("m_corpseAccelerationDecayDelay: %d, totalDelay: %d, accelMod: %f, m_respawnDelay: %d", m_corpseAccelerationDecayDelay, totalDelay, accelMod, m_respawnDelay);

            m_corpseAccelerationDecayDelay -= (totalDelay * accelMod) - m_respawnDelay;
            m_respawnDelay = 0;
        }
        else
            m_respawnDelay -= totalDelay * accelMod;
    }

    creature->SetRespawnDelay(m_respawnDelay / IN_MILLISECONDS,true);

    if (!m_corpseAccelerationDecayDelay && creature->m_loot)
    {
        // LootAccess wraps Loot* now.
        LootAccess lootAccess(creature->m_loot);

        if (lootAccess.IsLootedForAll()) //No loot left. Just despawn the corpse.
        {
            creature->RemoveCorpse();
            return;
        }

        uint32 defaultDelay = 2 * MINUTE;

        CreatureInfo const* cinfo = creature->GetCreatureInfo();

        if (cinfo->CorpseDelay)
            defaultDelay = cinfo->CorpseDelay;
        else if (sObjectMgr.IsEncounter(creature->GetEntry(), creature->GetMapId()))
        {
            // encounter boss forced decay timer to 1h
            defaultDelay = 3600;                               // TODO: maybe add that to config file
        }
        else
        {
            switch (cinfo->Rank)
            {
            case CREATURE_ELITE_RARE:
                defaultDelay = sWorld.getConfig(CONFIG_UINT32_CORPSE_DECAY_RARE);
                break;
            case CREATURE_ELITE_ELITE:
                defaultDelay = sWorld.getConfig(CONFIG_UINT32_CORPSE_DECAY_ELITE);
                break;
            case CREATURE_ELITE_RAREELITE:
                defaultDelay = sWorld.getConfig(CONFIG_UINT32_CORPSE_DECAY_RAREELITE);
                break;
            case CREATURE_ELITE_WORLDBOSS:
                defaultDelay = sWorld.getConfig(CONFIG_UINT32_CORPSE_DECAY_WORLDBOSS);
                break;
            default:
                defaultDelay = sWorld.getConfig(CONFIG_UINT32_CORPSE_DECAY_NORMAL);
                break;
            }
        }

        defaultDelay *= static_cast<uint32>(IN_MILLISECONDS) / (1+accelMod);

        //We will decrease the loot time by a factor capping at 20 seconds.
        m_corpseAccelerationDecayDelay = std::max(uint32(20 * static_cast<uint32>(IN_MILLISECONDS)), defaultDelay);
        creature->SetCorpseAccelerationDelay(m_corpseAccelerationDecayDelay);

        creature->ReduceCorpseDecayTimer();
        return;
    }
    MANGOS_ASSERT(m_corpseAccelerationDecayDelay < 24 * HOUR * static_cast<uint32>(IN_MILLISECONDS));
    creature->SetCorpseAccelerationDelay(m_corpseAccelerationDecayDelay);
}

std::list<Unit*> PlayerbotAI::GetAllHostileUnitsAroundWO(WorldObject* wo, float distanceAround)
{
    std::list<Unit*> hostileUnits;
    // Search around the world object `wo` (e.g. a lootable corpse) but judge hostility from
    // the BOT's perspective. The 2-arg ctor used the object itself as the friendly-reference,
    // so for a creature corpse it asked "is the dead mob friendly to u?" (flagging neutral
    // critters and even friendly guards as hostile), and for a non-Unit object (GameObject)
    // it set the reference to nullptr and crashed. Passing `bot` fixes both.
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(wo, bot, distanceAround);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(hostileUnits, u_check);
    Cell::VisitAllObjects(wo, searcher, distanceAround);

    // AnyUnfriendlyUnitInObjectRangeCheck uses !IsFriendlyTo (reaction < REP_FRIENDLY), which
    // includes neutral critters and wolves. Strip them out — only units the bot is truly hostile
    // to (reaction <= REP_HOSTILE) are real threats that should block looting.
    hostileUnits.remove_if([this](Unit* u) { return !bot->IsHostileTo(u); });

    return hostileUnits;
}

std::list<Unit*> PlayerbotAI::GetAllHostileNPCNonPetUnitsAroundWO(WorldObject* wo, float distanceAround)
{
    std::list<Unit*> hostileUnitsNonPlayers;
    for (auto hostileUnit : GetAllHostileUnitsAroundWO(wo, distanceAround))
    {
        if (!hostileUnit->IsPlayer() && !hostileUnit->IsCorpse())
        {
            if (hostileUnit->IsCreature())
            {
                Creature* creature = GetCreature(hostileUnit->GetObjectGuid());
                if (!creature || (creature && creature->IsPet()))
                {
                    continue;
                }
            }

            hostileUnitsNonPlayers.push_back(hostileUnit);
        }
    }

    return hostileUnitsNonPlayers;
}

void PlayerbotAI::SendDelayedPacket(WorldSession* session, futurePackets futPackets)
{
    std::thread t([session, futPacket = std::move(futPackets)]() mutable {
        for (auto& delayedPacket : futPacket.get())
        {
            if (delayedPacket.second)
                std::this_thread::sleep_for(std::chrono::milliseconds(delayedPacket.second));

            std::unique_ptr<WorldPacket> packetPtr(new WorldPacket(delayedPacket.first));
            session->QueuePacket(std::move(packetPtr));
        }
    });

    t.detach();
}

void PlayerbotAI::ReceiveDelayedPacket(futurePackets futPackets)
{
    PacketHandlingHelper* handler = &botOutgoingPacketHandlers;
    std::thread t([handler, futPackets = std::move(futPackets)]() mutable {
        for (auto& delayedPacket : futPackets.get())
        {            
            handler->AddPacket(delayedPacket.first);
            if(delayedPacket.second)
                std::this_thread::sleep_for(std::chrono::milliseconds(delayedPacket.second));
        }
        });

    t.detach();
}

PlayerbotHolder* PlayerbotAI::GetHolder() const
{
    if (sRandomPlayerbotMgr.IsRandomBot(bot))
        return &sRandomPlayerbotMgr;

    if (bot->GetMaster())
        return GetBotMgr(static_cast<Player*>(bot->GetMaster()));

    return GetBotMgr(bot);
}

std::string PlayerbotAI::InventoryParseOutfitName(std::string outfit)
{
    int pos = outfit.find("=");
    if (pos == -1) return "";
    return outfit.substr(0, pos);
}

ItemIds PlayerbotAI::InventoryParseOutfitItems(std::string text)
{
    ItemIds itemIds;

    uint8 pos = text.find("=") + 1;
    while (pos < text.size())
    {
        int endPos = text.find(',', pos);
        if (endPos == -1)
            endPos = text.size();

        std::string idC = text.substr(pos, endPos - pos);
        uint32 id = atol(idC.c_str());
        pos = endPos + 1;
        if (id)
            itemIds.insert(id);
    }

    return itemIds;
}

void PlayerbotAI::Ping(float x, float y)
{
    WorldPacket data(MSG_MINIMAP_PING, (8 + 4 + 4));
    data << bot->GetObjectGuid();
    data << x;
    data << y;

    if (bot->GetGroup())
    {
        bot->GetGroup()->BroadcastPacket(
#ifdef MANGOS
            & data,
#endif
#ifdef CMANGOS
            data,
#endif
            true, -1, bot->GetObjectGuid());
    }
    else
    {
        bot->GetSession()->SendPacket(
#ifdef MANGOS
            & data
#endif
#ifdef CMANGOS
            data
#endif
            );
    }
}

void PlayerbotAI::Poi(float x, float y, std::string icon_name, Player* player, uint32 flags, uint32 icon, uint32 icon_data)
{
    if (!player)
        player = master;

    if (!player)
        return;

    WorldPacket data(SMSG_GOSSIP_POI, (4 + 4 + 4 + 4 + 4 + 10)); // guess size
    data << flags;
    data << x;
    data << y;
    data << icon;
    data << icon_data;
    data << icon_name;

    sServerFacade.SendPacket(player, data);
}

//Find Poison ...Natsukawa
Item* PlayerbotAI::FindPoison() const
{
   // list out items in main backpack
   for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
   {
      Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
      if (pItem)
      {
         const ItemPrototype* const pItemProto = pItem->GetProto();

         if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
            continue;

         if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == 6)
            return pItem;
      }
   }
   // list out items in other removable backpacks
   for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
   {
      const Bag* const pBag = (Bag *)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
      if (pBag)
         for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
         {
            Item* const pItem = bot->GetItemByPos(bag, slot);
            if (pItem)
            {
               const ItemPrototype* const pItemProto = pItem->GetProto();

               if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
                  continue;

               if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == 6)
                  return pItem;
            }
         }
   }
   return NULL;
}

Item* PlayerbotAI::FindConsumable(uint32 displayId) const
{
   // list out items in main backpack
   for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
   {
      Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
      if (pItem)
      {
         const ItemPrototype* const pItemProto = pItem->GetProto();

         if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
            continue;

         if ((pItemProto->Class == ITEM_CLASS_CONSUMABLE || pItemProto->Class == ITEM_CLASS_TRADE_GOODS) && pItemProto->DisplayInfoID == displayId)
            return pItem;
      }
   }
   // list out items in other removable backpacks
   for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
   {
      const Bag* const pBag = (Bag *)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
      if (pBag)
         for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
         {
            Item* const pItem = bot->GetItemByPos(bag, slot);
            if (pItem)
            {
               const ItemPrototype* const pItemProto = pItem->GetProto();

               if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
                  continue;

               if ((pItemProto->Class == ITEM_CLASS_CONSUMABLE || pItemProto->Class == ITEM_CLASS_TRADE_GOODS) && pItemProto->DisplayInfoID == displayId)
                  return pItem;
            }
         }
   }
   return NULL;
}

Item* PlayerbotAI::FindBandage() const
{
   // list out items in main backpack
   for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
   {
      Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
      if (pItem)
      {
         const ItemPrototype* const pItemProto = pItem->GetProto();

         if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
            continue;
#ifdef MANGOSBOT_ZERO
         if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == ITEM_SUBCLASS_FOOD)
#else
         if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == ITEM_SUBCLASS_BANDAGE)
#endif
            return pItem;
      }
   }
   // list out items in other removable backpacks
   for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
   {
      const Bag* const pBag = (Bag *)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
      if (pBag)
         for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
         {
            Item* const pItem = bot->GetItemByPos(bag, slot);
            if (pItem)
            {
               const ItemPrototype* const pItemProto = pItem->GetProto();

               if (!pItemProto || bot->CanUseItem(pItemProto) != EQUIP_ERR_OK)
                  continue;

#ifdef MANGOSBOT_ZERO
               if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == ITEM_SUBCLASS_FOOD)
#else
               if (pItemProto->Class == ITEM_CLASS_CONSUMABLE && pItemProto->SubClass == ITEM_SUBCLASS_BANDAGE)
#endif
                  return pItem;
            }
         }
   }
   return nullptr;
}

static const uint32 uPriorizedSharpStoneIds[8] =
{
    ADAMANTITE_SHARPENING_DISPLAYID, FEL_SHARPENING_DISPLAYID, ELEMENTAL_SHARPENING_DISPLAYID, DENSE_SHARPENING_DISPLAYID,
    SOLID_SHARPENING_DISPLAYID, HEAVY_SHARPENING_DISPLAYID, COARSE_SHARPENING_DISPLAYID, ROUGH_SHARPENING_DISPLAYID
};

static const uint32 uPriorizedWeightStoneIds[8] =
{
    ADAMANTITE_WEIGHTSTONE_DISPLAYID, FEL_WEIGHTSTONE_DISPLAYID, ELEMENTAL_SHARPENING_DISPLAYID, DENSE_WEIGHTSTONE_DISPLAYID,
    SOLID_WEIGHTSTONE_DISPLAYID, HEAVY_WEIGHTSTONE_DISPLAYID, COARSE_WEIGHTSTONE_DISPLAYID, ROUGH_WEIGHTSTONE_DISPLAYID
};

/**
 * FindStoneFor()
 * return Item* Returns sharpening/weight stone item eligible to enchant a bot weapon
 *
 * params:weapon Item* the weap�n the function should search and return a enchanting item for
 * return nullptr if no relevant item is found in bot inventory, else return a sharpening or weight
 * stone based on the weapon subclass
 *
 */
Item* PlayerbotAI::FindStoneFor(Item* weapon) const
{
    Item* stone;
    ItemPrototype const* pProto = weapon->GetProto();
    if (pProto && pProto->Class == ITEM_CLASS_WEAPON && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD || pProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2
        || pProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE || pProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2
        || pProto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER))
    {
        for (uint8 i = 0; i < countof(uPriorizedSharpStoneIds); ++i)
        {
            stone = FindConsumable(uPriorizedSharpStoneIds[i]);
            if (stone)
                return stone;
        }
    }
    else if (pProto && pProto->Class == ITEM_CLASS_WEAPON && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE || pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2))
    {
        for (uint8 i = 0; i < countof(uPriorizedWeightStoneIds); ++i)
        {
            stone = FindConsumable(uPriorizedWeightStoneIds[i]);
            if (stone)
                return stone;
        }
    }

    return nullptr;
}

static const uint32 uPriorizedWizardOilIds[5] =
{
    MINOR_WIZARD_OIL, LESSER_WIZARD_OIL, BRILLIANT_WIZARD_OIL, WIZARD_OIL, SUPERIOR_WIZARD_OIL
};

static const uint32 uPriorizedManaOilIds[4] =
{
   MINOR_MANA_OIL, LESSER_MANA_OIL, BRILLIANT_MANA_OIL, SUPERIOR_MANA_OIL,
};

Item* PlayerbotAI::FindOilFor(Item* weapon) const
{
    Item* oil = nullptr;
    ItemPrototype const* pProto = weapon->GetProto();

    const std::vector<uint32> uPriorizedWizardOilIds = { MINOR_WIZARD_OIL, LESSER_WIZARD_OIL, BRILLIANT_WIZARD_OIL, WIZARD_OIL, SUPERIOR_WIZARD_OIL };
    const std::vector<uint32> uPriorizedManaOilIds = { MINOR_MANA_OIL, LESSER_MANA_OIL, BRILLIANT_MANA_OIL, SUPERIOR_MANA_OIL };

    if (pProto && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD || pProto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF || pProto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER))
    {
        for (uint8 i = 0; i < uPriorizedWizardOilIds.size(); i++)
        {
            oil = FindConsumable(uPriorizedWizardOilIds[i]);
            if (!oil && i < uPriorizedManaOilIds.size())
            {
                oil = FindConsumable(uPriorizedManaOilIds[i]);
            }

            if(oil)
            {
                break;
            }
        }
    }
    else if (pProto && (pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE || pProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2))
    {
        for (uint8 i = 0; i < uPriorizedManaOilIds.size(); i++)
        {
            oil = FindConsumable(uPriorizedManaOilIds[i]);
            if (!oil && i < uPriorizedWizardOilIds.size())
            {
                oil = FindConsumable(uPriorizedWizardOilIds[i]);
            }

            if (oil)
            {
                break;
            }
        }
    }

    return oil;
}

//  on self
void PlayerbotAI::ImbueItem(Item* item)
{
   ImbueItem(item, TARGET_FLAG_SELF, ObjectGuid());
}

//  item on unit
void PlayerbotAI::ImbueItem(Item* item, Unit* target)
{
   if (!target)
      return;

   ImbueItem(item, TARGET_FLAG_UNIT, target->GetObjectGuid());
}

//  item on equipped item
void PlayerbotAI::ImbueItem(Item* item, uint8 targetInventorySlot)
{
   if (targetInventorySlot >= EQUIPMENT_SLOT_END)
      return;

   Item* const targetItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, targetInventorySlot);
   if (!targetItem)
      return;

   ImbueItem(item, TARGET_FLAG_ITEM, targetItem->GetObjectGuid());
}

// generic item use method
#ifdef MANGOSBOT_ZERO
void PlayerbotAI::ImbueItem(Item* item, uint16 targetFlag, ObjectGuid targetGUID)
#else
void PlayerbotAI::ImbueItem(Item* item, uint32 targetFlag, ObjectGuid targetGUID)
#endif
{
   if (!item)
      return;

   uint8 bagIndex = item->GetBagSlot();
   uint8 slot = item->GetSlot();
   uint8 cast_count = 0;
   ObjectGuid item_guid = item->GetObjectGuid();

   uint32 spellId = 0;
   uint8 spell_index = 0;
   for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
   {
      if (item->GetProto()->Spells[i].SpellId > 0)
      {
         spellId = item->GetProto()->Spells[i].SpellId;
         spell_index = i;
         break;
      }
   }

#ifdef CMANGOS
   std::unique_ptr<WorldPacket> packet(new WorldPacket(CMSG_USE_ITEM, 20));
#endif
#ifdef MANGOS
   WorldPacket* packet = new WorldPacket(CMSG_USE_ITEM);
#endif

   *packet << bagIndex;
   *packet << slot;
   *packet << spell_index;
#ifdef MANGOSBOT_ZERO
   *packet << targetFlag;
#endif
#ifdef MANGOSBOT_ONE
   *packet << cast_count;
   *packet << item_guid;
   *packet << targetFlag;
#endif
#ifdef MANGOSBOT_TWO
   *packet << spellId << item_guid << uint32(0) << uint8(0);
   *packet << targetFlag;
#endif

#ifdef CMANGOS
   if (targetFlag & (TARGET_FLAG_UNIT | TARGET_FLAG_ITEM | TARGET_FLAG_GAMEOBJECT))
#endif
#ifdef MANGOS
   if (targetFlag & (TARGET_FLAG_UNIT | TARGET_FLAG_ITEM | TARGET_FLAG_OBJECT))
#endif
      *packet << targetGUID.WriteAsPacked();

#ifdef CMANGOS
   bot->GetSession()->QueuePacket(std::move(packet));
#endif
#ifdef MANGOS
   bot->GetSession()->QueuePacket(packet);
#endif
}

void PlayerbotAI::EnchantItemT(uint32 spellid, uint8 slot, Item* item)
{
    Item* pItem = nullptr;
    if (item)
        pItem = item;
    else
        pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);

   if (!pItem)
    return;

   if (pItem->GetSlot() != slot)
       return;

#ifdef CMANGOS
   if (pItem->GetOwner() == nullptr)
       return;
#endif

#ifdef MANGOS
   SpellEntry const* spellInfo = sSpellStore.LookupEntry(spellid);
#else
   SpellEntry const* spellInfo = sSpellTemplate.LookupEntry<SpellEntry>(spellid);
#endif
   if (!spellInfo)
      return;

   uint32 enchantid = spellInfo->EffectMiscValue[0];
   if (!enchantid)
   {
      sLog.outError("%u: Invalid enchantid %s %s" , enchantid , " report to devs", bot->GetName());
      return;
   }

   if (!((1 << pItem->GetProto()->SubClass) & spellInfo->EquippedItemSubClassMask) &&
      !((1 << pItem->GetProto()->InventoryType) & spellInfo->EquippedItemInventoryTypeMask))
   {

      sLog.outError("%s: items could not be enchanted, wrong item type equipped", bot->GetName());

      return;
   }

#ifdef MANGOSBOT_TWO
   EnchantmentSlot enchantSlot = spellInfo->Effect[0] == SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC ? PRISMATIC_ENCHANTMENT_SLOT : PERM_ENCHANTMENT_SLOT;
#else
   EnchantmentSlot enchantSlot = PERM_ENCHANTMENT_SLOT;
#endif

   bot->ApplyEnchantment(pItem, enchantSlot, false);
   pItem->SetEnchantment(enchantSlot, enchantid, 0, 0);
   bot->ApplyEnchantment(pItem, enchantSlot, true);

   sLog.outDetail("%s: items was enchanted successfully!", bot->GetName());
}

uint32 PlayerbotAI::GetBuffedCount(Player* player, std::string spellname)
{
    Group* group = bot->GetGroup();
    uint32 bcount = 0;

    if (group)
    {
        for (GroupReference *gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->getSource();
            if (!member || !member->IsInWorld() && member->GetMapId() != bot->GetMapId())
                continue;

            if (!member->IsInGroup(player, true))
                continue;

            if (HasAura(spellname, member))
                bcount++;
        }
    }
    return bcount;
}

// WHY A BOT'S CHARGE STUNNED THE TARGET AND WENT NOWHERE.
//
// The core does everything needed for a client-less caster: Charge's launch
// calls MotionMaster::MoveCharge, which puts a ChargeMovementGenerator on top
// and drives the spline server-side - UpdateSplineMovement moves a Player by
// SetPosition every tick. The stun is a triggered spell applied at launch, so
// the target froze whether or not the caster ever arrived.
//
// What stopped the arrival was us. On the next AI tick, "reach melee" or the
// chase saw the bot out of range and called MoveChase, and MotionMaster::Mutate
// interrupts whatever is on top before it pushes - the charge included. The
// chase code had a guard for exactly this case, but it tested for
// EFFECT_MOTION_TYPE, and this core's charge generator reports
// CHARGE_MOTION_TYPE, so it never fired. Measured 2026-09-09 on a warrior
// companion: target stunned, companion standing where it cast.
//
// This is the one test, and MoveTo2 and ChaseTo - the two primitives that
// actually move a bot - both ask it. Not put into CanMove on purpose: the
// update loop calls StopMoving() when a moving bot "cannot move", which would
// cancel the very spline this is protecting.
bool PlayerbotAI::IsCharging()
{
    if (bot->IsStopped())
        return false;

    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm || mm->empty())
        return false;

    const MovementGeneratorType type = mm->GetCurrentMovementGeneratorType();
    return type == CHARGE_MOTION_TYPE || type == EFFECT_MOTION_TYPE;
}

bool PlayerbotAI::CanMove()
{
    // do not allow if not vehicle driver
    if (IsInVehicle() && !IsInVehicle(true))
    {
        return false;
    }

    if (sServerFacade.IsFrozen(bot))
    {
        return false;
    }
    if (sServerFacade.IsInRoots(bot))
    {
        return false;
    }
    if (sServerFacade.IsFeared(bot))
    {
        return false;
    }
    if (sServerFacade.IsCharmed(bot))
    {
        return false;
    }
    if (bot->IsStunned())
    {
        return false;
    }
    if (bot->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
    {
        return false;
    }
    if (bot->IsPolymorphed())
    {
        return false;
    }
    if (bot->IsTaxiFlying())
    {
        return false;
    }
    if (sServerFacade.UnitIsDead(bot) && !bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        return false;
    }
    if (bot->IsBeingTeleported())
    {
        return false;
    }
    if (bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        return false;
    }
    if (bot->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CLIENT_CONTROL_LOST))
    {
        return false;
    }
    if (IsJumping())
    {
        return false;
    }
#ifdef MANGOSBOT_ONE
    if (bot->IsFalling())
    {
        return false;
    }
    if (bot->IsJumping())
    {
        return false;
    }
#else
    if (bot->IsFalling())
    {
        return false;
    }
#endif

    MotionMaster& mm = *bot->GetMotionMaster();
    MovementGeneratorType currentMotion = mm.GetCurrentMovementGeneratorType();
    
#ifdef CMANGOS
    if (currentMotion == TAXI_MOTION_TYPE)
    {
        return false;
    }
    if (currentMotion == FALL_MOTION_TYPE)
    {
        return false;
    }
#endif
#ifdef MANGOS
    if (currentMotion == FLIGHT_MOTION_TYPE)
    {
        return false;
    }
#endif
    return true;
}

void PlayerbotAI::StopMoving()
{
    if (bot->IsTaxiFlying())
        return;

    if (IsInVehicle())
        return;

    if (bot->IsBeingTeleportedFar())
        return;

    if (bot->GetTransport())
        bot->m_movementInfo.SetMovementFlags(MOVEFLAG_ONTRANSPORT);
    else
        bot->m_movementInfo.SetMovementFlags(MOVEFLAG_NONE);

    bot->InterruptMoving(true);
    MovementInfo mInfo = bot->m_movementInfo;
    float x, y, z;
    bot->GetPosition(x, y, z);
    float o = bot->GetPosition().o;
    mInfo.ChangePosition(x, y, z, o);
    WorldPacket data(MSG_MOVE_STOP);
#ifdef MANGOSBOT_TWO
    data << bot->GetObjectGuid().WriteAsPacked();
#endif
    data << mInfo;
    bot->GetSession()->HandleMovementOpcodes(data);

    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType())
    {
        bot->GetMotionMaster()->Clear(false, true);
        bot->GetMotionMaster()->MoveIdle();
    }
}

bool PlayerbotAI::IsInRealGuild()
{
    if (!bot->GetGuildId())
        return false;

    Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
    if (!guild)
        return false;

    uint32 leaderAccount = sObjectMgr.GetPlayerAccountIdByGUID(guild->GetLeaderGuid());
    if (!leaderAccount)
        return false;

    return !sPlayerbotAIConfig.IsInRandomAccountList(leaderAccount);
}

bool PlayerbotAI::HasPlayerRelation()
{
    if (HasRealPlayerMaster())
        return true;

    if (IsInRealGuild())
        return true;

    if (IsPlayerFriend())
        return true;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return true;

    for (auto& p : sRandomPlayerbotMgr.GetPlayers())
    {
        // FindSocial, not GetSocial: the latter ASSERTs when a session has no
        // MasterPlayer, so one bad entry in this map aborts the process instead
        // of being skipped. That is not a hypothetical - see the 2026-09-06
        // crash walked back to exactly this line.
        PlayerSocial* social = p.second ? p.second->FindSocial() : nullptr;
        if (social && social->HasFriend(bot->GetObjectGuid()))
        {
            SetPlayerFriend(true);
            return true;
        }
    }

    return false;
}

/*
 * A BOT NPC NEVER VOLUNTEERS CHAT. NOT A WORD, BY ANY ROUTE.
 *
 * A bot NPC is scenery with a name. What it has to say is written down -- a
 * gossip menu, or an idle timeline that speaks through MonsterSay in the core
 * -- and none of that comes through here. Anything this module decides to say
 * on its own is therefore something nobody asked for, arriving in a voice the
 * design never chose.
 *
 * WHY EVERY EMITTER AND NOT THE ONE PATH THAT WAS FOUND. Switching the AI off
 * is one early return before DoNextAction, so the engine actions are covered by
 * construction -- and everything ABOVE that line is not. The chat reply queue
 * was the first to be found (QueueChatResponse, below); the whispered-command
 * reply is another; the packet handlers are three more doors. Chasing them one
 * at a time is how the same bug keeps coming back, so the refusal goes where
 * the words leave instead.
 *
 * FOUND THE HARD WAY, 2026-09-16: Sister Sanguinary, a Troll, greeted her
 * owner in ORCISH -- unreadable, since he was not Horde. That is not a bug in
 * the language: `AiPlayerbot.RandomBotSayWithoutMaster = 1` turns any masterless
 * bot's message into a PUBLIC SAY, and a public say carries the speaker's racial
 * language. A bot NPC has no master by definition, so every one of them is one
 * stray Tell away from shouting gibberish at a player.
 *
 * NOT THE `silent` STRATEGY. That is a companion's mute, it is a list people
 * edit, and several callers deliberately speak over it with `ignoreSilent`.
 * This is a property of what the character IS, so it is asked of the binding
 * table the same way Player::IsBotNpc() is.
 */
bool PlayerbotAI::MaySpeak() const
{
    return bot && !bot->IsBotNpc();
}

void PlayerbotAI::QueueChatResponse(uint32 msgType, ObjectGuid guid1, ObjectGuid guid2, std::string message, std::string chanName, std::string name, bool noDelay)
{
    /*
     * NOTHING THAT IS MEANT TO BE QUIET MAY ENTER THIS QUEUE.
     *
     * The queue is drained at the very top of UpdateAIInternal, by a direct
     * call to the static ChatReplyAction::ChatReplyDo -- above the IsBotNpc
     * early return that stops DoNextAction, and above ChatReplyAction::isUseful
     * which is where the `silent` strategy is checked. So neither of the two
     * ways this server makes a bot quiet reached the small talk at all: a bot
     * NPC standing as scenery answered anyone talking near it, and a companion
     * carrying +silent still chatted back at its owner.
     *
     * The gate goes here rather than at the drain so the queue never fills with
     * replies that will be thrown away. Commands are unaffected: they travel
     * the separate chatCommands queue and are handled either way.
     */
    if (bot->IsBotNpc() || HasStrategy("silent", BotState::BOT_STATE_NON_COMBAT))
        return;

    std::scoped_lock lock(chatRepliesMutex);
    chatReplies.push(ChatQueuedReply(msgType, guid1.GetCounter(), guid2.GetCounter(), message, chanName, name, time(0) + (noDelay ? 0 : urand(inCombat ? 15 : 10, inCombat ? 30 : 20))));
}

bool PlayerbotAI::PlayAttackEmote(float chanceMultiplier)
{
    auto group = bot->GetGroup();
    if (group) chanceMultiplier /= (group->GetMembersCount() - 1);
    if ((float)urand(0, 10000) * chanceMultiplier < sPlayerbotAIConfig.attackEmoteChance * 10000.0f && bot->IsInCombat())
    {
        std::vector<uint32> sounds;
        sounds.push_back(TEXTEMOTE_OPENFIRE);
        sounds.push_back(305);
        sounds.push_back(307);
        PlaySound(sounds[urand(0, sounds.size() - 1)]);
        return true;
    }

    return false;
}

void PlayerbotAI::QueuePacket(WorldPacket& pkt)
{
    // Penqle WorldPacket has deleted copy-assign; copy-construct + move-assign.
    std::unique_ptr<WorldPacket> packet(new WorldPacket(pkt));
    bot->GetSession()->QueuePacket(std::move(packet));
}

float PlayerbotAI::GetLevelFloat() const
{
    float level = bot->GetLevel();

    uint32 nextLevelXp = bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);

    if (!nextLevelXp)
        return level;

    uint32 xp = bot->GetUInt32Value(PLAYER_XP);

    while (xp > nextLevelXp)
    {
        level++;
        xp -= nextLevelXp;

        nextLevelXp = sObjectMgr.GetXPForLevel(level);
    }

    level += float(xp) / float(nextLevelXp);

    return level;
}

bool PlayerbotAI::CanSpellClick(Player* bot, uint32 entry)
{
#ifdef MANGOSBOT_TWO
    SpellClickInfoMapBounds clickPair = sObjectMgr.GetSpellClickInfoMapBounds(entry);

    if (clickPair.first != clickPair.second)
    {
        for (SpellClickInfoMap::const_iterator itr = clickPair.first; itr != clickPair.second; ++itr)
        {
            if (itr->second.questStart)
            {
                // not in expected required quest state
                if (!bot || ((!itr->second.questStartCanActive || !bot->IsActiveQuest(itr->second.questStart)) && !bot->GetQuestRewardStatus(itr->second.questStart)))
                    return false;
            }

            if (itr->second.questEnd)
            {
                // not in expected forbidden quest state
                if (!bot || bot->GetQuestRewardStatus(itr->second.questEnd))
                    return false;
            }

            return true;
        }
    }
#endif
    return false;
}

bool PlayerbotAI::CanSpellClick(ObjectGuid guid) const
{
#ifdef MANGOSBOT_TWO
    if (!guid.IsCreatureOrVehicle())
        return false;

    Creature* creature = GetAnyTypeCreature(guid);

    if (!creature)
        return CanSpellClick(bot, guid.GetEntry());

    // Check if there are spell click entries for this creature
    SpellClickInfoMapBounds clickPair = sObjectMgr.GetSpellClickInfoMapBounds(guid.GetEntry());
    if (clickPair.first == clickPair.second)
        return false;

    // Check if any of the spell click entries fit the requirements for this bot
    for (SpellClickInfoMap::const_iterator itr = clickPair.first; itr != clickPair.second; ++itr)
    {
        if (itr->second.IsFitToRequirements(bot, creature))
        {
            return true;
        }
    }
#endif
    return false;
}

bool PlayerbotAI::HandleSpellClick(uint32 entry) 
{
#ifdef MANGOSBOT_TWO
    SpellClickInfoMapBounds clickPair = sObjectMgr.GetSpellClickInfoMapBounds(entry);

    if (clickPair.first == clickPair.second)
        return false;

    Creature* creature = nullptr;

    AiObjectContext* context = aiObjectContext;
    std::list<ObjectGuid> guids = AI_VALUE(std::list<ObjectGuid>, "nearest npcs");
    for (auto& guid : guids)
    {
        if (!guid.IsCreatureOrVehicle())
            continue;

        if (guid.GetEntry() != entry)
            continue;

        return HandleSpellClick(guid);

        break;
    }  
#endif
    return false;
}

bool PlayerbotAI::HandleSpellClick(ObjectGuid guid)
{
#ifdef MANGOSBOT_TWO
    SpellClickInfoMapBounds clickPair = sObjectMgr.GetSpellClickInfoMapBounds(guid.GetEntry());

    if (clickPair.first == clickPair.second)
        return false;

    Creature* creature = nullptr;

    creature = GetAnyTypeCreature(guid);

    if (!creature)
        return false;

    for (SpellClickInfoMap::const_iterator itr = clickPair.first; itr != clickPair.second; ++itr)
    {
        if (itr->second.IsFitToRequirements(bot, creature))
        {
            if (sScriptDevAIMgr.OnNpcSpellClick(bot, creature, itr->second.spellId))
                return true;

            Unit* caster = (itr->second.castFlags & 0x1) ? (Unit*)bot : (Unit*)creature;
            Unit* target = (itr->second.castFlags & 0x2) ? (Unit*)bot : (Unit*)creature;

            if (itr->second.spellId)
            {
                caster->CastSpell(target, itr->second.spellId, TRIGGERED_OLD_TRIGGERED);
                return true;
            }
            else
                sLog.outError("WorldSession::HandleSpellClick: npc_spell_click with entry %u has 0 in spell_id. Not handled custom case?", creature->GetEntry());
        }
    }
#endif
    return false;
}


// See the declaration: the group members that are actual people. Used by the
// ported dungeon-clear status publisher to address its addon packets.
std::vector<Player*> PlayerbotAI::GetRealPlayersInGroup()
{
    std::vector<Player*> out;
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return out;

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* member = gref->getSource();
        if (member && IsRealPlayer(member))
            out.push_back(member);
    }
    return out;
}
