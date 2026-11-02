/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2017  MaNGOS project <https://getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include "Common.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "Object.h"
#include "Chat.h"
#include "Language.h"
#include "ScriptMgr.h"
#include "World.h"
#include "Group.h"
#include "LFGHandler.h"
#include "LFGMgr.h"
#include "LFTMgr.h"

// RecruitBot.cpp, stubbed in PlayerbotStubs.cpp: the classes with a premade
// spec for a hire role (SO_RECRUIT_*), lower case and comma separated, in
// class id order. The same classes Helper Elowen's second page offers.
extern std::string Playerbot_HireClasses(uint32 role);

// Opens ComfyUI's stone window. Prefix CSTONE:
//
//   O;<tank classes>;<healer classes>;<damage classes>   open it
//   C                                                     close it
//
// The class lists come from the premade specs, so the addon keeps no list
// of its own. An empty list (no bot module) leaves "Anyone" only.
static void SendStoneWindowOpen(Player* player)
{
    static uint32 const ROLES[] = { SO_RECRUIT_TANK, SO_RECRUIT_HEALER, SO_RECRUIT_DPS };

    std::string msg = "O";
    for (uint32 role : ROLES)
        msg += ";" + Playerbot_HireClasses(role);
    player->SendAddonMessage("CSTONE", msg);
}

void WorldSession::HandleMeetingStoneJoinOpcode(WorldPacket& recv_data)
{
    ObjectGuid guid;

    recv_data >> guid;

    DEBUG_LOG("WORLD: Recvd CMSG_MEETINGSTONE_JOIN Message guid: %s", guid.GetString().c_str());

    // ignore for remote control state
    if (!_player->IsSelfMover())
        return;

    GameObject *obj = _player->GetGameObjectIfCanInteractWith(guid);

    if (!obj)
        return;

    // Never expect this opcode for some type GO's
    if (obj->GetGoType() != GAMEOBJECT_TYPE_MEETINGSTONE)
    {
        sLog.outError("HandleMeetingStoneJoinOpcode: CMSG_MEETINGSTONE_JOIN for not allowed GameObject type %u (Entry %u), didn't expect this to happen.", obj->GetGoType(), obj->GetEntry());
        return;
    }

    // ComfyUI's stone window: hire companions here, or open the group finder
    // from it. A client without ComfyUI never sent `.stone ready` and gets the
    // role check below. The window has no party refusals of its own: the
    // hire says its own (leader only, party full, two each).
    if (HasStoneWindow())
    {
        SetStoneWindowGuid(obj->GetObjectGuid());
        SendStoneWindowOpen(_player);
        return;
    }

    if (Group* grp = _player->GetGroup())
    {
        if (!grp->IsLeader(_player->GetObjectGuid()))
        {
            SendMeetingstoneFailed(MEETINGSTONE_FAIL_PARTYLEADER);

            return;
        }

        if (grp->isRaidGroup())
        {
            SendMeetingstoneFailed(MEETINGSTONE_FAIL_RAID_GROUP);
            return;
        }

        if (grp->IsFull())
        {
            SendMeetingstoneFailed(MEETINGSTONE_FAIL_FULL_GROUP);
            return;
        }
    }


   GameObjectInfo const* gInfo = sObjectMgr.GetGameObjectInfo(obj->GetEntry());

   // The stone queues for the group finder, not the 1.12 meeting stone queue.
   // That queue has no window: it only says "still seeking more members"
   // every 5 minutes until the party is full.
   sLFTMgr.JoinFromMeetingStone(_player, gInfo->meetingstone.areaID);
}

void WorldSession::HandleMeetingStoneLeaveOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: Recvd CMSG_MEETINGSTONE_LEAVE");
    if(Group *grp = _player->GetGroup())
    {
        if(grp->IsLeader(_player->GetObjectGuid()) && grp->isInLFG())
        {
            sLFGMgr.RemoveGroupFromQueue(grp->GetId());
        }
        else
        {
            SendMeetingstoneSetqueue(0, MEETINGSTONE_STATUS_NONE);
        }
    }
    else
    {
        sLFGMgr.RemovePlayerFromQueue(_player->GetObjectGuid());
    }
}

void WorldSession::HandleMeetingStoneInfoOpcode(WorldPacket & /*recv_data*/)
{
    DEBUG_LOG("WORLD: Received CMSG_MEETING_STONE_INFO");

    if(Group *grp = _player->GetGroup())
    {
        if(grp->isInLFG())
        {
            SendMeetingstoneSetqueue(grp->GetLFGAreaId(), MEETINGSTONE_STATUS_JOINED_QUEUE);
        }
        else
        {
            SendMeetingstoneSetqueue(0, MEETINGSTONE_STATUS_NONE);
        }
    }
    else
    {
        sLFGMgr.RestoreOfflinePlayer(_player);
    }
}

void WorldSession::SendMeetingstoneFailed(uint8 status)
{
    WorldPacket data(SMSG_MEETINGSTONE_JOINFAILED, 1);
    data << uint8(status);
    SendPacket(&data);
}

void WorldSession::SendMeetingstoneSetqueue(uint32 areaid, uint8 status)
{
    WorldPacket data(SMSG_MEETINGSTONE_SETQUEUE, 5);
    data << uint32(areaid);
    data << uint8(status);
    SendPacket(&data);
}
