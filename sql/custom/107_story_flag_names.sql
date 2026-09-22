-- ============================================================================
--  THE NAME BESIDE THE NUMBER  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\107_story_flag_names.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  ------------------------------------------------------- WHY THIS EXISTS
--
--  106 stores flags as numbers, because that is what a `conditions` row can
--  hold: value1 is an int and nothing more. A number is unreadable, so this is
--  the table that says 300000 is "greeted-mother".
--
--  EXACTLY THE SHAPE `shop` ALREADY HAS. A shop is an npc_vendor_template id
--  with a name beside it in tw_world.shop (092); a story flag is a number with
--  a name beside it here. Same reasoning, same database, and the website reads
--  both the same way.
--
--  NOTHING IN THE CORE READS THIS TABLE. The core deals in numbers: the script
--  command writes one and the condition tests one. The name exists so a person
--  can work, which is why it lives in tw_world beside the conditions it labels
--  rather than in tw_char beside the rows it counts.
--
--  ------------------------------------------------------------ THE BLOCK
--
--  Ids 300000-300999, ours, next to quests 100000-100999, shops 200000-200999
--  and bot questgivers 210000-210999. A number says which kind of thing it is,
--  which this repo has been glad of every time it has done it.
--
--  NOT AUTO_INCREMENT FROM 1. A flag id ends up in conditions.value1 and in
--  character_story_flag.flag, two tables that hold other people's numbers too,
--  and a bare 7 in either is a number with no owner.
--
--  ----------------------------------------------------------- THE NAME
--
--  A SLUG, NOT A SENTENCE. It is typed into a picker and read back in a gate
--  that says "remembers greeted-mother", so it wants to be short, lower case
--  and hyphenated -- an identifier, which is the one place CLAUDE.md's rule
--  against dashes does not reach. `note` is where the sentence goes, for
--  whoever finds the flag a year from now and cannot remember what set it.
--
--  UNIQUE on the name, because two flags called the same thing is a trap that
--  only shows up as a gate that never opens.
--
--  Re-runnable: CREATE TABLE IF NOT EXISTS, and nothing here is seeded --
--  flags are made on the website as they are needed.
-- ============================================================================

CREATE TABLE IF NOT EXISTS `story_flag` (
    `id`   INT(10) UNSIGNED NOT NULL DEFAULT 0
           COMMENT 'ours, 300000-300999',
    `name` VARCHAR(64) NOT NULL DEFAULT ''
           COMMENT 'a slug: short, lower case, hyphenated',
    `note` VARCHAR(255) NOT NULL DEFAULT ''
           COMMENT 'what sets it and what is waiting on it',
    PRIMARY KEY (`id`),
    UNIQUE KEY `name` (`name`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci
  COMMENT='What each story flag number means. The core never reads this';
