-- ============================================================================
--  Black Witch Hat  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\223_black_witch_hat.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console:  reload item_template
--
--  A white cloth hat with no stats. Display 12220 is the hat of Dread Mage
--  Hat (3556, display 16544) with the hair left showing: the two rows differ
--  only in the icon and the helmet geoset vis (0 here, 265 there) in
--  ItemDisplayInfo.dbc. Any class and race can wear it. It is not bound,
--  so players can trade it.
--
--  Nothing sells or drops it yet. `.additem 100077` gives one.
-- ============================================================================

DELETE FROM item_template WHERE entry = 100077;

INSERT INTO item_template
  (entry, class, subclass, name, description, display_id, quality, flags,
   buy_count, buy_price, sell_price, inventory_type, allowable_class,
   allowable_race, item_level, required_level, stackable, max_count,
   armor, bonding, material, sheath, max_durability)
VALUES
  (100077, 4, 1, 'Black Witch Hat', '', 12220, 1, 0,
   1, 500, 125, 1, -1,
   -1, 1, 0, 1, 0,
   2, 0, 7, 0, 20);
