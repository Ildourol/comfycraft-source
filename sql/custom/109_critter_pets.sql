-- ============================================================================
--  EVERY CRITTER A CHARACTER HAS PATTED  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\109_critter_pets.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  APPLY BEFORE THE BINARY THAT READS IT. The login query holder selects from
--  this table for every character, and a failed query on this core asserts
--  and writes a crash dump rather than warning.
--
--  ---------------------------------------------------------- WHAT IT IS FOR
--
--  The critter chart at /charts: most KINDS of critter patted. The spec is
--  docs/features/critter-chart.md.
--
--  A ROW IS ONE SPAWN, AND THE CHART COUNTS NAMES. Scoring by spawn would hand
--  the chart to whoever runs laps of Elwynn (711 of the 3,755 critter spawns
--  are rabbits); scoring by kind hands it to whoever found the rare ones. The
--  spawn is kept anyway because it costs nothing and leaves room for a second
--  board later without a migration.
--
--  `entry` is here rather than joined from tw_world.creature because a house
--  critter is not in that table, and because a world re-import can renumber
--  spawns. The chart counts DISTINCT creature_template.name through `entry`, so
--  the three Toad entries are one toad.
--
--  `spawn` is only ever a guid that survives a restart: a `creature` row, or a
--  house critter in the 8,000,000-8,100,000 block. Anything summoned gets a
--  fresh guid each time and the core does not record it at all.
--
--  ------------------------------------- DELETING A CHARACTER MUST SWEEP IT
--
--  CHARACTER GUIDS ARE REUSED and companions are created and deleted all the
--  time. Player::DeleteFromDB gains a DELETE for this table in the same change.
--  A row left behind would not crash (the core writes with INSERT IGNORE), it
--  would hand a brand new character somebody else's score.
--
--  Re-runnable: CREATE TABLE IF NOT EXISTS, and nothing here is seeded. There
--  is no backfill; nothing was recorded before this.
-- ============================================================================

CREATE TABLE IF NOT EXISTS `character_critter_pet` (
    `guid`         INT(10) UNSIGNED NOT NULL DEFAULT 0
                   COMMENT 'characters.guid',
    `spawn`        INT(10) UNSIGNED NOT NULL DEFAULT 0
                   COMMENT 'creature guid: a tw_world.creature row, or a house critter',
    `entry`        INT(10) UNSIGNED NOT NULL DEFAULT 0
                   COMMENT 'creature_template.entry; the chart counts its name',
    `first_pet_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`guid`, `spawn`),
    KEY `entry` (`entry`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci
  COMMENT='The critter chart: every critter spawn a character has patted';
