-- ============================================================================
--  Elwynn kobolds: Malachite at 4%  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\144_kobold_malachite.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console, no restart:
--
--    reload creature_loot_template
--
--  143 triples every Malachite (774) chance, which puts Kobold Tunneler (475)
--  at 1.98% and Kobold Miner (40) at 2.04%. Both are now 4%, near the
--  Prowler's 3.9%. Kobold Geomancer (476) stays at 12.96%.
--
--  Apply after 143: a run of 143 sets these two back to three times upstream.
--  Re-runnable.
-- ============================================================================

UPDATE creature_loot_template
   SET ChanceOrQuestChance = 4
 WHERE item = 774 AND entry IN (475, 40);
