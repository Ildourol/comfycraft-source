-- ============================================================================
--  A FIFTH TRIGGER: SOMEBODY EMOTED AT IT  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\108_bot_npc_idle_emote.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  APPLY BEFORE THE BINARY THAT READS IT, the same rule as 100 and 101. A
--  missing column is a failed query inside World::SetInitialWorldSettings, and
--  a failed query on this core asserts and writes a crash dump.
--
--  ------------------------------------------------------- WHY THIS EXISTS
--
--  All four existing triggers are moments the SERVER picks: a period, an
--  arrival, the end of a conversation, the NPC coming into the world. Not one
--  of them is a player deciding to address the NPC.
--
--  This one is. /bow at the priestess and she bows back. The 1.12 client's
--  CMSG_TEXT_EMOTE carries three fields -- the EmotesText id, the variant, and
--  THE GUID OF WHATEVER THE PLAYER HAD TARGETED -- so the server already knows
--  who the emote was aimed at, and has since 2005. Nothing was listening.
--
--  --------------------------------------- WHY IT IS A LIST AND THE OTHERS ARE NOT
--
--  The other four are one per design and the table says so. This one is not:
--  one NPC wanting to answer /wave, /bow and /dance differently is the ordinary
--  case, not an exotic one, so an emote rule carries WHICH EMOTE and a design
--  may have as many as it likes.
--
--  THE UNIQUE KEY IS WIDENED RATHER THAN DROPPED. (bot_npc_id, trigger_kind)
--  becomes (bot_npc_id, trigger_kind, emote_id), and because the four old kinds
--  always carry emote_id = 0 they stay exactly one-per-design. One key, two
--  meanings, both enforced -- which is better than removing the constraint that
--  was protecting the first four in order to let the fifth repeat.
--
--  ----------------------------------------- A COLUMN RATHER THAN A TABLE
--
--  `bot_npc_idle` already has the loader, the save path, the delete-with-the-NPC
--  sweep, the reload and the id blocks. A second table would need a second copy
--  of all five, and the fifth one -- deleting an NPC's rules when the NPC goes
--  -- is the one that is silently wrong when it is forgotten.
--
--  ------------------------------------------------------ WHAT IT POINTS AT
--
--  `script_id` as before, a generic_scripts id in 6450000-6459999, with spoken
--  lines in broadcast_text 6460000-6469999. Nothing about the id blocks, the
--  BOT_NPC_IDLE_SCRIPT_MIN/MAX range test in BotNpc.h, or the reload order
--  changes.
--
--  `period` is the cooldown, and IT IS NOT OPTIONAL. Nothing in this core
--  throttles emotes: CanSpeak is a mute check, a text emote never touches the
--  ChatFlood counter, and the only other ceiling is 300 packets per session
--  update. A held-down /bow macro arrives several times a second. Housing's
--  critter pat reached the same conclusion and settled on three seconds.
--
--  `radius` is UNUSED on an emote row. The trigger is targeted-only, and the
--  core applies the text-emote listen range on top, which is exactly the set of
--  people who saw the emote happen.
--
--  ---------------------------------------------------------------- RELOADS
--
--  Unchanged, and still in this order:
--
--    reload broadcast_text
--    reload generic_scripts
--    reload dialog_scenes
--    reload bot_npc
--
--  Emote rules live in this table, so `reload bot_npc` already covers them.
--
--  Re-runnable: every change is guarded, because ALTER TABLE has no
--  IF NOT EXISTS for columns on this MariaDB and re-MODIFYing an ENUM that is
--  already right is a table rebuild for nothing.
-- ============================================================================

DELIMITER $$

DROP PROCEDURE IF EXISTS `add_bot_npc_idle_emote` $$
CREATE PROCEDURE `add_bot_npc_idle_emote`()
BEGIN
    IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
                   WHERE TABLE_SCHEMA = DATABASE()
                     AND TABLE_NAME = 'bot_npc_idle'
                     AND COLUMN_NAME = 'emote_id') THEN
        ALTER TABLE `bot_npc_idle`
            ADD COLUMN `emote_id` INT(10) UNSIGNED NOT NULL DEFAULT 0
                COMMENT 'emote only: the EmotesText.dbc id it answers. 0 on every other kind'
                AFTER `radius`;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
                   WHERE TABLE_SCHEMA = DATABASE()
                     AND TABLE_NAME = 'bot_npc_idle'
                     AND COLUMN_NAME = 'trigger_kind'
                     AND COLUMN_TYPE LIKE '%emote%') THEN
        ALTER TABLE `bot_npc_idle`
            MODIFY COLUMN `trigger_kind`
                ENUM('loop','near','after_talk','custom','emote') NOT NULL DEFAULT 'loop'
                COMMENT 'what sets it off. `custom` sets itself off; `emote` is the only one that may repeat';
    END IF;

    -- The widening. Guarded on emote_id being absent from the key rather than
    -- on the key existing, so re-running finds it already done.
    IF NOT EXISTS (SELECT 1 FROM information_schema.STATISTICS
                   WHERE TABLE_SCHEMA = DATABASE()
                     AND TABLE_NAME = 'bot_npc_idle'
                     AND INDEX_NAME = 'one_per_trigger'
                     AND COLUMN_NAME = 'emote_id') THEN
        ALTER TABLE `bot_npc_idle`
            DROP INDEX `one_per_trigger`,
            ADD UNIQUE KEY `one_per_trigger` (`bot_npc_id`, `trigger_kind`, `emote_id`);
    END IF;
END $$

DELIMITER ;

CALL `add_bot_npc_idle_emote`();
DROP PROCEDURE `add_bot_npc_idle_emote`;
