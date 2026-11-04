-- ============================================================================
--  FOUR JUG CRATES IN "FURNITURE: ODDS & ENDS"  (tw_world, bug tracker 110)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\234_jug_furniture.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console, in this order:
--
--    reload item_template
--    reload npc_vendor
--    house furniture reload
--
--  No restart is needed.
--
--  THE JUGS. One crate for each jug model in the client, at 20s (a vendor
--  buys one back for 4s, sql/custom/130):
--
--    Wicker Jug         gameobject 180332, jug01.mdx     (display 213)
--    Round Wicker Jug   gameobject 180333, jug02.mdx     (display 7)
--    Orcish Jug         gameobject 180350, OrcJug01.mdx  (display 458)
--    Round Orcish Jug   gameobject 180351, OrcJug02.mdx  (display 459)
--
--  All four are type 5. The stock entries are used, not Turtle's path-named
--  copies (2001881, 2001882, 2003875, 2003876), which are the same models.
--  The shelf category is "Odds & Ends": categorise() answers Misc for all
--  four, and shelfBucket() rolls Misc up to Odds & Ends.
--
--  THE ROWS ARE THE ONES /admin/furniture MAKES (website/lib/furniture.js
--  create and insert). The item is a copy of crate 100100 with the entry,
--  name, price and description changed. The description ends in the model
--  tag [m<displayId>], which ComfyUI's Crates.lua reads for the picture.
--
--  NO ITEM ENTRY IS WRITTEN HERE. Production makes crates on the page, so a
--  fixed number could collide with a crate that exists only there. Each jug
--  takes MAX(entry) + 1 over 100100-100199 and 100400-100999 when this runs,
--  as nextEntry() does, and goes on at 100400 after 100199.
--
--  SAFE TO RUN AGAIN. A jug is skipped when:
--    - its name is already an item in 100100-100199 or 100400-100999,
--    - a crate already sells its display id (the page's rule),
--    - its gameobject is missing or is not type 5 or 7.
--  The shop row is written only on the run that makes the crate, so a re-run
--  does not put back a jug somebody took out in /admin/shops.
--
--  THE SHOP is found by name. Each jug goes in at MAX(slot) + 1, and none goes
--  in past 128 items (MAX_VENDOR_ITEMS). A jug left out by the limit, or made
--  on a server with no such shop, is a crate sold by nobody: add it in
--  /admin/shops.
--
--  It needs 042, 092, 095 and 219 first. The name column has item_template's
--  collation, so the name check compares like with like on every server.
-- ============================================================================


-- ---------------------------------------------------------------- the list
CREATE TEMPORARY TABLE comfy_jug_list (
  seq        tinyint unsigned   NOT NULL PRIMARY KEY,
  go_entry   mediumint unsigned NOT NULL,
  crate_name varchar(48) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL
);
INSERT INTO comfy_jug_list VALUES
  (1, 180332, 'Wicker Jug'),
  (2, 180333, 'Round Wicker Jug'),
  (3, 180350, 'Orcish Jug'),
  (4, 180351, 'Round Orcish Jug');


-- ---------------------------------------------------------------- the checks
--  r numbers the jugs that pass, 1 up, in list order.
CREATE TEMPORARY TABLE comfy_jug AS
  SELECT l.go_entry, l.crate_name, g.displayId AS display,
         ROW_NUMBER() OVER (ORDER BY l.seq) AS r
    FROM comfy_jug_list l
    JOIN gameobject_template g ON g.entry = l.go_entry
   WHERE g.type IN (5, 7)
     AND NOT EXISTS (SELECT 1 FROM item_template i
                      WHERE i.name = l.crate_name
                        AND (i.entry BETWEEN 100100 AND 100199 OR i.entry BETWEEN 100400 AND 100999))
     AND g.displayId NOT IN (SELECT sg.displayId FROM house_furniture_item f
                               JOIN gameobject_template sg ON sg.entry = f.go_entry);
DROP TEMPORARY TABLE comfy_jug_list;


-- ---------------------------------------------------------------- the numbers
--  nextEntry(): the highest entry in the two ranges, plus one. 100099 when
--  both are empty, so the first is 100100. Past 100199 the count jumps 200,
--  to 100400. Past 100999 the ranges are used up and the jug is dropped.
SET @top := (SELECT COALESCE(MAX(entry), 100099) FROM item_template
              WHERE entry BETWEEN 100100 AND 100199 OR entry BETWEEN 100400 AND 100999);

ALTER TABLE comfy_jug ADD COLUMN item_entry int unsigned NOT NULL DEFAULT 0;
UPDATE comfy_jug
   SET item_entry = @top + r + IF(@top <= 100199 AND @top + r > 100199, 200, 0);
DELETE FROM comfy_jug WHERE item_entry > 100999;


-- ---------------------------------------------------------------- the crates
--  Copied from crate 100100, as insert() does: the icon, the use spell and
--  the script come from that one row.
CREATE TEMPORARY TABLE comfy_crate AS
  SELECT j.item_entry AS new_entry, j.crate_name AS new_name, j.display AS new_display, i.*
    FROM comfy_jug j
    JOIN item_template i ON i.entry = 100100;
UPDATE comfy_crate SET entry = new_entry, name = new_name, buy_price = 2000, sell_price = 400,
       description = CONCAT('Right-click to place this in your house. [m', new_display, ']');
ALTER TABLE comfy_crate DROP COLUMN new_entry, DROP COLUMN new_name, DROP COLUMN new_display;
INSERT IGNORE INTO item_template SELECT * FROM comfy_crate;
DROP TEMPORARY TABLE comfy_crate;

--  Only for an item this run made: the join on the name leaves out a jug
--  whose item did not go in (no crate 100100 to copy).
INSERT IGNORE INTO house_furniture_item (item_entry, go_entry, scale, category)
  SELECT j.item_entry, j.go_entry, 0, 'Odds & Ends'
    FROM comfy_jug j
    JOIN item_template i ON i.entry = j.item_entry AND i.name = j.crate_name;


-- ---------------------------------------------------------------- the shop
SET @shop  := (SELECT id FROM shop
                WHERE name = 'Furniture: Odds & Ends' AND id BETWEEN 200000 AND 200999 AND shop_type = 0);
SET @slot  := (SELECT COALESCE(MAX(slot), 0) FROM npc_vendor_template WHERE entry = @shop);
SET @count := (SELECT COUNT(*) FROM npc_vendor_template WHERE entry = @shop);

INSERT IGNORE INTO npc_vendor_template (entry, slot, item, maxcount, incrtime)
  SELECT @shop, @slot + j.r, j.item_entry, 0, 0
    FROM comfy_jug j
    JOIN house_furniture_item f ON f.item_entry = j.item_entry AND f.go_entry = j.go_entry
   WHERE @shop IS NOT NULL AND @count + j.r <= 128;

DROP TEMPORARY TABLE comfy_jug;


-- ----------------------------------------------------------------------------
-- ROLLBACK: take the jugs out of the shop. The crates stay: a crate already
-- bought keeps its number, and a number is never reused.
--
-- DELETE t FROM npc_vendor_template t
--   JOIN item_template i ON i.entry = t.item
--   JOIN shop s ON s.id = t.entry
--  WHERE s.name = 'Furniture: Odds & Ends'
--    AND i.name IN ('Wicker Jug', 'Round Wicker Jug', 'Orcish Jug', 'Round Orcish Jug')
--    AND (i.entry BETWEEN 100100 AND 100199 OR i.entry BETWEEN 100400 AND 100999);
-- ----------------------------------------------------------------------------
