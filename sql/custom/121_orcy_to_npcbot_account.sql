-- tw_char + tw_logon. Moves the bot NPC Orcy (guid 4644) off the owner's account
-- (510, ALOOFBIT) onto RNDBOT501 (519), where .npcbot place puts new bodies.
--    Get-Content sql\custom\121_orcy_to_npcbot_account.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
-- Orcy was made from the owner's own character, so it stayed on their account.
-- PlayerbotAIConfig::loadBotNpcs stands a body up from bot_npc_spawn whatever
-- its account, so the move changes nothing about how Orcy appears.
--
-- Run it only with mangosd DOWN. A bot NPC is always online, and a save writes
-- characters.account back (Player::SaveToDB is a REPLACE). Re-runnable.
--
-- The move happens only where account 519 is RNDBOT501. The VPS had no such
-- account on 2026-09-24 (its RNDBOT accounts stop at 512), so there the file
-- does nothing and Orcy stays on account 510.

UPDATE tw_char.characters SET account = 519
WHERE guid = 4644 AND name = 'Orcy'
  AND EXISTS (SELECT 1 FROM tw_logon.account WHERE id = 519 AND username = 'RNDBOT501');

-- The character list on the realm screen reads these counts.
UPDATE tw_logon.realmcharacters rc
SET rc.numchars = (SELECT COUNT(*) FROM tw_char.characters c WHERE c.account = rc.acctid)
WHERE rc.acctid IN (510, 519) AND rc.realmid = 1;
