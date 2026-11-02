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
--    2. Shop 200900 is a goods shop: 77 enchant scrolls, 7-150 Rune Chips
--       each by the enchant's skill (the cost section below).
--       These are the enchants an enchanter can make at skill 225 or lower
--       (Expert, which a level 30 character can train), from trainers and
--       from formulas.
--
--  WHY THE SCROLLS CAST SPELLS 100100-100176 AND NOT THE REAL ENCHANTS. The
--  client checks reagents and the rod from its own Spell.dbc before it sends
--  the cast (sql/custom/003 has the history). Each scroll casts a copy of the
--  real spell with the reagents and the rod cleared. The server makes the
--  copies in SpellMgr.cpp (COMFY_REAGENT_FREE) and patch-V carries the same
--  copies (patches/build-patch-v.js, REAGENT_FREE). A scroll does nothing for
--  a player without the new patch-V. The three lists must stay the same.
--
--  IDS
--    item 100036                Rune Chip
--    items 100037-100076        the 40 scrolls to skill 150
--    items 102000-102036        the 37 scrolls from skill 151 to 225
--    spells 100100-100139       the reagent-free copies of 100037-100076
--    spells 100140-100176       the reagent-free copies of 102000-102036
--    shop 200900                fixed, not max+1, so prod and local agree.
--                               Goods rows 2009000001-2009000077 (shop * 10000
--                               + slot, lib/shops.js TRADE_STRIDE).
--
--  After applying: restart mangosd (new spells and items), or with the
--  binary already running: reload custom_merchant, reload moonstone_payout.
--
--  Re-runnable: DELETE by id range, then INSERT.
-- ============================================================================

DELETE FROM item_template     WHERE entry BETWEEN 100036 AND 100076;
DELETE FROM item_template     WHERE entry BETWEEN 102000 AND 102036;
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
  (100036,15,0,'Rune Chip',35465,2,0,0,0,0,-1,-1,1,0,200,0,1,0,0,
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

-- The Expert enchants, skill 151 to 225: what a level 30 enchanter can learn.
-- Items 102000-102036 cast spells 100140-100176, same order. Minor Speed
-- (102027) needs character level 20 to use.
INSERT INTO item_template
  (entry, class, subclass, name, display_id, quality, flags, buy_price, sell_price,
   inventory_type, allowable_class, allowable_race, item_level, required_level,
   stackable, max_count, bonding, material, sheath,
   spellid_1, spelltrigger_1, spellcharges_1)
VALUES
  (102000,0,0,'Scroll: Enchant Weapon - Lesser Beastslayer',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100140,0,-1),
  (102001,0,0,'Scroll: Enchant Weapon - Lesser Elemental Slayer',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100141,0,-1),
  (102002,0,0,'Scroll: Enchant Weapon - Winter''s Might',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100142,0,-1),
  (102003,0,0,'Scroll: Enchant Weapon - Striking',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100143,0,-1),
  (102004,0,0,'Scroll: Enchant 2H Weapon - Impact',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100144,0,-1),
  (102005,0,0,'Scroll: Enchant Chest - Greater Health',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100145,0,-1),
  (102006,0,0,'Scroll: Enchant Chest - Greater Mana',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100146,0,-1),
  (102007,0,0,'Scroll: Enchant Chest - Lesser Stats',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100147,0,-1),
  (102008,0,0,'Scroll: Enchant Chest - Superior Health',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100148,0,-1),
  (102009,0,0,'Scroll: Enchant Bracer - Spirit',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100149,0,-1),
  (102010,0,0,'Scroll: Enchant Bracer - Stamina',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100150,0,-1),
  (102011,0,0,'Scroll: Enchant Bracer - Lesser Deflection',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100151,0,-1),
  (102012,0,0,'Scroll: Enchant Bracer - Strength',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100152,0,-1),
  (102013,0,0,'Scroll: Enchant Bracer - Agility',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100153,0,-1),
  (102014,0,0,'Scroll: Enchant Bracer - Vampirism',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100154,0,-1),
  (102015,0,0,'Scroll: Enchant Bracer - Intellect',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100155,0,-1),
  (102016,0,0,'Scroll: Enchant Bracer - Greater Spirit',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100156,0,-1),
  (102017,0,0,'Scroll: Enchant Cloak - Defense',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100157,0,-1),
  (102018,0,0,'Scroll: Enchant Cloak - Fire Resistance',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100158,0,-1),
  (102019,0,0,'Scroll: Enchant Cloak - Greater Defense',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100159,0,-1),
  (102020,0,0,'Scroll: Enchant Cloak - Resistance',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100160,0,-1),
  (102021,0,0,'Scroll: Enchant Cloak - Lesser Agility',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100161,0,-1),
  (102022,0,0,'Scroll: Enchant Boots - Lesser Agility',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100162,0,-1),
  (102023,0,0,'Scroll: Enchant Boots - Lesser Stamina',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100163,0,-1),
  (102024,0,0,'Scroll: Enchant Boots - Lesser Intellect',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100164,0,-1),
  (102025,0,0,'Scroll: Enchant Boots - Lesser Spirit',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100165,0,-1),
  (102026,0,0,'Scroll: Enchant Boots - Stamina',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100166,0,-1),
  (102027,0,0,'Scroll: Enchant Boots - Minor Speed',1093,2,64,0,0,0,-1,-1,30,20,10,0,1,7,0,100167,0,-1),
  (102028,0,0,'Scroll: Enchant Gloves - Skinning',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100168,0,-1),
  (102029,0,0,'Scroll: Enchant Gloves - Agility',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100169,0,-1),
  (102030,0,0,'Scroll: Enchant Gloves - Advanced Mining',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100170,0,-1),
  (102031,0,0,'Scroll: Enchant Gloves - Strength',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100171,0,-1),
  (102032,0,0,'Scroll: Enchant Gloves - Advanced Herbalism',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100172,0,-1),
  (102033,0,0,'Scroll: Enchant Shield - Lesser Stamina',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100173,0,-1),
  (102034,0,0,'Scroll: Enchant Shield - Spirit',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100174,0,-1),
  (102035,0,0,'Scroll: Enchant Shield - Lesser Block',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100175,0,-1),
  (102036,0,0,'Scroll: Enchant Shield - Stamina',1093,2,64,0,0,0,-1,-1,30,0,10,0,1,7,0,100176,0,-1);

-- ---------------------------------------------------------------- The shop
-- A goods shop (shop_type 1), opened from a dialog button "Opens a shop".
INSERT INTO shop (id, name, shop_type, comment) VALUES
  (200900, 'Rune Chip scrolls', 1, 'sql/custom/168. Enchants to skill 225, 7-150 Rune Chips by skill.');

-- One merchant row and one cost row a scroll, both with id 2009000000 + slot
-- (lib/shops.js). The shop lists the scrolls by slot type (grp), then by
-- skill, so the new scrolls sit beside the old ones of the same slot.
--   grp: 1 weapon, 2 2H weapon, 3 chest, 4 bracer, 5 cloak, 6 boots,
--        7 gloves, 8 shield.
-- THE COST GROWS WITH THE ENCHANT'S SKILL: 7 x (150 / 7) ^ ((skill - 15) / 210),
-- rounded. Skill 15 costs 7, 75 costs 17, 150 costs 50, 225 costs 150: each
-- 50 skill about doubles the price. The skill is the enchanting skill the
-- recipe needs, from the trainer or the formula.
CREATE TEMPORARY TABLE scroll_skill (
  entry mediumint unsigned PRIMARY KEY, grp tinyint unsigned, skill smallint unsigned);
INSERT INTO scroll_skill (entry, grp, skill) VALUES
  (100037,1, 90), (100038,1, 90), (100039,1,140),
  (102000,1,175), (102001,1,175), (102002,1,190), (102003,1,195),
  (100040,2, 75), (100041,2,100), (100042,2,100), (100043,2,110), (100044,2,145),
  (102004,2,200),
  (100045,3, 15), (100046,3, 20), (100047,3, 40), (100048,3, 60), (100049,3, 80),
  (100050,3,120), (100051,3,140), (100052,3,145), (100053,3,150),
  (102005,3,160), (102006,3,185), (102007,3,200), (102008,3,220),
  (100054,4, 20), (100055,4, 50), (100056,4, 60), (100057,4, 80), (100058,4, 80),
  (100059,4,120), (100060,4,130), (100061,4,140), (100062,4,150),
  (102009,4,165), (102010,4,170), (102011,4,170), (102012,4,180), (102013,4,185),
  (102014,4,185), (102015,4,210), (102016,4,220),
  (100063,5, 45), (100064,5, 70), (100065,5,110), (100066,5,115), (100067,5,125),
  (100068,5,135),
  (102017,5,155), (102018,5,175), (102019,5,205), (102020,5,205), (102021,5,225),
  (100069,6,125), (100070,6,125),
  (102022,6,160), (102023,6,170), (102024,6,170), (102025,6,190), (102026,6,215),
  (102027,6,225),
  (100071,7,145), (100072,7,145), (100073,7,145),
  (102028,7,200), (102029,7,210), (102030,7,215), (102031,7,225), (102032,7,225),
  (100074,8,105), (100075,8,115), (100076,8,130),
  (102033,8,155), (102034,8,180), (102035,8,195), (102036,8,210);

CREATE TEMPORARY TABLE scroll_slot
SELECT entry, skill, ROW_NUMBER() OVER (ORDER BY grp, skill, entry) AS slot FROM scroll_skill;

INSERT INTO itemextendedcost (id, requiredItem1, requiredItemCount1)
SELECT 2009000000 + slot, 100036, ROUND(7 * POW(150 / 7, (skill - 15) / 210))
  FROM scroll_slot;

INSERT INTO custom_merchant (id, entry, slot, item, count, extendedcost, condition_id)
SELECT 2009000000 + slot, 200900, slot, entry, 1, 2009000000 + slot, 0
  FROM scroll_slot;

DROP TEMPORARY TABLE scroll_slot;
DROP TEMPORARY TABLE scroll_skill;

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
