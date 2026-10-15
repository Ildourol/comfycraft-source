-- ============================================================================
--  Leper Gnome: Tattered Gnomish Shoulders at 5%  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\136_leper_gnome_shoulders.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console, no restart:
--
--    reload creature_loot_template
--
--  Upstream gives Leper Gnome (1211) Tattered Gnomish Shoulders (14389, grey,
--  sells for 2 copper) at chance 100, so every kill dropped one. It was the
--  only common mob up to level 15 with a 100% item drop. 5% is a normal junk
--  chance. Re-runnable.
-- ============================================================================

UPDATE creature_loot_template
   SET ChanceOrQuestChance = 5
 WHERE entry = 1211 AND item = 14389;
