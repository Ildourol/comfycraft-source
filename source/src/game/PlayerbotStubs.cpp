// Empty implementations for builds without the playerbots module
// (BUILD_PLAYERBOTS=OFF). The host calls these unconditionally; without the
// stubs the link step fails on missing symbols. When the module is built it
// provides the real ones and this file is excluded (src/game/CMakeLists.txt).
//
// This list used to be three times as long. Everything the bots reached the
// core through is a module hook now, and a hook needs no stub - the virtual
// has an empty body and nothing registers it. What is left are the few
// symbols that are genuinely core-side: the module bootstrap, the damage-log
// probes compiled into Unit.cpp, and the chat commands Chat.cpp registers
// whether or not the module exists.

#include "Objects/Player.h"
#include "Objects/Unit.h"
#include "World.h"
#include "WorldPacket.h"
#include "Chat/Chat.h"
#include "ComfyCommands.h"
#include <string>

// The single call the core makes into the module: it registers the hook
// objects. Nothing to register when the module is not there.
void World::InitPlayerbotsAtStartup()         {}

void BotActionLog_LogCastStart  (WorldObject*, uint32, uint64, uint32)         {}
void BotActionLog_LogCastResult (WorldObject*, uint32, uint8, const char*)     {}
void BotActionLog_LogDamage     (Unit*, Unit*, uint32, uint32, const char*)    {}
void BotActionLog_LogAuraAttempt(Unit*, uint32, int32, uint64)                 {}
void BotActionLog_LogAuraApply  (Unit*, uint32, int32, uint64)                 {}
void BotActionLog_LogAuraRemove (Unit*, uint32, uint64)                        {}

// Backs SCRIPT_COMMAND_RECRUIT_BOT (93). With no bot module there is nobody to
// recruit, so the gossip option simply does nothing.
bool Playerbot_RecruitBotForPlayer(Player*, uint32, uint32) { return false; }

// Backs `reload bot_npc` re-wearing a design's outfit. With no bot module there
// are no bot NPCs to dress, so this can only be reached by a stale row.
bool BotNpc_Dress(Player*, std::string const&, std::string& error)
    { error = "playerbots not built"; return false; }

// Backs `.gear`: the item pick is the bots' own.
bool Playerbot_GearPlayer(Player*, uint32, uint32, std::string& error)
    { error = "gear needs the playerbots module"; return false; }

// ChatHandler bot-command stubs. Chat.cpp registers `.bot`, `.rndbot`,
// `.ahbot`, and `.perfmon` in the command table unconditionally (no
// #ifdef BUILD_PLAYERBOTS gate), so the host must link these symbols even
// when the bot module isn't compiled in. Return true and inform the user.
bool ChatHandler::HandlePlayerbotCommand(char*)
    { SendSysMessage("Playerbots not built (BUILD_PLAYERBOTS=OFF)."); return true; }
bool ComfyCommands::HandleNpcBotCommand(ChatHandler* handler, char*)
    { handler->SendSysMessage("Bot NPCs need the playerbots module (BUILD_PLAYERBOTS=OFF)."); return true; }
bool ComfyCommands::HandleAddDressingRoomCommand(ChatHandler* handler, char*)
    { handler->SendSysMessage("Saved looks need the playerbots module (BUILD_PLAYERBOTS=OFF)."); return true; }
bool ComfyCommands::HandleCompanionsCommand(ChatHandler* handler, char*)
    { handler->SendSysMessage("No companions out."); return true; }
bool ComfyCommands::HandleCompanionDebugCommand(ChatHandler* handler, char*)
    { handler->SendSysMessage("Companions need the playerbots module (BUILD_PLAYERBOTS=OFF)."); return true; }
bool ChatHandler::HandleRandomPlayerbotCommand(char*)
    { SendSysMessage("Random playerbots not built (BUILD_PLAYERBOTS=OFF)."); return true; }
bool ChatHandler::HandleAhBotCommand(char*)
    { SendSysMessage("AHBot not built (BUILD_PLAYERBOTS=OFF)."); return true; }
bool ChatHandler::HandlePerfMonCommand(char*)
    { SendSysMessage("Bot performance monitor not built (BUILD_PLAYERBOTS=OFF)."); return true; }
