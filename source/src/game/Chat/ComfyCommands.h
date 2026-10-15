#pragma once

// ComfyCraft's own chat command handlers. They are free functions rather than
// ChatHandler members so that a new command does not edit Chat.h: that header
// reaches 121 objects, this one reaches Chat.cpp (the command table) and the
// files that define the handlers. ChatHandler names this struct as a friend, so
// the handlers keep the protected members they used as members, through
// `handler->`.
//
// A new command: declare it here, define it in its .cpp, add the table row in
// Chat.cpp, and a COMFY_COMMANDS row in Commands.cpp.

#include "Common.h"

class ChatHandler;
class Player;
struct House;

struct ComfyCommands
{
    // Commands/Commands.cpp
    static bool HandleComfyHelpCommand(ChatHandler* handler, char* args);
    static bool HandleComfyNpcsCommand(ChatHandler* handler, char* args);
    static bool HandleGuildHouseCommand(ChatHandler* handler, char* args);
    static bool HandleModifyNpcFlagCommand(ChatHandler* handler, char* args);
    static bool HandleSetLevelCommand(ChatHandler* handler, char* args);
    static bool HandleLevelCapCommand(ChatHandler* handler, char* args);
    static bool HandleFlightModeCommand(ChatHandler* handler, char* args);
    static bool HandleEffectCommand(ChatHandler* handler, char* args);
    static bool HandleEmoteCommand(ChatHandler* handler, char* args);
    static bool HandleSpotCommand(ChatHandler* handler, char* args);
    static bool HandleSpotFromCommand(ChatHandler* handler, char* args);
    static bool HandleLookupTeleCommand(ChatHandler* handler, char* args);
    static bool HandleSceneCommand(ChatHandler* handler, char* args);
    static bool HandleReloadBotNpcCommand(ChatHandler* handler, char* args);
    static bool HandleReloadBroadcastTextCommand(ChatHandler* handler, char* args);
    static bool HandleReloadCustomMerchantCommand(ChatHandler* handler, char* args);
    static bool HandleReloadDialogScenesCommand(ChatHandler* handler, char* args);
    static bool HandleReloadGameObjectTemplateCommand(ChatHandler* handler, char* args);
    static bool HandleReloadRentedMountCommand(ChatHandler* handler, char* args);
    static bool HandleReloadStartZonesCommand(ChatHandler* handler, char* args);
    static bool HandleGMTicketRespondCommand(ChatHandler* handler, char* args);

    // Commands/ObjCommands.cpp
    static bool HandleObjCommand(ChatHandler* handler, char* args);

    // Handlers/MiscHandler.cpp, beside the bug-report code it shares with
    // CMSG_BUG rather than beside the other chat commands.
    static bool HandleBugCommand(ChatHandler* handler, char* args);

    // Housing/HouseCommands.cpp
    static bool HandleHouseGoCommand(ChatHandler* handler, char* args);
    static bool HandleHouseResetCommand(ChatHandler* handler, char* args);
    static bool HandleHouseVisitCommand(ChatHandler* handler, char* args);
    static bool HandleHouseAdmitCommand(ChatHandler* handler, char* args);
    static bool HandleHouseRefuseCommand(ChatHandler* handler, char* args);
    static bool HouseAnswerKnock(ChatHandler* handler, char* args, bool admit);
    static bool HandleHouseEditCommand(ChatHandler* handler, char* args);
    static bool HandleHouseStorageCommand(ChatHandler* handler, char* args);
    static bool HandleHouseSettingCommand(ChatHandler* handler, char* args);
    static bool HandleHouseFurnitureReloadCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectAddCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectDragCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectMoveCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectRotateCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectScaleCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectDelCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectClearCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectListCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectSelectCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectUnselectCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectGroupCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectUngroupCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectChalkCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectEditCommand(ChatHandler* handler, char* args);
    static bool HandleHouseObjectMarkerCommand(ChatHandler* handler, char* args);
    static bool HandleHouseEntranceCreateCommand(ChatHandler* handler, char* args);
    static bool HandleHouseEntranceListCommand(ChatHandler* handler, char* args);
    static bool HandleHouseEntranceMoveCommand(ChatHandler* handler, char* args);
    static bool HandleHouseEntranceRemoveCommand(ChatHandler* handler, char* args);
    static bool HandleHouseEntranceWhereCommand(ChatHandler* handler, char* args);
    static bool HandleHouseEntranceHomeCommand(ChatHandler* handler, char* args);
    static bool HandleHouseEntranceTemplateCommand(ChatHandler* handler, char* args);
    static bool HandleHouseEntranceModelCommand(ChatHandler* handler, char* args);
    static bool HandleHouseEntranceHandleCommand(ChatHandler* handler, char* args);
    static bool HandleHouseTemplateCreateCommand(ChatHandler* handler, char* args);
    static bool HandleHouseTemplateExitCommand(ChatHandler* handler, char* args);
    static bool HandleHouseTemplateSaveCommand(ChatHandler* handler, char* args);
    static bool HandleHouseTemplateGotoCommand(ChatHandler* handler, char* args);
    static bool HandleHouseTemplateDeleteCommand(ChatHandler* handler, char* args);
    static bool HandleHouseTemplateListCommand(ChatHandler* handler, char* args);
    static uint32 HouseTarget(ChatHandler* handler, char* &args, House*& house, bool useSelection = true);

    // Housing/HouseCritter.cpp
    static bool HandleHouseCritterAddCommand(ChatHandler* handler, char* args);
    static bool HandleHouseCritterDragCommand(ChatHandler* handler, char* args);
    static bool HandleHouseCritterWanderCommand(ChatHandler* handler, char* args);
    static bool HandleHouseCritterDelCommand(ChatHandler* handler, char* args);
    static bool HandleHouseCritterListCommand(ChatHandler* handler, char* args);
    static bool HandleHouseCritterVisualCommand(ChatHandler* handler, char* args);
    static bool HandleHouseCritterReloadCommand(ChatHandler* handler, char* args);
    static bool HandleHouseCritterNameCommand(ChatHandler* handler, char* args);

    // The PlayerBots module (playerbot/PlayerbotAI.cpp), stubbed in
    // PlayerbotStubs.cpp for a build without it.
    static bool HandleNpcBotCommand(ChatHandler* handler, char* args);
    // Its Creature half. A member, not a free function, for the friend access.
    // Only HandleNpcBotCommand calls it, so it needs no stub.
    static int NpcBotCreatureCommand(ChatHandler* handler, Player* me, std::string const& action, char const* nameArg);
    static bool HandleAddDressingRoomCommand(ChatHandler* handler, char* args);

    // playerbot/CompanionOwnership.cpp, also stubbed in PlayerbotStubs.cpp.
    static bool HandleCompanionsCommand(ChatHandler* handler, char* args);
};
