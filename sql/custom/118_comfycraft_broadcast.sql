-- 118_comfycraft_broadcast.sql
--
-- Replaces upstream's autobroadcast ("This is an unofficial continuation of
-- Turtle-WoW 1.18.1", mangos_string 50009) with four ComfyCraft messages.
--
-- AutoBroadCastMgr sends one random row of `autobroadcast` to every player
-- each AutoBroadcast.Timer (2400000 ms, 40 minutes, in mangosd.conf). Each
-- message is one short line, so a broadcast never fills the chat frame.
--
-- mangos_string 100000-100009 are ours, for broadcasts. The upstream string
-- 50009 stays in mangos_string; only its autobroadcast row goes.
--
-- Apply, then in the mangosd console, in this order:
--   reload mangos_string
--   reload autobroadcast
--
-- To restore upstream's message:
--   DELETE FROM `autobroadcast`;
--   INSERT INTO `autobroadcast` (`string_id`) VALUES (50009);

DELETE FROM `mangos_string` WHERE `entry` BETWEEN 100000 AND 100009;
INSERT INTO `mangos_string` (`entry`, `content_default`) VALUES
(100000, 'ComfyCraft is an experimental project: a welcoming place to unwind after work, fish, roleplay and run dungeons with friends.'),
(100001, 'Some experiments here are made to see what happens. What if Dwarves could be Druids? What if Orcs could relax in Stormwind?'),
(100002, 'ComfyCraft is meant to be comfy, not easy. XP rates are normal, monsters are still strong, and classes are tuned down at the top end.'),
(100003, 'We are happy to have you. Thanks for being here.');

DELETE FROM `autobroadcast`;
INSERT INTO `autobroadcast` (`string_id`) VALUES (100000), (100001), (100002), (100003);
