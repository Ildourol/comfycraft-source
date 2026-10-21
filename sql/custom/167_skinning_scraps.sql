-- ============================================================================
--  SKINNING GIVES 1 OR 2 RUINED LEATHER SCRAPS  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\167_skinning_scraps.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then `reload skinning_loot_template` in the console.
--
--  When a skin rolls Ruined Leather Scraps (2934), it gives 1 or 2, not 1.
--  The chance of scraps against Light Leather does not change. 126 skinning
--  tables, the low-level beasts. Light Leather still takes 3 scraps.
--
--  Re-runnable. To undo: the same UPDATE with maxcount = 1.

UPDATE skinning_loot_template SET maxcount = 2 WHERE item = 2934 AND mincountOrRef = 1;
