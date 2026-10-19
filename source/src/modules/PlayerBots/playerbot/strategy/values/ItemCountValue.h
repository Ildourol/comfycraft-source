#pragma once
#include "playerbot/strategy/Value.h"
#include "playerbot/strategy/ItemVisitors.h"

namespace ai
{
    class ItemCountValue : public Uint32CalculatedValue, public Qualified
	{
	public:
        ItemCountValue(PlayerbotAI* ai, std::string name = "item count") : Uint32CalculatedValue(ai, name), Qualified() {}
        virtual uint32 Calculate() override;
	};

    // The list is cached for up to a second, and an item can be destroyed in
    // that second: a potion's last charge, a stack sold or traded. Get and
    // LazyGet hand back only the items the bot still holds.
    class InventoryItemValue : public CalculatedValue<std::list<Item*> >, public Qualified
    {
    public:
        InventoryItemValue(PlayerbotAI* ai, std::string name = "inventory items") : CalculatedValue<std::list<Item*> >(ai, name), Qualified() {}
        virtual std::list<Item*> Calculate() override;
        virtual std::list<Item*> Get() override;
        virtual std::list<Item*> LazyGet() override;

    private:
        std::list<Item*> StillHeld() const;

        // What Calculate found, with each item's guid: a freed item's address
        // can be reused by a new item.
        std::vector<std::pair<Item*, ObjectGuid>> found;
    };

    class InventoryItemIdValue : public CalculatedValue<std::list<uint32> >, public Qualified
    {
    public:
        InventoryItemIdValue(PlayerbotAI* ai, std::string name = "inventory item ids") : CalculatedValue<std::list<uint32> >(ai, name), Qualified() {}
        virtual std::list<uint32> Calculate() override { std::list<uint32> retVal;  for (auto& item : AI_VALUE2(std::list<Item*>, "inventory items", getQualifier())) { ItemPrototype const* proto = item->GetProto();  retVal.push_back(proto->ItemId);} return retVal;};
    };

    class EquipedUsableTrinketValue : public CalculatedValue<std::list<Item*> >, public Qualified
    {
    public:
        EquipedUsableTrinketValue(PlayerbotAI* ai) : CalculatedValue<std::list<Item*> >(ai), Qualified() {}
        virtual std::list<Item*> Calculate() override;
    };

    void InvalidateItemCountCache(Player* bot);
    void InvalidateItemCountCache(Player* bot, const std::string& qualifier);
}
