-- ============================================================================
--  MANGY WOLF DROPS THE SMALL LEATHER COLLAR  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\156_mangy_wolf_drops_small_leather_collar.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  Then, in the mangosd console:
--    reload creature_loot_template
--
--  Fashion Demands Sacrifices (60030, level 10) asks for 5 Small Leather
--  Collar (4813). In Elwynn Forest only the Prowler, the Young Forest Bear
--  and the Gray Forest Wolf (all 20%) dropped it. The Mangy Wolf (525,
--  level 5-6) now drops it at 10%. No other creature uses loot id 525.
--
--  The DELETE also covers the Timber Wolf (69) and the Young Wolf (299). An
--  earlier draft of this file gave them the collar, and they must not drop it.

DELETE FROM creature_loot_template WHERE entry IN (69, 299, 525) AND item = 4813;
INSERT INTO creature_loot_template (entry, item, ChanceOrQuestChance, groupid, mincountOrRef, maxcount, condition_id) VALUES
  (525, 4813, 10, 0, 1, 1, 0);
