-- ============================================================================
--  LIGHT LEATHER COUNT RISES WITH MOB LEVEL  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\171_light_leather_by_level.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then `reload skinning_loot_template` in the console.
--
--  Stock, a skin that rolls Light Leather (2318) gives 1 at every level.
--  Now the count follows the highest level_max of the creatures that use the
--  skinning table:
--
--    level 1-9    1     (51 tables, not changed)
--    level 10-14  1-2   (59 tables)
--    level 15+    1-3   (155 tables)
--
--  The chance of Light Leather against scraps and hide does not change.
--
--  Re-runnable. To undo: set maxcount = 1 on item 2318, except entries
--  1191, 10116 and 12723, which were 1-2 before this file.

UPDATE skinning_loot_template l
JOIN (
  SELECT s.entry, MAX(ct.level_max) AS hi
  FROM skinning_loot_template s
  JOIN creature_template ct ON ct.skinning_loot_id = s.entry
  WHERE s.item = 2318
  GROUP BY s.entry
) t ON t.entry = l.entry
SET l.maxcount = CASE WHEN t.hi >= 15 THEN 3 ELSE 2 END
WHERE l.item = 2318 AND l.mincountOrRef = 1 AND t.hi >= 10;
