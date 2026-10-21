-- ============================================================================
--  MOONSTONES: SELL GEAR FOR STONES, BUY ENCHANT SCROLLS WITH THEM  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\168_moonstones.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  APPLY BEFORE THE BINARY THAT READS IT. MoonstoneMgr::Load selects
--  `moonstone_payout` at startup, and a missing table asserts.
--
--  The feature is docs/features/rune-chips.md. In short:
--
--    1. A dialog button "trades gear for Rune Chips" (script command 103) puts
--       ComfyUI in pick mode. Each green or blue weapon or armour the player
--       clicks is destroyed for Rune Chips rolled from `moonstone_payout`.
--       No gold.
--    2. Shop 200900 is a goods shop: 40 enchant scrolls, 7-25 Rune Chips
--       each by the enchant's skill (the cost section below).
--       These are the enchants an enchanter can make at skill 150 or lower,
--       from trainers and from formulas.
--
--  WHY THE SCROLLS CAST SPELLS 100100-100139 AND NOT THE REAL ENCHANTS. The
--  client checks reagents and the rod from its own Spell.dbc before it sends
--  the cast (sql/custom/003 has the history). Each scroll casts a copy of the
--  real spell with the reagents and the rod cleared. The server makes the
--  copies in SpellMgr.cpp (COMFY_REAGENT_FREE) and patch-V carries the same
--  copies (patches/build-patch-v.js, REAGENT_FREE). A scroll does nothing for
--  a player without the new patch-V. The three lists must stay the same.
--
--  IDS
--    item 100036                Rune Chip
--    items 100037-100076        the 40 scrolls, in shop order
--    spells 100100-100139       the reagent-free copies, same order
--    shop 200900                fixed, not max+1, so prod and local agree.
--                               Goods rows 2009000001-2009000040 (shop * 10000
--                               + slot, lib/shops.js TRADE_STRIDE).
--
--  After applying: restart mangosd (new spells and items), or with the
--  binary already running: reload custom_merchant, reload moonstone_payout.
--
--  Re-runnable: DELETE by id range, then INSERT.
-- ============================================================================

DELETE FROM item_template     WHERE entry BETWEEN 100036 AND 100076;
DELETE FROM custom_merchant   WHERE id    BETWEEN 2009000000 AND 2009009999;
DELETE FROM itemextendedcost  WHERE id    BETWEEN 2009000000 AND 2009009999;
DELETE FROM shop              WHERE id    = 200900;

-- ---------------------------------------------------------------- Rune Chip
-- Bound on pickup, like the scrolls. No sell price, so no vendor takes it.
INSERT INTO item_template
  (entry, class, subclass, name, display_id, quality, flags, buy_price, sell_price,
   inventory_type, allowable_class, allowable_race, item_level, required_level,
   stackable, max_count, bonding, material, sheath, description)
VALUES
  (100036,15,0,'Rune Chip',38760,2,0,0,0,0,-1,-1,1,0,200,0,1,0,0,
   'An enchanter trades scrolls for these.');

-- ---------------------------------------------------------------- Scrolls
-- The same shape as Elandra's scrolls (sql/custom/003): class 0, on use,
-- charges -1 so the scroll is used up.
INSERT INTO item_template
  (entry, class, subclass, name, display_id, quality, flags, buy_price, sell_price,
   inventory_type, allowable_class, allowable_race, item_level, required_level,
   stackable, max_count, bonding, material, sheath,
   spellid_1, spelltrigger_1, spellcharges_1)
VALUES
  (100037,0,0,'Scroll: Enchant Weapon - Minor Striking',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100100,0,-1),
  (100038,0,0,'Scroll: Enchant Weapon - Minor Beastslayer',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100101,0,-1),
  (100039,0,0,'Scroll: Enchant Weapon - Lesser Striking',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100102,0,-1),
  (100040,0,0,'Scroll: Enchant 2H Weapon - Minor Intellect',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100103,0,-1),
  (100041,0,0,'Scroll: Enchant 2H Weapon - Minor Impact',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100104,0,-1),
  (100042,0,0,'Scroll: Enchant 2H Weapon - Lesser Intellect',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100105,0,-1),
  (100043,0,0,'Scroll: Enchant 2H Weapon - Lesser Spirit',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100106,0,-1),
  (100044,0,0,'Scroll: Enchant 2H Weapon - Lesser Impact',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100107,0,-1),
  (100045,0,0,'Scroll: Enchant Chest - Minor Health',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100108,0,-1),
  (100046,0,0,'Scroll: Enchant Chest - Minor Mana',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100109,0,-1),
  (100047,0,0,'Scroll: Enchant Chest - Minor Absorption',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100110,0,-1),
  (100048,0,0,'Scroll: Enchant Chest - Lesser Health',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100111,0,-1),
  (100049,0,0,'Scroll: Enchant Chest - Lesser Mana',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100112,0,-1),
  (100050,0,0,'Scroll: Enchant Chest - Health',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100113,0,-1),
  (100051,0,0,'Scroll: Enchant Chest - Lesser Absorption',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100114,0,-1),
  (100052,0,0,'Scroll: Enchant Chest - Mana',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100115,0,-1),
  (100053,0,0,'Scroll: Enchant Chest - Minor Stats',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100116,0,-1),
  (100054,0,0,'Scroll: Enchant Bracer - Minor Deflect',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100117,0,-1),
  (100055,0,0,'Scroll: Enchant Bracer - Minor Stamina',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100118,0,-1),
  (100056,0,0,'Scroll: Enchant Bracer - Minor Spirit',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100119,0,-1),
  (100057,0,0,'Scroll: Enchant Bracer - Minor Agility',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100120,0,-1),
  (100058,0,0,'Scroll: Enchant Bracer - Minor Strength',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100121,0,-1),
  (100059,0,0,'Scroll: Enchant Bracer - Lesser Spirit',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100122,0,-1),
  (100060,0,0,'Scroll: Enchant Bracer - Lesser Stamina',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100123,0,-1),
  (100061,0,0,'Scroll: Enchant Bracer - Lesser Strength',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100124,0,-1),
  (100062,0,0,'Scroll: Enchant Bracer - Lesser Intellect',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100125,0,-1),
  (100063,0,0,'Scroll: Enchant Cloak - Minor Resistance',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100126,0,-1),
  (100064,0,0,'Scroll: Enchant Cloak - Minor Protection',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100127,0,-1),
  (100065,0,0,'Scroll: Enchant Cloak - Minor Agility',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100128,0,-1),
  (100066,0,0,'Scroll: Enchant Cloak - Lesser Protection',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100129,0,-1),
  (100067,0,0,'Scroll: Enchant Cloak - Lesser Fire Resistance',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100130,0,-1),
  (100068,0,0,'Scroll: Enchant Cloak - Lesser Shadow Resistance',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100131,0,-1),
  (100069,0,0,'Scroll: Enchant Boots - Minor Stamina',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100132,0,-1),
  (100070,0,0,'Scroll: Enchant Boots - Minor Agility',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100133,0,-1),
  (100071,0,0,'Scroll: Enchant Gloves - Mining',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100134,0,-1),
  (100072,0,0,'Scroll: Enchant Gloves - Herbalism',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100135,0,-1),
  (100073,0,0,'Scroll: Enchant Gloves - Fishing',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100136,0,-1),
  (100074,0,0,'Scroll: Enchant Shield - Minor Stamina',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100137,0,-1),
  (100075,0,0,'Scroll: Enchant Shield - Lesser Protection',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100138,0,-1),
  (100076,0,0,'Scroll: Enchant Shield - Lesser Spirit',1093,2,64,0,0,0,-1,-1,20,0,10,0,1,7,0,100139,0,-1);

-- ---------------------------------------------------------------- The shop
-- A goods shop (shop_type 1), opened from a dialog button "Opens a shop".
INSERT INTO shop (id, name, shop_type, comment) VALUES
  (200900, 'Rune Chip scrolls', 1, 'sql/custom/168. Enchants to skill 150, 7-25 Rune Chips by skill.');

-- One cost row a scroll, the same id as its merchant row (lib/shops.js).
-- THE COST SLIDES WITH THE ENCHANT'S SKILL: 5 + skill / 7.5, rounded. Skill
-- 15 costs 7, skill 75 costs 15, skill 150 costs 25. The skill is the
-- enchanting skill the recipe needs, from the trainer or the formula.
CREATE TEMPORARY TABLE scroll_skill (entry mediumint unsigned PRIMARY KEY, skill smallint unsigned);
INSERT INTO scroll_skill (entry, skill) VALUES
  (100037, 90),
  (100038, 90),
  (100039,140),
  (100040, 75),
  (100041,100),
  (100042,100),
  (100043,110),
  (100044,145),
  (100045, 15),
  (100046, 20),
  (100047, 40),
  (100048, 60),
  (100049, 80),
  (100050,120),
  (100051,140),
  (100052,145),
  (100053,150),
  (100054, 20),
  (100055, 50),
  (100056, 60),
  (100057, 80),
  (100058, 80),
  (100059,120),
  (100060,130),
  (100061,140),
  (100062,150),
  (100063, 45),
  (100064, 70),
  (100065,110),
  (100066,115),
  (100067,125),
  (100068,135),
  (100069,125),
  (100070,125),
  (100071,145),
  (100072,145),
  (100073,145),
  (100074,105),
  (100075,115),
  (100076,130);

INSERT INTO itemextendedcost (id, requiredItem1, requiredItemCount1)
SELECT 2009000000 + (entry - 100036), 100036, ROUND(5 + skill / 7.5)
  FROM scroll_skill;

DROP TEMPORARY TABLE scroll_skill;

INSERT INTO custom_merchant (id, entry, slot, item, count, extendedcost, condition_id)
SELECT 2009000000 + (entry - 100036), 200900, entry - 100036, entry, 1,
       2009000000 + (entry - 100036), 0
  FROM item_template WHERE entry BETWEEN 100037 AND 100076;

-- ---------------------------------------------------------------- Payout
-- One row a band. An item that matches no row is refused. The count is
-- weighted toward max_count: count k has weight (k - min_count + 1), so for
-- 1-5 the chances are 7, 13, 20, 27 and 33 percent.
CREATE TABLE IF NOT EXISTS `moonstone_payout` (
  `quality`        tinyint(3)  unsigned NOT NULL COMMENT '2 green, 3 blue',
  `min_item_level` smallint(5) unsigned NOT NULL,
  `max_item_level` smallint(5) unsigned NOT NULL,
  `min_count`      tinyint(3)  unsigned NOT NULL,
  `max_count`      tinyint(3)  unsigned NOT NULL,
  PRIMARY KEY (`quality`, `min_item_level`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COMMENT='Rune Chips for one item traded at an enchanter. sql/custom/168';

DELETE FROM moonstone_payout;
INSERT INTO moonstone_payout (quality, min_item_level, max_item_level, min_count, max_count) VALUES
  (2,  1,  29, 1,  5),
  (2, 30, 255, 3,  8),
  (3,  1,  29, 5, 10),
  (3, 30, 255, 8, 15);
