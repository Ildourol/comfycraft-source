-- ============================================================================
--  Elwynn Forest: Small Leather Collar at 20%  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\141_elwynn_collars.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console, no restart:
--
--    reload creature_loot_template
--
--  Fashion Demands Sacrifices (60030, level 10) asks for 5 Small Leather
--  Collar (4813). Upstream drops it only from cats, and Elwynn Forest has no
--  wild cats. These three mobs now drop it at 20%, near the cats' 18-25%:
--
--    Prowler (118, loot 118)                 level 9-10
--    Gray Forest Wolf (1922, loot 1922)      level 7-8
--    Young Forest Bear (822, loot 2000394)   level 8-9
--
--  Loot 2000394 is shared with Tirisfal Plagued Bear (80466, level 7-9), so
--  that bear drops the collar too. A positive chance, as on the cats: the
--  collar drops with or without the quest. Re-runnable.
-- ============================================================================

DELETE FROM creature_loot_template
 WHERE item = 4813 AND entry IN (118, 1922, 2000394);

INSERT INTO creature_loot_template
       (entry, item, ChanceOrQuestChance, groupid, mincountOrRef, maxcount, condition_id)
VALUES (118,     4813, 20, 0, 1, 1, 0),
       (1922,    4813, 20, 0, 1, 1, 0),
       (2000394, 4813, 20, 0, 1, 1, 0);
