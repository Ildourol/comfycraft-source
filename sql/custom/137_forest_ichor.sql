-- ============================================================================
--  Forest Spider: Forest Ichor at 50%  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\137_forest_ichor.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console, no restart:
--
--    reload creature_loot_template
--
--  Upstream gives Forest Spider (30) Forest Ichor (41993) at quest chance 33
--  (stored as -33). Forest Ichor Exchange (41895) asks for 3. At 50% a player
--  kills about 6 spiders, not 9. The value stays negative, so the ichor drops
--  only while the quest is active. Re-runnable.
-- ============================================================================

UPDATE creature_loot_template
   SET ChanceOrQuestChance = -50
 WHERE entry = 30 AND item = 41993;
