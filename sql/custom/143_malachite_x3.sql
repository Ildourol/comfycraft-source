-- ============================================================================
--  Malachite drops three times as often  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\143_malachite_x3.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console, no restart:
--
--    reload creature_loot_template
--    reload gameobject_loot_template
--    reload pickpocketing_loot_template
--    reload reference_loot_template
--
--  Fashion Demands Sacrifices (60030) asks for 1 Malachite (774). Upstream
--  drops it from mobs at 0.3-7.9% (Elwynn's Prowler 1.3%), from a Copper
--  Vein at 1% and from chests at 0.2-0.5%. Every positive chance in these
--  four tables is now three times the upstream value:
--
--    creature_loot_template       692 rows   (the highest is 23.8%)
--    gameobject_loot_template       7 rows   (veins 3%; the vein's gem group
--                                             is 9% in all)
--    pickpocketing_loot_template   60 rows
--    reference_loot_template        5 rows
--
--  Not changed: quest-only rows (a negative chance, such as -45), containers
--  in item_loot_template (already 3-62%) and fishing.
--
--  comfy_loot_baseline keeps the upstream chance, so a second run sets the
--  same value and does not multiply again. Re-runnable.
-- ============================================================================

CREATE TABLE IF NOT EXISTS comfy_loot_baseline (
  src     VARCHAR(32)        NOT NULL,
  entry   MEDIUMINT UNSIGNED NOT NULL,
  item    MEDIUMINT UNSIGNED NOT NULL,
  groupid TINYINT UNSIGNED   NOT NULL,
  chance  FLOAT              NOT NULL,
  PRIMARY KEY (src, entry, item, groupid)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3
  COMMENT='Upstream loot chances, kept by sql/custom scripts that scale them';

INSERT IGNORE INTO comfy_loot_baseline
SELECT 'creature', entry, item, groupid, ChanceOrQuestChance
  FROM creature_loot_template WHERE item = 774 AND ChanceOrQuestChance > 0;
INSERT IGNORE INTO comfy_loot_baseline
SELECT 'gameobject', entry, item, groupid, ChanceOrQuestChance
  FROM gameobject_loot_template WHERE item = 774 AND ChanceOrQuestChance > 0;
INSERT IGNORE INTO comfy_loot_baseline
SELECT 'pickpocketing', entry, item, groupid, ChanceOrQuestChance
  FROM pickpocketing_loot_template WHERE item = 774 AND ChanceOrQuestChance > 0;
INSERT IGNORE INTO comfy_loot_baseline
SELECT 'reference', entry, item, groupid, ChanceOrQuestChance
  FROM reference_loot_template WHERE item = 774 AND ChanceOrQuestChance > 0;

UPDATE creature_loot_template l JOIN comfy_loot_baseline b
    ON b.src = 'creature' AND b.entry = l.entry AND b.item = l.item AND b.groupid = l.groupid
   SET l.ChanceOrQuestChance = LEAST(b.chance * 3, 100)
 WHERE l.item = 774;
UPDATE gameobject_loot_template l JOIN comfy_loot_baseline b
    ON b.src = 'gameobject' AND b.entry = l.entry AND b.item = l.item AND b.groupid = l.groupid
   SET l.ChanceOrQuestChance = LEAST(b.chance * 3, 100)
 WHERE l.item = 774;
UPDATE pickpocketing_loot_template l JOIN comfy_loot_baseline b
    ON b.src = 'pickpocketing' AND b.entry = l.entry AND b.item = l.item AND b.groupid = l.groupid
   SET l.ChanceOrQuestChance = LEAST(b.chance * 3, 100)
 WHERE l.item = 774;
UPDATE reference_loot_template l JOIN comfy_loot_baseline b
    ON b.src = 'reference' AND b.entry = l.entry AND b.item = l.item AND b.groupid = l.groupid
   SET l.ChanceOrQuestChance = LEAST(b.chance * 3, 100)
 WHERE l.item = 774;
