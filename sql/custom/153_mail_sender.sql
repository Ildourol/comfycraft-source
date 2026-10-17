-- ============================================================================
--  THE MAIL SENDER: "ComfyCraft Staff"  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\153_mail_sender.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  `send letter` (the Workshop's Mail page, the bug report reward) sends as
--  this creature, so the mailbox shows "ComfyCraft Staff" as the sender. A
--  GM letter from no one shows "Blizzard". The client asks for the name with
--  a creature query, the way it does for the auction house and quest NPCs.
--
--  Never spawned. The row is a copy of Helper Elowen (100009) with the name
--  changed and the gossip, flags and scripts cleared. The entry is named in
--  Commands.cpp (MAIL_SENDER_ENTRY) and website/lib/mail.js.
-- ============================================================================

DELETE FROM creature_template WHERE entry = 100041;

CREATE TEMPORARY TABLE mail_sender SELECT * FROM creature_template WHERE entry = 100009;
UPDATE mail_sender SET entry = 100041, name = 'ComfyCraft Staff', subname = '',
       gossip_menu_id = 0, npc_flags = 0, trainer_id = 0, vendor_id = 0,
       ai_name = '', script_name = '', loot_id = 0, pickpocket_loot_id = 0,
       skinning_loot_id = 0, gold_min = 0, gold_max = 0, equipment_id = 0;
INSERT INTO creature_template SELECT * FROM mail_sender;
DROP TEMPORARY TABLE mail_sender;
