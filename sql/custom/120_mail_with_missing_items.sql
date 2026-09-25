-- tw_char. Deletes mails whose attached items no longer exist in item_instance.
--    Get-Content sql\custom\120_mail_with_missing_items.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
-- Orcy had 64 "problems with equipping/storing" mails from 2026-09-14, each
-- holding a Soulforge armour piece (22086-22093) with no item_instance row.
-- Such a mail shows an attachment that cannot be taken.
--
-- Only a mail whose EVERY item is missing is deleted; a mail with one live item
-- is left alone. Run it only while the receivers are offline: a logged-in
-- character holds its mail in memory and saves it back. Re-runnable.

DROP TEMPORARY TABLE IF EXISTS dead_mail;
CREATE TEMPORARY TABLE dead_mail AS
SELECT m.id AS mail_id, m.itemTextId AS text_id
FROM mail m
WHERE EXISTS (SELECT 1 FROM mail_items mi WHERE mi.mail_id = m.id)
  AND NOT EXISTS (SELECT 1 FROM mail_items mi JOIN item_instance ii ON ii.guid = mi.item_guid
                  WHERE mi.mail_id = m.id);

DELETE it FROM item_text it JOIN dead_mail d ON d.text_id = it.id WHERE d.text_id <> 0;
DELETE mi FROM mail_items mi JOIN dead_mail d ON d.mail_id = mi.mail_id;
DELETE m FROM mail m JOIN dead_mail d ON d.mail_id = m.id;

DROP TEMPORARY TABLE dead_mail;
