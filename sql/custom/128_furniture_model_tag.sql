-- ============================================================================
--  Housing: every furniture crate names its own model  (tw_world)
-- ============================================================================
--  A crate's description ends in a model tag:
--
--    Right-click to place this in your house. [m25588]
--
--  25588 is the gameobject display id of the model the crate places, and the
--  file name of its picture in ComfyUI's thumbs folder. ComfyUI reads the tag
--  off the item (addon/ComfyUI/Crates.lua) and strips it before drawing the
--  tooltip. The server sends an item's description the first time a client
--  sees the item, so a crate made at /admin/furniture on production carries
--  its own picture, with no addon rebuild. Without ComfyUI the tag shows.
--
--  The page writes the tag on every crate it makes; this file adds it to the
--  crates that already exist. Critter crates (house_critter_item) are not
--  touched: they show a creature, from their own table in the addon.
--
--  Re-runnable: the description is rewritten whole from the mapping, so a
--  second run changes nothing. Apply with:
--    Get-Content sql\custom\128_furniture_model_tag.sql | DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--  then `reload item_template`. A client that has seen the old text shows the
--  new one after a restart; the launcher clears the item cache on every start.
-- ============================================================================

UPDATE item_template i
  JOIN house_furniture_item f ON f.item_entry = i.entry
  JOIN gameobject_template g ON g.entry = f.go_entry
   SET i.description = CONCAT('Right-click to place this in your house. [m', g.displayId, ']')
 WHERE g.displayId > 0;
