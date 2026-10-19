#include "playerbot/playerbot.h"
#include "ItemCountValue.h"
#include "Objects/Item.h"
#include "Objects/ItemPrototype.h"
#include "Objects/Player.h"
#include "Objects/Bag.h"
#include <unordered_map>
#include <unordered_set>
#include <ctime>

using namespace ai;

static std::list<Item*> Find(PlayerbotAI* ai, std::string qualifier)
{
    std::list<Item*> result;
    Player* bot = ai->GetBot();
	IterateItemsMask mask = IterateItemsMask((uint8)IterateItemsMask::ITERATE_ITEMS_IN_EQUIP | (uint8)IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    result = ai->InventoryParseItems(qualifier, mask);
    return result;
}

uint32 ItemCountValue::Calculate()
{
    uint32 count = 0;
    std::list<Item*> items = Find(ai, qualifier);
    for (Item* item : items)
        count += item->GetCount();
    
    return count;
}

std::list<Item*> InventoryItemValue::Calculate()
{
    std::list<Item*> items = Find(ai, qualifier);
    found.clear();
    for (Item* item : items)
        found.emplace_back(item, item->GetObjectGuid());
    return items;
}

std::list<Item*> InventoryItemValue::Get()
{
    CalculatedValue<std::list<Item*> >::Get();
    return StillHeld();
}

std::list<Item*> InventoryItemValue::LazyGet()
{
    CalculatedValue<std::list<Item*> >::LazyGet();
    return StillHeld();
}

// The same places Find looks: equipped, the backpack and the bags. A cached
// pointer is compared by value before it is read, so a freed one is never
// dereferenced.
std::list<Item*> InventoryItemValue::StillHeld() const
{
    std::list<Item*> result;
    if (found.empty())
        return result;

    Player* bot = ai->GetBot();
    std::unordered_set<Item*> held;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            held.insert(item);
    for (uint8 slot = KEYRING_SLOT_START; slot < KEYRING_SLOT_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            held.insert(item);
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item* item = bag->GetItemByPos(slot))
                    held.insert(item);

    for (const auto& entry : found)
        if (held.count(entry.first) && entry.first->GetObjectGuid() == entry.second)
            result.push_back(entry.first);
    return result;
}

std::list<Item*> EquipedUsableTrinketValue::Calculate()
{
	std::list<Item*> trinkets;
	std::list<Item*> result;

	Player* bot = ai->GetBot();

	if (Item* trinket1 = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1))
		trinkets.push_back(trinket1);

	if (Item* trinket2 = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET2))
		trinkets.push_back(trinket2);

	if (trinkets.empty())
		return result;

	for (Item * item : trinkets)
	{
		ItemPrototype const* proto = item->GetProto();

		for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
		{
			if (proto->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE &&
				proto->Spells[i].SpellId > 0 &&
				bot->IsSpellReady(proto->Spells[i].SpellId, proto))
			{
				result.push_back(item);
			}
		}
	}

	return result;
}