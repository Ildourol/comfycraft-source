-- ============================================================================
--  Fashion Demands Sacrifices (level 10): no Light Leather, 1 Malachite
--  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\142_fashion_level_10.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console, no restart:
--
--    reload quest_template
--
--  Upstream 60030 asks for 5 Linen Cloth, 5 Light Leather, 5 Malachite and
--  5 Small Leather Collar. Light Leather (2318) comes only from Skinning, so
--  a level 10 character without it cannot finish the quest alone. The item is
--  removed and the two after it move up one slot.
--
--  Malachite (774) drops from Elwynn's Prowler at 1.3% and from a Copper
--  Vein at 1%, and no vendor sells it. Five is about 400 kills, so the
--  quest asks for one:
--
--    1  Linen Cloth (2589)           5
--    2  Malachite (774)              1
--    3  Small Leather Collar (4813)  5
--
--  The quest text names no items. A character with the quest in progress
--  when this lands has its counts in the old slots: abandon and take it again.
--  Re-runnable.
-- ============================================================================

UPDATE quest_template
   SET ReqItemId1 = 2589, ReqItemCount1 = 5,
       ReqItemId2 = 774,  ReqItemCount2 = 1,
       ReqItemId3 = 4813, ReqItemCount3 = 5,
       ReqItemId4 = 0,    ReqItemCount4 = 0
 WHERE entry = 60030;
