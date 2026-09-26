-- tw_char. Deletes the old random bot population at the next mangosd start.
--    Get-Content sql\custom\123_delete_random_bots.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
-- This queues the PlayerBots module's own cleanup. At startup,
-- RandomPlayerbotFactory::CreateRandomBots finds the 'bot_delete' row, deletes
-- every character on an RNDBOT account through Player::DeleteFromDB (a hard
-- delete, so CharDelete.Method = 1 keeps no rows), and removes the row. With
-- RandomBotAccountCount = 0 in aiplayerbot.conf, the same start then deletes
-- every RNDBOT account left empty.
--
-- THE BINARY MUST BE 2026-09-26 OR LATER. That build makes the cleanup skip
-- bot NPC bodies: .npcbot place puts them on RNDBOT accounts. An older binary
-- deletes every bot NPC body with the rest. The deploy applies sql/custom
-- after it installs the new binary and before it starts it, so a normal
-- deploy is safe. Do not apply this by hand on a box running an older build.
--
-- Measured locally on 2026-09-26: 4,584 characters and 500 accounts deleted
-- in 72 seconds of startup, 11 bot NPC bodies kept.
--
-- Value 1 keeps a bot that is on a player's friend list or in a guild a
-- player leads. Value 2 would delete whole accounts without that check.
--
-- Re-runnable: it queues nothing while a request is already waiting.

INSERT INTO tw_char.ai_playerbot_random_bots (owner, bot, time, event, value)
SELECT 0, 0, UNIX_TIMESTAMP(), 'bot_delete', 1
FROM DUAL
WHERE NOT EXISTS (SELECT 1 FROM tw_char.ai_playerbot_random_bots WHERE event = 'bot_delete');
