-- tw_logon + tw_web. The realm is Brill, with id 1, on every box.
--    Get-Content sql\custom\122_realm_brill.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_logon
--
-- Both changes were made here by hand on 2026-09-17 and never became a file,
-- so no deploy carried them. On 2026-09-24 the VPS still had the row as
-- id 2, 'Tortoise', and the site still said Tortoise.
-- docs/ai/server/realm.md has the reasons.
--
-- 1. mangosd.conf says RealmID = 1, so the row must be id 1. mangosd writes
--    realmcharacters and the row's icon under RealmID; realmd reads them under
--    the row's id. With the two apart, realm select shows 0 characters and PvP.
-- 2. The name. realmd rereads realmlist every 10 s; no restart.
-- 3. realmcharacters is recounted from tw_char, because the rows under the
--    old id are the only complete ones (the VPS had 505 under 2, 5 under 1).
--
-- The renames touch only a row that still says Tortoise, so a re-run does
-- not undo a later rename. Re-runnable. Run it with mangosd down: it writes
-- realmcharacters when a character is made or deleted.

SET @has1 = (SELECT COUNT(*) FROM tw_logon.realmlist WHERE id = 1);
UPDATE tw_logon.realmlist SET id = 1 WHERE id = 2 AND @has1 = 0;

UPDATE tw_logon.realmlist SET name = 'Brill' WHERE id = 1 AND name = 'Tortoise';
UPDATE tw_web.web_setting SET value = 'Brill' WHERE name = 'realm_name' AND value = 'Tortoise';

DELETE FROM tw_logon.realmcharacters
WHERE realmid NOT IN (SELECT id FROM tw_logon.realmlist);

REPLACE INTO tw_logon.realmcharacters (realmid, acctid, numchars)
SELECT 1, a.id, LEAST(COUNT(c.guid), 255)
FROM tw_logon.account a
LEFT JOIN tw_char.characters c ON c.account = a.id
GROUP BY a.id;
