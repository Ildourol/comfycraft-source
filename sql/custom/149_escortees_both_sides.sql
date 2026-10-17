-- ============================================================================
--  Escort NPCs are friendly to both sides  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\149_escortees_both_sides.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console: reload creature_template, reload
--  quest_start_scripts. A creature that is already spawned keeps its old
--  faction until it respawns or the server restarts.
--
--  Faction 40 (Escortee) has a template for each side and one for any player.
--  A side's template counts as that side, so the other side's players are
--  hostile to it: a Horde player could not talk to the Defias Traitor to
--  start "The Defias Brotherhood". The faction has no reputation bar, so the
--  factionless peace cannot reach it.
--
--    10, 33    -> 113   passive escortee
--    231, 232  -> 250   escortee that fights monsters
--    774, 775  -> 290   holiday revelers and fire eaters
--
--  Each pair has the same flags as its target. The core makes the same change
--  in Unit::SetFactionTemplateId, which also covers the C++ escort scripts;
--  this file makes the data say it too. Re-runnable.
-- ============================================================================

UPDATE `creature_template` SET `faction` = 113 WHERE `faction` IN (10, 33);
UPDATE `creature_template` SET `faction` = 250 WHERE `faction` IN (231, 232);
UPDATE `creature_template` SET `faction` = 290 WHERE `faction` IN (774, 775);

-- Set Faction (command 22) when an escort starts: Deathstalker Faerleia (452),
-- Ranshalla (4901), Kanati Greycloud (4966).
UPDATE `quest_start_scripts` SET `datalong` = 113 WHERE `command` = 22 AND `datalong` IN (10, 33);
UPDATE `quest_start_scripts` SET `datalong` = 250 WHERE `command` = 22 AND `datalong` IN (231, 232);
