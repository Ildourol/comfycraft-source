#include "BarbershopMgr.h"

#include "GameObject.h"
#include "Map.h"
#include "Player.h"
#include "WorldSession.h"

#include <cstdlib>
#include <vector>

BarbershopMgr sBarbershopMgr;

namespace
{
    char const* BarbershopPrefix = "TW_BARBERSHOP";
    std::string const BarbershopMessagePrefix = std::string(BarbershopPrefix) + "\t";

    // The display id the GM appearance commands flash to make the client
    // rebuild a player's model; new hair bytes alone do not redraw it.
    uint32 const DisplayIdBox = 4;

    // How far the player may be from the chair before the session ends.
    float const MaxChairDistance = 5.0f;

    // Index into Look::values, Limits and the byte table below.
    enum Customization
    {
        CUSTOMIZATION_STYLE  = 0,
        CUSTOMIZATION_COLOR  = 1,
        CUSTOMIZATION_FACIAL = 2,
        CUSTOMIZATION_SKIN   = 3,
        CUSTOMIZATION_FACE   = 4,
        CUSTOMIZATION_COUNT  = 5,
    };

    // Where each one lives on the player: { field, byte }.
    uint32 const Bytes[CUSTOMIZATION_COUNT][2] =
    {
        { PLAYER_BYTES, 2 }, { PLAYER_BYTES, 3 }, { PLAYER_BYTES_2, 0 },
        { PLAYER_BYTES, 0 }, { PLAYER_BYTES, 1 },
    };

    char const* const PreviewCommands[CUSTOMIZATION_COUNT] =
    {
        "C2S_PREVIEW_STYLE", "C2S_PREVIEW_COLOR", "C2S_PREVIEW_FACIAL_HAIR",
        "C2S_PREVIEW_SKIN", "C2S_PREVIEW_FACE",
    };

    // BARBERSHOP_ERROR1..5 in the client's GlobalStrings.lua.
    enum BarbershopError
    {
        BARBERSHOP_ERROR_NO_SESSION   = 1,
        BARBERSHOP_ERROR_NO_MONEY     = 2,
        BARBERSHOP_ERROR_IN_SESSION   = 3,
        BARBERSHOP_ERROR_NOT_ALLOWED  = 4,
        BARBERSHOP_ERROR_SYSTEM       = 5,
    };

    /*
     * How many of each choice a race has, { male, female }, indexed by race id.
     * Race 10 is the High Elf, "BloodElf" in the client.
     *
     * The first three are copied from the `Customizations` table in
     * Turtle_Barbershop.lua, which is what draws the "3/17" on each selector:
     * the server must refuse exactly what the client will not offer, or a
     * hand-sent message buys a style the client has no art for.
     *
     * Skin and face come from CharSections.dbc, counting only rows whose last
     * field is 0: the rows with 1 are NPC skins such as HumanMaleSkin00_103NecroBlue,
     * and they reuse the same colour indexes. The client's copy (patch-4.mpq) and
     * server/dbc agreed on every count on 2026-09-24. Faces are listed per skin
     * colour, and every race has the same number of faces on every skin, so one
     * number per race and sex is the whole truth and a skin change never strands
     * a face.
     */
    uint8 const Limits[CUSTOMIZATION_COUNT][11][2] =
    {
        {   // hair style
            { 0, 0 }, { 17, 26 }, { 13, 14 }, { 17, 19 }, { 12, 12 }, { 16, 17 },
            { 13, 12 }, { 12, 12 }, { 10, 10 }, { 14, 17 }, { 16, 20 },
        },
        {   // hair colour
            { 0, 0 }, { 10, 10 }, { 8, 8 }, { 10, 10 }, { 10, 10 }, { 11, 11 },
            { 9, 9 }, { 10, 10 }, { 10, 10 }, { 5, 5 }, { 16, 16 },
        },
        {   // facial hair, markings, earrings, tusks or features
            { 0, 0 }, { 9, 7 }, { 11, 7 }, { 21, 6 }, { 14, 10 }, { 17, 8 },
            { 9, 7 }, { 12, 7 }, { 14, 10 }, { 10, 5 }, { 11, 11 },
        },
        {   // skin colour
            { 0, 0 }, { 10, 10 }, { 9, 9 }, { 14, 14 }, { 11, 11 }, { 6, 6 },
            { 20, 12 }, { 6, 6 }, { 12, 12 }, { 3, 3 }, { 11, 11 },
        },
        {   // face
            { 0, 0 }, { 12, 15 }, { 10, 9 }, { 10, 10 }, { 10, 9 }, { 10, 10 },
            { 5, 4 }, { 7, 7 }, { 5, 6 }, { 4, 6 }, { 10, 10 },
        },
    };

    std::vector<std::string> SplitFields(std::string const& text)
    {
        std::vector<std::string> fields;
        size_t from = 0;
        for (;;)
        {
            size_t at = text.find(';', from);
            fields.push_back(text.substr(from, at == std::string::npos ? std::string::npos : at - from));
            if (at == std::string::npos)
                return fields;
            from = at + 1;
        }
    }

    bool IsInBarberChairState(Player const* player)
    {
        uint8 state = player->GetStandState();
        return state >= UNIT_STAND_STATE_SIT_LOW_CHAIR && state <= UNIT_STAND_STATE_SIT_HIGH_CHAIR;
    }
}

/*
 * RETAIL'S SHAPE, OUR NUMBERS.
 *
 * Retail read a base price per level from gtBarberShopCostBase.dbc and charged
 * the whole base for a new style, half of it for a new colour on the same
 * style, and three quarters each for new facial hair and a new skin colour.
 * The 1.12 client has no such table, so the base here is level squared times
 * 5 copper: 5c at level 1, 5s at 10, 80s at 40 and 1g 80s at 60. Retail never
 * sold faces in this shape; a face is priced like a skin.
 */
uint32 BarbershopMgr::GetCost(Player const* player, Look const& from, Look const& to)
{
    uint32 level = player->GetLevel();
    uint32 base = level * level * 5;

    uint32 cost = 0;
    if (to.values[CUSTOMIZATION_STYLE] != from.values[CUSTOMIZATION_STYLE])
        cost += base;
    else if (to.values[CUSTOMIZATION_COLOR] != from.values[CUSTOMIZATION_COLOR])
        cost += base / 2;
    if (to.values[CUSTOMIZATION_FACIAL] != from.values[CUSTOMIZATION_FACIAL])
        cost += base * 3 / 4;
    if (to.values[CUSTOMIZATION_SKIN] != from.values[CUSTOMIZATION_SKIN])
        cost += base * 3 / 4;
    if (to.values[CUSTOMIZATION_FACE] != from.values[CUSTOMIZATION_FACE])
        cost += base * 3 / 4;
    return cost;
}

BarbershopMgr::Look BarbershopMgr::ReadLook(Player const* player)
{
    Look look;
    for (uint32 i = 0; i < CUSTOMIZATION_COUNT; ++i)
        look.values[i] = player->GetByteValue(Bytes[i][0], Bytes[i][1]);
    return look;
}

void BarbershopMgr::ApplyLook(Player* player, Look const& look)
{
    bool changed = false;
    for (uint32 i = 0; i < CUSTOMIZATION_COUNT; ++i)
    {
        if (player->GetByteValue(Bytes[i][0], Bytes[i][1]) != look.values[i])
        {
            player->SetByteValue(Bytes[i][0], Bytes[i][1], look.values[i]);
            changed = true;
        }
    }
    if (!changed)
        return;

    // Player::UpdateAppearance does the same flash but ends in DeMorph, which
    // would drop a morph. Put back whatever display the player had instead;
    // the next values update carries it with the new bytes.
    uint32 display = player->GetDisplayId();
    player->SetDisplayId(DisplayIdBox);
    player->DirectSendPublicValueUpdate(UNIT_FIELD_DISPLAYID);
    player->SetDisplayId(display);
}

uint8 BarbershopMgr::GetLimit(Player const* player, uint32 customization)
{
    uint32 race = player->GetRace();
    uint32 gender = player->GetGender();
    if (race >= 11 || gender >= 2)
        return 0;
    return Limits[customization][race][gender];
}

bool BarbershopMgr::IsAllowed(Player const* player, Look const& original, uint32 customization, uint8 value)
{
    // What the character already has is always allowed, whatever the table says.
    if (value == original.values[customization])
        return true;
    return value < GetLimit(player, customization);
}

void BarbershopMgr::Send(Player* player, std::string const& message)
{
    player->SendAddonMessage(BarbershopPrefix, message);
}

void BarbershopMgr::SendPreview(Player* player, Session const& session)
{
    Look look = ReadLook(player);
    std::string message = "S2C_UPDATE_PREVIEW;" + std::to_string(GetCost(player, session.original, look));
    for (uint32 i = 0; i < CUSTOMIZATION_COUNT; ++i)
        message += ";" + std::to_string(look.values[i]);
    Send(player, message);
}

bool BarbershopMgr::GetSession(Player const* player, Session& out) const
{
    auto itr = m_sessions.find(player->GetGUIDLow());
    if (itr == m_sessions.end())
        return false;
    out = itr->second;
    return true;
}

void BarbershopMgr::Open(Player* player, GameObject* chair)
{
    if (!player || !chair || !player->IsAlive() || player->IsInCombat())
        return;

    // A shapeshift or a morph hides the hair the window is about.
    if (player->GetDisplayId() != player->GetNativeDisplayId())
    {
        player->GetSession()->SendNotification("Return to your normal form first.");
        return;
    }

    std::lock_guard<std::mutex> guard(m_lock);

    // Clicking the chair again while seated: the client ignores a second open.
    if (m_sessions.count(player->GetGUIDLow()))
        return;

    Session session;
    session.chair = chair->GetObjectGuid();
    session.original = ReadLook(player);
    m_sessions[player->GetGUIDLow()] = session;
    ++m_count;

    uint8 const* v = session.original.values;
    Send(player, "S2C_OPEN_BARBER;0;" + std::to_string(v[CUSTOMIZATION_STYLE]) + ";;" +
        std::to_string(v[CUSTOMIZATION_COLOR]) + ";;" + std::to_string(v[CUSTOMIZATION_FACIAL]) + ";" +
        std::to_string(v[CUSTOMIZATION_SKIN]) + ";" + std::to_string(v[CUSTOMIZATION_FACE]) + ";" +
        std::to_string(GetLimit(player, CUSTOMIZATION_SKIN)) + ";" +
        std::to_string(GetLimit(player, CUSTOMIZATION_FACE)));
}

void BarbershopMgr::Close(Player* player, Session const& session, bool notify)
{
    ApplyLook(player, session.original);
    if (m_sessions.erase(player->GetGUIDLow()))
        --m_count;
    if (notify)
        Send(player, "S2C_CLOSE_BARBER");
}

bool BarbershopMgr::HandleAddonMessage(Player* player, uint32 type, std::string const& msg)
{
    if (!player || type != CHAT_MSG_GUILD || msg.compare(0, BarbershopMessagePrefix.size(), BarbershopMessagePrefix) != 0)
        return false;

    std::vector<std::string> fields = SplitFields(msg.substr(BarbershopMessagePrefix.size()));
    std::string const& command = fields[0];

    std::lock_guard<std::mutex> guard(m_lock);

    Session session;
    if (!GetSession(player, session))
    {
        // The client sends C2S_CLOSE at every login to clear a session it may
        // have lost to a disconnect. That is not an error worth a red line.
        if (command != "C2S_CLOSE")
            Send(player, "S2C_ERROR;" + std::to_string(BARBERSHOP_ERROR_NO_SESSION));
        return true;
    }

    for (uint32 customization = 0; customization < CUSTOMIZATION_COUNT; ++customization)
    {
        if (command != PreviewCommands[customization])
            continue;
        if (fields.size() < 2)
            return true;

        int value = atoi(fields[1].c_str());
        if (value < 0 || value > 255 || !IsAllowed(player, session.original, customization, uint8(value)))
        {
            Send(player, "S2C_ERROR;" + std::to_string(BARBERSHOP_ERROR_NOT_ALLOWED));
            SendPreview(player, session);
            return true;
        }

        Look look = ReadLook(player);
        look.values[customization] = uint8(value);
        ApplyLook(player, look);
        SendPreview(player, session);
        return true;
    }

    if (command == "C2S_PURCHASE")
    {
        Look look = ReadLook(player);
        uint32 cost = GetCost(player, session.original, look);
        if (player->GetMoney() < cost)
        {
            Send(player, "S2C_ERROR;" + std::to_string(BARBERSHOP_ERROR_NO_MONEY));
            return true;
        }

        if (cost)
            player->ModifyMoney(-int32(cost));

        // The new look becomes the one a cancel goes back to, and the one the
        // save writes.
        m_sessions[player->GetGUIDLow()].original = look;

        std::string message = "S2C_PURCHASE_RESULT;1";
        for (uint32 i = 0; i < CUSTOMIZATION_COUNT; ++i)
            message += ";" + std::to_string(look.values[i]);
        Send(player, message);
    }
    else if (command == "C2S_RESET")
    {
        ApplyLook(player, session.original);
        SendPreview(player, session);
    }
    else if (command == "C2S_CLOSE")
    {
        Close(player, session, true);
        player->SetStandState(UNIT_STAND_STATE_STAND);
    }

    return true;
}

void BarbershopMgr::Update(Player* player)
{
    if (!m_count.load())
        return;

    std::lock_guard<std::mutex> guard(m_lock);

    Session session;
    if (!GetSession(player, session))
        return;

    bool stay = player->IsAlive() && !player->IsInCombat() && IsInBarberChairState(player);
    if (stay)
    {
        GameObject* chair = player->IsInWorld() ? player->GetMap()->GetGameObject(session.chair) : nullptr;
        stay = chair && player->GetDistance(chair) <= MaxChairDistance;
    }

    if (!stay)
        Close(player, session, true);
}

void BarbershopMgr::OnLogout(Player* player)
{
    if (!m_count.load())
        return;

    std::lock_guard<std::mutex> guard(m_lock);

    Session session;
    if (GetSession(player, session))
        Close(player, session, false);
}

void BarbershopMgr::GetSaveBytes(Player const* player, uint32& bytes, uint32& bytes2) const
{
    if (!m_count.load())
        return;

    std::lock_guard<std::mutex> guard(m_lock);

    Session session;
    if (!GetSession(player, session))
        return;

    // PLAYER_BYTES is exactly skin, face, hair style and hair colour, low byte
    // first; PLAYER_BYTES_2 byte 0 is facial hair and the rest is left alone.
    uint8 const* v = session.original.values;
    bytes = uint32(v[CUSTOMIZATION_SKIN]) | (uint32(v[CUSTOMIZATION_FACE]) << 8) |
        (uint32(v[CUSTOMIZATION_STYLE]) << 16) | (uint32(v[CUSTOMIZATION_COLOR]) << 24);
    bytes2 = (bytes2 & 0xFFFFFF00) | uint32(v[CUSTOMIZATION_FACIAL]);
}
