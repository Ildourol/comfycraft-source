-- ============================================================================
--  LINEN CLOTH FROM THE LEVEL 1-4 HUMANOIDS  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\169_linen_low_level.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then `reload creature_loot_template` in the console.
--
--  Stock, the first area of each starting zone gives no cloth: linen starts
--  at level 5, at about 30% for 1-2. These 14 loot tables now give 1 Linen
--  Cloth (2589) at 20%. Imps, makrura and friendly quest NPCs are left out.
--
--  2000387 is shared by Rockjaw Trogg and Trogg Vermin; 2000194 by Kobold
--  Laborer and Snufflesnout (level 4-5).
--
--  Re-runnable. To undo: the DELETE alone.

DELETE FROM creature_loot_template WHERE item = 2589 AND entry IN (
  6,        -- Kobold Vermin           Northshire
  257,      -- Kobold Worker           Northshire
  2000194,  -- Kobold Laborer          Northshire
  38,       -- Defias Thug             Northshire
  724,      -- Burly Rockjaw Trogg     Coldridge Valley
  2000387,  -- Rockjaw Trogg           Coldridge Valley
  1718,     -- Rockjaw Raider          Coldridge Valley
  946,      -- Frostmane Novice        Coldridge Valley
  706,      -- Frostmane Troll Whelp   Coldridge Valley
  1506,     -- Scarlet Convert         Deathknell
  1507,     -- Scarlet Initiate        Deathknell
  2952,     -- Bristleback Quilboar    Camp Narache
  2953,     -- Bristleback Shaman      Camp Narache
  61786     -- Blood Elf Spy           high elf start
);

INSERT INTO creature_loot_template (entry, item, ChanceOrQuestChance, groupid, mincountOrRef, maxcount, condition_id) VALUES
  (6,       2589, 20, 0, 1, 1, 0),
  (257,     2589, 20, 0, 1, 1, 0),
  (2000194, 2589, 20, 0, 1, 1, 0),
  (38,      2589, 20, 0, 1, 1, 0),
  (724,     2589, 20, 0, 1, 1, 0),
  (2000387, 2589, 20, 0, 1, 1, 0),
  (1718,    2589, 20, 0, 1, 1, 0),
  (946,     2589, 20, 0, 1, 1, 0),
  (706,     2589, 20, 0, 1, 1, 0),
  (1506,    2589, 20, 0, 1, 1, 0),
  (1507,    2589, 20, 0, 1, 1, 0),
  (2952,    2589, 20, 0, 1, 1, 0),
  (2953,    2589, 20, 0, 1, 1, 0),
  (61786,   2589, 20, 0, 1, 1, 0);
