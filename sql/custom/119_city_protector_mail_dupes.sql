-- tw_char. Removes the duplicate City Protector's Medallion mails.
--    Get-Content sql\custom\119_city_protector_mail_dupes.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
-- Player::MailCityProtectorMedallion ran at every login and checked only bags and
-- bank, so a medallion left unopened in the mailbox caused another mail at each
-- login. Luf had 167. The core now checks mail_items too.
--
-- Keeps the oldest medallion mail per character and deletes the rest, with their
-- items and texts. Run it only while every affected character is offline: a
-- logged-in character holds its mail in memory and saves it back.
-- Re-runnable: a second run finds one mail per character and deletes nothing.

DROP TEMPORARY TABLE IF EXISTS cp_dupes;
CREATE TEMPORARY TABLE cp_dupes AS
SELECT m.id AS mail_id, m.itemTextId AS text_id, mi.item_guid
FROM mail m
JOIN mail_items mi ON mi.mail_id = m.id AND mi.item_template = 50056
WHERE m.messageType = 3 AND m.sender = 4968
  AND m.id > (SELECT MIN(m2.id)
              FROM mail m2
              JOIN mail_items mi2 ON mi2.mail_id = m2.id AND mi2.item_template = 50056
              WHERE m2.messageType = 3 AND m2.sender = 4968 AND m2.receiver = m.receiver);

DELETE ii FROM item_instance ii JOIN cp_dupes d ON d.item_guid = ii.guid;
DELETE it FROM item_text it JOIN cp_dupes d ON d.text_id = it.id WHERE d.text_id <> 0;
DELETE mi FROM mail_items mi JOIN cp_dupes d ON d.mail_id = mi.mail_id;
DELETE m FROM mail m JOIN cp_dupes d ON d.mail_id = m.id;

DROP TEMPORARY TABLE cp_dupes;
