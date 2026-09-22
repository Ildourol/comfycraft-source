-- ============================================================================
--  A DURABLE PER-CHARACTER FACT  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\106_story_flags.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  APPLY BEFORE THE BINARY THAT READS IT. Player::_LoadStoryFlags selects from
--  this table inside LoadFromDB, and a failed query on this core asserts and
--  writes a crash dump rather than warning. 087, 093, 099 and 100 all record
--  the same rule.
--
--  ------------------------------------------------------- WHY THIS EXISTS
--
--  Everything a bot NPC does today is forgotten the moment it finishes. The
--  timeline runs, the line is said, and a minute later the world is exactly as
--  it was. This is the other half: a fact about ONE CHARACTER that outlives the
--  script which set it, so an NPC can hold a quest back until you have greeted
--  her, and a gossip button can appear only for somebody who has.
--
--  ------------------------------------------------- WHY NOT A HIDDEN QUEST
--
--  A quest used as a flag was the first design and it does work.
--  Player::CompleteQuest (Player.cpp) happily completes a quest the player
--  never took: SetQuestStatus default-constructs the status row and the
--  log-slot write is guarded, so there is a real silent-completion path.
--
--  What it costs is the problem. The flag quest takes a quest log slot, fires
--  the completion toast and sound at somebody who accepted nothing, appears in
--  /database as a phantom step in the chain, and -- the one that cannot be
--  undone -- SatisfyQuestStatus then refuses that quest FOR EVER, because it
--  refuses anything whose status is not QUEST_STATUS_NONE.
--
--  A flag is the thing we actually meant. It has no side effects, any dialog
--  button can set one, and it reads back through `conditions` like every other
--  gate on this server.
--
--  ---------------------------------------------------------- THE SHAPE
--
--  A ROW IS "THIS CHARACTER HAS THIS FLAG". There is no value and no counter:
--  a flag is set or it is absent, which is what makes clearing one a DELETE and
--  makes the whole table safe to read as a set. Something that needs to count
--  is not a flag and should not be bent into one.
--
--  `set_at` is not read by anything. It is there because the first question
--  anybody asks of a table like this is "when did that happen", and the answer
--  is otherwise gone.
--
--  ------------------------------------- DELETING A CHARACTER MUST SWEEP IT
--
--  CHARACTER GUIDS ARE REUSED HERE, and a companion is created per hire and
--  deleted per dismiss, so this is not a rare path. Player::DeleteFromDB gains
--  a DELETE for this table in the same change that adds it -- CLAUDE.md records
--  the three dormant crashes that turned reliable on 2026-09-06 for exactly
--  this reason, and 067 is the sweep written after the fact that time.
--
--  A row left behind would not crash: this table has no primary-key collision
--  to hit, because the new character would simply find the row already there.
--  It is worse than a crash. A brand new character would silently inherit
--  somebody else's story, and the only evidence would be a quest that was
--  already available to a level one who had never met the NPC.
--
--  Re-runnable: CREATE TABLE IF NOT EXISTS, and nothing here is seeded.
-- ============================================================================

CREATE TABLE IF NOT EXISTS `character_story_flag` (
    `guid`   INT(10) UNSIGNED NOT NULL DEFAULT 0
             COMMENT 'characters.guid',
    `flag`   INT(10) UNSIGNED NOT NULL DEFAULT 0
             COMMENT 'story_flag.id in tw_world, 300000-300999',
    `set_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
             COMMENT 'read by nothing. Here because it is the first thing anybody asks',
    PRIMARY KEY (`guid`, `flag`),
    KEY `flag` (`flag`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci
  COMMENT='What a character has done that something else is waiting on';
