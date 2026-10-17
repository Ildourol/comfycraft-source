-- ============================================================================
--  Quest objects can be used by both sides  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\151_quest_objects_both_sides.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console: reload gameobject_template. An object that
--  is already spawned keeps its old faction until the server restarts.
--
--  Some quest objects carry a player race faction: Human (1), Orc (2), Night
--  Elf (4) or Gnome (115). These factions have no reputation bar, so the
--  factionless peace cannot reach them, and the client will not let the other
--  side use the object. A Horde player could take "Planting the Heart" (941)
--  but could not click Denalan's Planter to hand it in.
--
--  Questgivers (type 2) and chests (type 3) move to faction 35, which is
--  friendly to everyone. Traps, duel flags, mage portals and other objects
--  that a player places are left alone: they take the faction of the player
--  who places them. Re-runnable.
-- ============================================================================

UPDATE `gameobject_template` SET `faction` = 35
WHERE `type` IN (2, 3) AND `faction` IN (1, 2, 3, 4, 5, 6, 115, 116, 1609, 1610);
