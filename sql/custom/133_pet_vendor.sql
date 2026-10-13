-- ============================================================================
--  Custom NPC: every companion pet, free  (tw_world)
-- ============================================================================
--  Pet Collector (creature 100039). A GM tool for testing pets, with no spawn.
--  Summon it with:  .npc summon 100039
--  (.npc add saves a spawn to `creature`; .npc summon lasts until a restart.)
--
--  The stock is every item in collection_pet that exists in item_template,
--  read when this file is applied: 226 on 2026-10-10. Apply it again after a
--  pet is added. A window shows at most 128 items (MAX_VENDOR_ITEMS), so the
--  list is two shops behind two gossip buttons, split by the item name's
--  first letter: A to L (113) and M to Z (113).
--
--  Every row points at one vendor_cost row with money 0 and no items, which
--  is "free". cost_id 0 would charge the item's own buy_price.
--
--  IDS: creature 100039 | shops 200002, 200003 | menu 64110
--       npc_text / broadcast_text 6400320
-- ============================================================================

DELETE FROM npc_vendor_template WHERE entry IN (200002, 200003);
DELETE FROM shop                WHERE id    IN (200002, 200003);
DELETE FROM vendor_cost         WHERE comment = 'Pet Collector: free';
DELETE FROM gossip_menu_option  WHERE menu_id = 64110;
DELETE FROM gossip_menu         WHERE entry   = 64110;
DELETE FROM npc_text            WHERE ID      = 6400320;
DELETE FROM broadcast_text      WHERE entry   = 6400320;
DELETE FROM creature_template   WHERE entry   = 100039;

INSERT INTO vendor_cost (money, comment) VALUES (0, 'Pet Collector: free');
SET @free = LAST_INSERT_ID();

INSERT INTO shop (id, name, shop_type, comment) VALUES
  (200002, 'Pets A to L', 0, 'Pet Collector (100039), sql/custom/133'),
  (200003, 'Pets M to Z', 0, 'Pet Collector (100039), sql/custom/133');

-- Some items teach two spells (Tirisfal Bat), so take each item once.
INSERT INTO npc_vendor_template (entry, slot, item, maxcount, incrtime, itemflags, condition_id, cost_id)
SELECT IF(LEFT(p.name, 1) <= 'L', 200002, 200003),
       ROW_NUMBER() OVER (PARTITION BY LEFT(p.name, 1) <= 'L' ORDER BY p.name, p.entry) - 1,
       p.entry, 0, 0, 0, 0, @free
FROM (SELECT DISTINCT i.entry, i.name
      FROM collection_pet c
      JOIN item_template i ON i.entry = c.itemId) p;

INSERT INTO broadcast_text (entry, male_text, female_text, chat_type, language_id) VALUES
  (6400320, 'Every pet in the world, and all of them free. Pick a shelf.',
            'Every pet in the world, and all of them free. Pick a shelf.', 0, 0);

INSERT INTO npc_text (ID, BroadcastTextID0, Probability0) VALUES (6400320, 6400320, 1);

INSERT INTO gossip_menu (entry, text_id) VALUES (64110, 6400320);

-- option_id 3 is GOSSIP_OPTION_VENDOR, npc_option_npcflag 4 the vendor flag,
-- and action_menu_id the shop. Icon 1 is the bag.
INSERT INTO gossip_menu_option
  (menu_id, id, option_icon, option_text, option_id, npc_option_npcflag, action_menu_id)
VALUES
  (64110, 0, 1, 'Pets A to L', 3, 4, 200002),
  (64110, 1, 1, 'Pets M to Z', 3, 4, 200003);

-- npc_flags 5 is gossip and vendor. The model is Donni Anthania's (6367).
INSERT INTO creature_template
  (entry, name, subname, display_id1, faction, npc_flags, gossip_menu_id,
   level_min, level_max, health_min, health_max, armor, unit_class, type,
   speed_walk, speed_run, scale, base_attack_time, ranged_attack_time,
   dmg_min, dmg_max, movement_type, inhabit_type, regeneration, ai_name)
VALUES
  (100039, 'Pet Collector', 'Every Pet', 5086, 35, 5, 64110,
   60, 60, 4007, 4007, 3272, 1, 7,
   1, 1.14286, 1, 2000, 2000,
   103.4, 129.8, 0, 3, 3, '');
