-- ============================================================================
--  Cleft of Shadow: the five Horde Affiliates NPCs are Orgrimmar  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\147_cleft_of_shadow_friendly.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console: reload creature_template. A creature that is
--  already spawned keeps its old faction until it respawns or the server
--  restarts.
--
--  Craven Drok, Gan'rul Bloodeye, Cazul, Zankaja and Therzok used faction
--  template 83 (Horde Affiliates, faction 66). Faction 66 has no reputation
--  bar, so the factionless peace standing cannot reach it, and template 83
--  is hostile to Alliance. An Alliance player saw them red and Therzok
--  attacked. Template 29 is the Orgrimmar townsfolk template: reputation
--  with Orgrimmar decides, and it has no contested-guard bit.
--
--  Template 83 itself is not changed. More than 100 other NPCs use it.
--  Each entry has one spawn, in Orgrimmar. Re-runnable.
-- ============================================================================

UPDATE `creature_template` SET `faction` = 29
 WHERE `entry` IN (5639, 5875, 5909, 5910, 6446);
