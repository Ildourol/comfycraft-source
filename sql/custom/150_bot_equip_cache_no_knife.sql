-- ============================================================================
--  BOT EQUIP CACHE: NO SKINNING KNIFE AS A WEAPON  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\150_bot_equip_cache_no_knife.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  Survivalist's Skinning Knife (7009) is Miscellaneous armor with a One-Hand
--  slot and weapon damage. The bot equip cache offered it to every class at
--  levels 1 to 21 in both hands. RandomItemMgr::CanEquipItem now refuses it,
--  but the cache is built only when the table is empty, so the old rows stay
--  until they are deleted here.
--
--  Item entries 1 to 4 (a headband and three mounts) also had stale rows for
--  the main and off hand.
--
--  The server reads the cache once at startup: apply, then restart. Safe to
--  run again.
-- ============================================================================

DELETE FROM `ai_playerbot_equip_cache`
 WHERE `item` = 7009
    OR (`item` IN (1, 2, 3, 4) AND `slot` IN (15, 16));
