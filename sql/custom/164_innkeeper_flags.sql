-- ============================================================================
--  INNKEEPERS WITHOUT THE INNKEEPER FLAG  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\164_innkeeper_flags.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then `reload creature_template` in the console. A creature already spawned
--  keeps its old flags until it respawns or the server restarts.
--
--  Upstream added these four with the subname Innkeeper but without
--  UNIT_NPC_FLAG_INNKEEPER (128). Without it the default gossip option "Make
--  this inn your home." is hidden, and the bind packet is refused.
--
--    51650  Volatala Zilmbu
--    51662  Ed "Maddog" McLoud
--    60652  Waya Tallgrain
--    81057  Eliana Firestorm
--
--  The OR leaves every other flag as it is, so the script is re-runnable.
-- ============================================================================

UPDATE creature_template SET npc_flags = npc_flags | 128
WHERE entry IN (51650, 51662, 60652, 81057);
