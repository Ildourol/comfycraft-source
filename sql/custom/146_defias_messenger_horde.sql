-- ============================================================================
--  Defias Messenger: killable for Horde too  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\146_defias_messenger_horde.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console: reload creature_ai_events
--  (it reloads creature_ai_scripts first).
--
--  The Defias Messenger (550) spawns unkillable. Its aggro script 55001
--  tested the attacker's faction. Alliance (condition 3) got generic script
--  55004 (killable, combat on) and a say line. Horde (condition 2) got 55003:
--  a stun, a Defias Dark Wizard (93116, level 60 elite) for 3 s, then the
--  messenger vanished and respawned. Factions are not at war on this server,
--  so every attacker now gets the Alliance branch. 55003 stays, unused.
--  Re-runnable.
-- ============================================================================

DELETE FROM `creature_ai_scripts` WHERE `id` = 55001;
INSERT INTO `creature_ai_scripts` (`id`, `delay`, `priority`, `command`, `datalong`, `dataint`, `dataint2`, `dataint3`, `condition_id`, `comments`) VALUES
(55001, 0, 1, 39, 55004, 100, 0, 0, 0, 'Defias Messenger - Start Script to Become Killable'),
(55001, 0, 0, 0, 0, 479, 478, 473, 0, 'Defias Messenger - Say Text');
