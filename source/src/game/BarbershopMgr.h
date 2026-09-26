#ifndef BARBERSHOP_MGR_H
#define BARBERSHOP_MGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

class GameObject;
class Player;

/*
 * THE SERVER HALF OF TURTLE'S BARBERSHOP.
 *
 * The client half ships in patch-4.mpq as
 * Interface\FrameXML\Turtle_BarbershopUI\Turtle_Barbershop.lua, and it is
 * complete: a window, a turntable model and three selectors. It draws nothing
 * until the server sends `TW_BARBERSHOP` / `S2C_OPEN_BARBER`, and upstream
 * never published the code that sends it, so sitting in a Barbershop Chair
 * (gameobject 105180) seated you and did nothing else.
 *
 * The protocol, read from that file (fields split on ';'):
 *
 *   S2C_OPEN_BARBER;cost;style;;color;;facial   (the empty fields are the
 *                                                "available" lists the client
 *                                                has commented out)
 *   S2C_UPDATE_PREVIEW;cost;style;color;facial
 *   S2C_PURCHASE_RESULT;1;style;color;facial
 *   S2C_ERROR;n                                  (BARBERSHOP_ERROR1..5)
 *   S2C_CLOSE_BARBER
 *
 *   C2S_PREVIEW_STYLE;n  C2S_PREVIEW_COLOR;n  C2S_PREVIEW_FACIAL_HAIR;n
 *   C2S_PURCHASE  C2S_RESET  C2S_CLOSE
 *
 * All values are the 0-based bytes in PLAYER_BYTES / PLAYER_BYTES_2.
 *
 * SKIN AND FACE ARE OURS. Turtle's window has no selector for them, so
 * addon/ComfyUI/Barbershop.lua adds two, and the protocol grows at the END of
 * each message, where Turtle's code never reads:
 *
 *   S2C_OPEN_BARBER;...;facial;skin;face;skinCount;faceCount
 *   S2C_UPDATE_PREVIEW;...;facial;skin;face
 *   S2C_PURCHASE_RESULT;...;facial;skin;face
 *   C2S_PREVIEW_SKIN;n  C2S_PREVIEW_FACE;n
 *
 * HEIGHT IS OURS TOO, added 2026-09-26, and it is not a byte. It is the
 * player's native scale in thousandths (characters.height, sql/custom/124),
 * sold from 900 to 1025 in steps of 5, and it goes after the face:
 *
 *   S2C_OPEN_BARBER;...;faceCount;height;min;max;step
 *   S2C_UPDATE_PREVIEW;...;face;height
 *   S2C_PURCHASE_RESULT;...;face;height
 *   C2S_PREVIEW_HEIGHT;n
 *
 * A PREVIEW IS SENT TO THE PLAYER ALONE. The client's model redraws from the
 * player's own fields, so a preview is a values update carrying the new bytes,
 * built by hand and sent only to that player's session. The real fields are
 * not touched until a purchase. So nobody else in view sees a preview or pays
 * for one in bandwidth, a save cannot write one, and a session that ends
 * without a purchase only has to send the old look back to the one client
 * that saw the new one. A purchase sets the real fields and redraws the model
 * for everyone in view, once.
 */
class BarbershopMgr
{
    public:
        static bool IsBarberChair(uint32 entry) { return entry == 105180; }
        // A height the chair sells, in thousandths. HandleCharCreateOpcode checks a
        // create-time height against it too.
        static bool IsSoldHeight(int height);

        // From the chair case of GameObject::Use, after the player is seated.
        void Open(Player* player, GameObject* chair);
        bool HandleAddonMessage(Player* player, uint32 type, std::string const& msg);

        // From Player::Update. Closes a session the player has walked out of.
        void Update(Player* player);
        // From WorldSession::LogoutPlayer.
        void OnLogout(Player* player);

    private:
        // The five appearance bytes a 1.12 character has, and the height.
        struct Look
        {
            uint8 values[5] = {};
            uint16 height = 1000;   // thousandths of the race's own scale

            bool operator==(Look const& other) const;
            bool operator!=(Look const& other) const { return !(*this == other); }
        };

        struct Session
        {
            ObjectGuid chair;
            Look original;  // what the player owns, and what the real fields hold
            Look preview;   // what the player's own client is showing
        };

        // Copper for going from one look to the other at the player's level.
        static uint32 GetCost(Player const* player, Look const& from, Look const& to);

        // Callers hold m_lock.
        void Close(Player* player, Session const& session, bool notify);
        Session* GetSession(Player const* player);

        static Look ReadLook(Player const* player);
        // Sets the real fields and redraws the model for everyone in view.
        static void ApplyLook(Player* player, Look const& look);
        // Shows this look to the player's own client only. `shown` is what that
        // client shows now: the model flashes only when a byte changes.
        static void ShowLook(Player* player, Look const& shown, Look const& look);
        static bool IsAllowed(Player const* player, Look const& original, uint32 customization, uint8 value);
        static bool IsHeightAllowed(Look const& original, int height);
        static uint8 GetLimit(Player const* player, uint32 customization);

        static void Send(Player* player, std::string const& message);
        static void SendPreview(Player* player, Session const& session);

        mutable std::mutex m_lock;
        std::unordered_map<uint32, Session> m_sessions;
        // Player::Update calls in for every player on every tick; this keeps
        // that to one atomic read while nobody is in a chair.
        std::atomic<uint32> m_count{0};
};

extern BarbershopMgr sBarbershopMgr;

#endif
