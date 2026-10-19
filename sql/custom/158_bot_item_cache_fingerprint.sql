-- tw_char. The bot item info cache is loaded at start-up instead of rebuilt when
-- its inputs are unchanged (RandomItemMgr::LoadItemInfoCache).
--
-- source_ids: every quest, vendor or drop source of an item, comma separated. The
-- old sourceId column holds only the first, and the upgrade checks read them all.
-- ai_playerbot_cache_fingerprint: what the cache was built from (table checksums,
-- Spell.dbc, the code version). A different value means rebuild.
--
-- Apply before the binary that reads them. Re-runnable.
--
--  Get-Content sql\custom\158_bot_item_cache_fingerprint.sql | `
--    DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char

ALTER TABLE `ai_playerbot_item_info_cache`
    ADD COLUMN IF NOT EXISTS `source_ids` TEXT NULL AFTER `minLevel`,
    ROW_FORMAT = DYNAMIC;

CREATE TABLE IF NOT EXISTS `ai_playerbot_cache_fingerprint` (
    `name` VARCHAR(32) NOT NULL,
    `value` VARCHAR(1024) NOT NULL,
    PRIMARY KEY (`name`)
) ENGINE = MyISAM DEFAULT CHARSET = utf8mb3 COMMENT = 'PlayerbotAI cache input fingerprints';
