#include "Moonstones.h"
#include "Database/DatabaseEnv.h"
#include "Item.h"
#include "ItemPrototype.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Util.h"
#include "WorldSession.h"

MoonstoneMgr sMoonstoneMgr;

// The addon prefix. ComfyUI/Moonstones.lua listens for it.
static char const* const MOONSTONE_PREFIX = "CMOON";

void MoonstoneMgr::Load()
{
    m_bands.clear();

    std::unique_ptr<QueryResult> result(WorldDatabase.Query(
        "SELECT quality, min_item_level, max_item_level, min_count, max_count FROM moonstone_payout"));
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            Band band;
            band.quality      = fields[0].GetUInt32();
            band.minItemLevel = fields[1].GetUInt32();
            band.maxItemLevel = fields[2].GetUInt32();
            band.minCount     = fields[3].GetUInt32();
            band.maxCount     = fields[4].GetUInt32();

            if (!band.minCount || band.maxCount < band.minCount)
            {
                sLog.outErrorDb("moonstone_payout: quality %u from item level %u pays %u-%u; skipped.",
                                band.quality, band.minItemLevel, band.minCount, band.maxCount);
                continue;
            }
            m_bands.push_back(band);
        }
        while (result->NextRow());
    }

    sLog.outString(">> Loaded %u Rune Chip payout bands.", uint32(m_bands.size()));
    LoadShopItems();
}

void MoonstoneMgr::LoadShopItems()
{
    m_shopItems.clear();

    std::unique_ptr<QueryResult> result(WorldDatabase.Query(
        "SELECT item FROM npc_vendor UNION SELECT item FROM npc_vendor_template "
        "UNION SELECT item FROM custom_merchant"));
    if (result)
    {
        do
            m_shopItems.insert(result->Fetch()[0].GetUInt32());
        while (result->NextRow());
    }

    sLog.outString(">> Rune Chips: %u items sold in shops, refused.", uint32(m_shopItems.size()));
}

uint32 MoonstoneMgr::Roll(ItemPrototype const* proto, char const*& why) const
{
    why = "Only green or blue weapons and armour.";

    if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
        return 0;

    Band const* band = nullptr;
    for (Band const& b : m_bands)
        if (b.quality == proto->Quality && proto->ItemLevel >= b.minItemLevel && proto->ItemLevel <= b.maxItemLevel)
            band = &b;
    if (!band)
        return 0;

    if (m_shopItems.count(proto->ItemId))
    {
        why = "A shop sells that item, so it earns no Rune Chips.";
        return 0;
    }

    // Weighted toward the top: count k has weight (k - min + 1).
    uint32 const n = band->maxCount - band->minCount + 1;
    uint32 pick = urand(1, n * (n + 1) / 2);
    for (uint32 weight = 1; weight <= n; ++weight)
    {
        if (pick <= weight)
            return band->minCount + weight - 1;
        pick -= weight;
    }
    return band->maxCount;
}

void MoonstoneMgr::StartTrade(Player* player, Unit* npc)
{
    {
        std::lock_guard<std::mutex> guard(m_tradingLock);
        m_trading[player->GetGUIDLow()] = npc->GetObjectGuid();
    }
    player->SendAddonMessage(MOONSTONE_PREFIX, "P");
}

void MoonstoneMgr::StopTrade(Player* player)
{
    {
        std::lock_guard<std::mutex> guard(m_tradingLock);
        m_trading.erase(player->GetGUIDLow());
    }
    player->SendAddonMessage(MOONSTONE_PREFIX, "S");
}

void MoonstoneMgr::Trade(Player* player, uint32 bag, uint32 slot)
{
    ObjectGuid npcGuid;
    {
        std::lock_guard<std::mutex> guard(m_tradingLock);
        auto it = m_trading.find(player->GetGUIDLow());
        if (it != m_trading.end())
            npcGuid = it->second;
    }

    Unit* npc = npcGuid ? player->GetMap()->GetUnit(npcGuid) : nullptr;
    if (!npc || !npc->IsAlive() || !player->IsWithinDistInMap(npc, MOONSTONE_REACH))
    {
        StopTrade(player);
        player->GetSession()->SendNotification("Talk to the enchanter first.");
        return;
    }

    // The client counts slots from 1. Bag 0 is the backpack.
    if (bag > 4 || !slot)
        return;
    uint8 const serverBag = bag ? uint8(INVENTORY_SLOT_BAG_START + bag - 1) : INVENTORY_SLOT_BAG_0;
    uint32 const serverSlot = bag ? slot - 1 : INVENTORY_SLOT_ITEM_START + slot - 1;
    if (!bag && serverSlot >= INVENTORY_SLOT_ITEM_END)
        return;

    Item* item = player->GetItemByPos(serverBag, uint8(serverSlot));
    if (!item || item->IsInTrade() || player->GetLootGuid() == item->GetObjectGuid())
        return;

    char const* why = nullptr;
    uint32 const count = Roll(item->GetProto(), why) * item->GetCount();
    if (!count)
    {
        player->GetSession()->SendNotification("%s", why);
        return;
    }

    // Room first, so a full bag costs the player nothing.
    ItemPosCountVec dest;
    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, MOONSTONE_ITEM, count) != EQUIP_ERR_OK)
    {
        player->SendEquipError(EQUIP_ERR_INVENTORY_FULL, nullptr, nullptr, MOONSTONE_ITEM);
        return;
    }

    player->LogItem(item, LogItemAction::Sold);
    player->ItemRemovedQuestCheck(item->GetEntry(), item->GetCount());
    player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);

    if (Item* stones = player->StoreNewItem(dest, MOONSTONE_ITEM, true))
        player->SendNewItem(stones, count, true, false);
}
