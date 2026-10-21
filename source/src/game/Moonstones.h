#ifndef MANGOS_MOONSTONES_H
#define MANGOS_MOONSTONES_H

#include "Common.h"
#include "ObjectGuid.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Item;
class Player;
class Unit;
struct ItemPrototype;

/*
 * GEAR FOR RUNE CHIPS. sql/custom/168, docs/features/rune-chips.md.
 *
 * The currency was first called Moonstone. Players see Rune Chip (item
 * 100036); the code, the table, `.moonstone` and the CMOON prefix kept the
 * first name, because no player sees them.
 *
 * A dialog button with "trades gear for Rune Chips" (SCRIPT_COMMAND_MOONSTONE_
 * TRADE, 103) calls StartTrade. That remembers the NPC and sends ComfyUI
 * `CMOON P`, which puts the client in pick mode: the spell cursor, and a click
 * on a bag item sends `.moonstone <bag> <slot>` instead of picking it up.
 * Right-click or Esc ends pick mode in the client; `CMOON S` ends it from here.
 *
 * Each `.moonstone` checks that the player is still near that NPC, destroys
 * the item and pays Rune Chips, a count rolled from `moonstone_payout`. No gold.
 *
 * AN ITEM THAT ANY SHOP LISTS IS REFUSED. Otherwise gold, honour or tokens buy
 * the item and the item becomes Rune Chips. The list is every item in
 * npc_vendor, npc_vendor_template and custom_merchant, read again by
 * `reload npc_vendor` and `reload custom_merchant`, which the shop editor sends
 * after a save.
 */

uint32 const MOONSTONE_ITEM = 100036;
float const MOONSTONE_REACH = 10.0f;            // yards from the NPC

class MoonstoneMgr
{
    public:
        void Load();                // the payout bands, then the shop list
        void LoadShopItems();

        // The Rune Chips for one of this item, or 0 when it is refused.
        // `why` is the line to show the player on a refusal.
        uint32 Roll(ItemPrototype const* proto, char const*& why) const;

        void StartTrade(Player* player, Unit* npc);
        // `.moonstone <bag> <slot>`, with the client's 0-4 bag and 1-based slot.
        void Trade(Player* player, uint32 bag, uint32 slot);

    private:
        void StopTrade(Player* player);

        struct Band
        {
            uint32 quality;
            uint32 minItemLevel, maxItemLevel;
            uint32 minCount, maxCount;
        };

        std::vector<Band> m_bands;
        std::unordered_set<uint32> m_shopItems;

        // Player guid to the NPC they are trading with. Maps update on
        // several threads, so behind a lock.
        std::unordered_map<uint32, ObjectGuid> m_trading;
        std::mutex m_tradingLock;
};

extern MoonstoneMgr sMoonstoneMgr;

#endif
