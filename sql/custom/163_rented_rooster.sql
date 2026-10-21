-- ============================================================================
--  A rentable riding chicken  (tw_world)
-- ============================================================================
--  Follow-on from 079. The chicken mount is spell 45007 "Magic Rooster":
--  instant, permanent, +mounted speed, model Creature\Chickenmount (display
--  18094). Creature 40005 "Swift Rooster" wears the same display and is the
--  creature the spell names, so the animal you click and the animal you ride
--  are the same by construction. 40005 is one of Turtle's display stand-ins,
--  like 40007-40045 that 079 already made rentable.
--
--  IT IS CLAMPED LIKE EVERYTHING ELSE: 50 copper, rental speed by level,
--  5 minutes.
--
--  IT IS NOT SPAWNED. 40005 has no `creature` row. Stand where it should be
--  and type `.npc add 40005`, which writes its own row.
--
--  RELOAD, IN THIS ORDER, LIVE:
--    broadcast_text, npc_text, creature_template, rented_mount
--  Re-running 079 deletes this row and greeting 100610. Run this file again
--  after it.
-- ============================================================================

-- -- greeting ----------------------------------------------------------------

DELETE FROM `broadcast_text` WHERE `entry` = 100610;
INSERT INTO `broadcast_text` (`entry`, `male_text`, `chat_type`, `language_id`) VALUES
  (100610, 'The rooster puffs out its chest and scratches at the dirt. It looks ready to run.', 0, 0);

DELETE FROM `npc_text` WHERE `ID` = 100610;
INSERT INTO `npc_text` (`ID`, `BroadcastTextID0`, `Probability0`) VALUES
  (100610, 100610, 1);

-- -- rentable ------------------------------------------------------------------

DELETE FROM `rented_mount` WHERE `creature_entry` = 40005;
INSERT INTO `rented_mount` (`creature_entry`, `spell_id`, `cost`, `speed`, `duration`, `text_id`, `noun`) VALUES
  (40005, 45007, 50, 40, 300000, 100610, 'rooster');

UPDATE `creature_template` SET `npc_flags` = `npc_flags` | 1, `script_name` = 'rented_mount'
WHERE `entry` = 40005;

-- -- check -------------------------------------------------------------------
--  Expect 1, 1, 1 and 0. The last is UNIT_FLAG_NOT_SELECTABLE, which makes a
--  renter you cannot click.

SELECT
  (SELECT COUNT(*) FROM `rented_mount` WHERE `creature_entry` = 40005) AS rentable,
  (SELECT COUNT(*) FROM `creature_template` WHERE `entry` = 40005
     AND `script_name` = 'rented_mount' AND (`npc_flags` & 1) = 1) AS wired,
  (SELECT COUNT(*) FROM `npc_text` WHERE `ID` = 100610) AS greeting,
  (SELECT COUNT(*) FROM `creature_template` WHERE `entry` = 40005
     AND (`unit_flags` & 0x02000000) <> 0) AS not_selectable;
