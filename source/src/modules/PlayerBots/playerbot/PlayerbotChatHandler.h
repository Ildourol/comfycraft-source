#pragma once
// Out of PlayerbotAI.h, so that header and the 437 files that reach it through
// playerbot.h do not need Chat.h. A base class needs the complete type, and
// Chat.h in every bot file made an edit to it rebuild the whole module.
#include "Chat/Chat.h"
#include "Objects/Player.h"

class PlayerbotChatHandler: protected ChatHandler
{
public:
    explicit PlayerbotChatHandler(Player* pMasterPlayer) : ChatHandler(pMasterPlayer->GetSession()) {}
    void sysmessage(std::string str) { SendSysMessage(str.c_str()); }
    uint32 extractQuestId(std::string str);
    uint32 extractSpellId(std::string str)
    {
        char* source = (char*)str.c_str();
        return ExtractSpellIdFromLink(&source);
    }
};
