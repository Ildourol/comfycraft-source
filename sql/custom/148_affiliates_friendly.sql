-- ============================================================================
--  Town NPCs of both sides are friendly to the other side  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\148_affiliates_friendly.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console: reload creature_template. A creature that is
--  already spawned keeps its old faction until it respawns or the server
--  restarts. In game, `.npc factionid <id>` on a target changes it at once.
--
--  The factionless peace lifts reputation, so it cannot reach a faction that
--  has no reputation bar. These NPCs use such factions:
--
--    84, 210  Alliance Affiliates  (faction 189)  hostile to Horde
--    53, 56   Human, Night Watch   (faction 49)   hostile to Horde
--    83, 106  Horde Affiliates     (faction 66)   hostile to Alliance
--
--  Each moves to a stock template with the same flags whose faction is on
--  Factionless.PeaceFactions. Then reputation decides, and the other side is
--  Friendly. The client reads the template it is sent, so it agrees.
--
--    84        -> 123   Stormwind, flags 33, hostile to Horde
--    53, 56    -> 1575  Stormwind, flags 33, hostile to Horde and monsters
--    210       -> 1575
--    83        -> 125   Orgrimmar, flags 33, hostile to Alliance
--    106       -> 1619  Orgrimmar, flags 33, hostile to Alliance and monsters
--
--  sql/custom/147 did the five Cleft of Shadow NPCs the same way.
--
--  NOT CHANGED, on purpose:
--
--  - Quest targets: the Hillsbrad farms and Dun Garok, Stormpike and
--    Frostwolf Battleguards, Trained Razorbeak, Remnants Sentry, Quel'Lithien
--    Protector, Bloodpetal Pest, Balos Jacken, the Forsaken Courier escort,
--    the Warsong Lumber Camp and the Horde Scouts and Shamans in Ashenvale.
--  - The Alliance and Horde Spirit Guides by the Gurubashi Arena.
--  - Wintersaber Trainers (589) and Earthen Ring (1014). Both have a
--    reputation bar. Adding them to Factionless.PeaceFactions is the lever.
--  - Templates 83 and 84 themselves. Many other NPCs use them.
--
--  Every entry below has its spawns on maps 0 and 1 only. Each UPDATE also
--  matches the old faction, so a row that was changed by hand is left alone.
--  Re-runnable.
-- ============================================================================

-- Alliance -----------------------------------------------------------------

-- Darkshire, Duskwood: the Night Watch.
UPDATE `creature_template` SET `faction` = 1575
 WHERE `entry` IN (264, 499, 828, 840, 887, 888, 2470, 10038) AND `faction` IN (53, 56);

UPDATE `creature_template` SET `faction` = 123
 WHERE `entry` = 5464 AND `faction` = 84;              -- Watchmaster Sorigal

-- Gilneas: Ravenshire, the church, the Vagrant Camp, and the people around them.
UPDATE `creature_template` SET `faction` = 123
 WHERE `entry` IN (61257, 61258, 61259, 61260, 61261, 61262, 61265, 61266,
                   61269, 61270, 61272, 61273, 61274, 61275, 61276, 61277,
                   61278, 61370, 61372, 61373, 61381, 61382, 61383, 61384,
                   61385, 61386, 61387, 61447, 61448, 61449, 61450, 61452,
                   61454, 61455, 61457, 61458, 61459, 61460, 61513, 61534,
                   61535, 61536, 61537, 61538, 61539, 61540, 61541, 61871)
   AND `faction` = 84;

-- One each: Arantir (Redridge), Thorvald Deepforge (Loch Modan),
-- Gelwin Brasspipe (Ironforge), Lord Ivar Pyrewood (Stormwind),
-- Grant Lafford, Corporal Danmere (Theramore).
UPDATE `creature_template` SET `faction` = 123
 WHERE `entry` IN (6030, 7009, 61467, 61557, 61614, 91190) AND `faction` = 84;

UPDATE `creature_template` SET `faction` = 1575
 WHERE `entry` = 9119 AND `faction` = 210;             -- Muigin, Marshal's Refuge

-- Horde --------------------------------------------------------------------

-- Kargath, Badlands.
UPDATE `creature_template` SET `faction` = 125
 WHERE `entry` IN (9077, 9078, 9083, 9084, 9085, 9086) AND `faction` = 83;

-- Hammerfall, Arathi Highlands.
UPDATE `creature_template` SET `faction` = 125
 WHERE `entry` IN (2618, 2619, 2787) AND `faction` = 83;

-- The Wildtusk village.
UPDATE `creature_template` SET `faction` = 125
 WHERE `entry` IN (91400, 91401, 91402, 91403, 91404, 91405, 91407, 91408,
                   91409, 91410, 91411, 91412, 91413, 91414, 91415, 91723)
   AND `faction` = 83;

UPDATE `creature_template` SET `faction` = 1619
 WHERE `entry` = 91290 AND `faction` = 106;            -- Shadow Hunter Yin'do

-- Thunder Bluff.
UPDATE `creature_template` SET `faction` = 125
 WHERE `entry` IN (3441, 4943, 5599, 6393) AND `faction` = 83;

-- Undercity, Tirisfal and the Bulwark.
UPDATE `creature_template` SET `faction` = 125
 WHERE `entry` IN (5675, 6395, 6467, 11022, 11608, 11615) AND `faction` = 83;

-- The Barrens and Camp Taurajo.
UPDATE `creature_template` SET `faction` = 125
 WHERE `entry` IN (3387, 5810, 5878, 5908, 5911, 6394, 10668) AND `faction` = 83;

-- The camp south of Thousand Needles (60749-60857).
UPDATE `creature_template` SET `faction` = 125
 WHERE `entry` IN (60749, 60750, 60751, 60752, 60753, 60754, 60755, 60857)
   AND `faction` = 83;

-- One each and small camps: the Horde Runners, Henchman Valik, Nag'zehn's
-- camp, Prate Cloudseer, Kagoro, Krog, Do'gol, Gurda Wildmane, Xen'Zilla,
-- Rexxar and Misha, Braelyn Firehand, Torek and his raiders, Haggrum
-- Bloodfist.
UPDATE `creature_template` SET `faction` = 125
 WHERE `entry` IN (2333, 2477, 2478, 4198, 4545, 4546, 4547, 4926, 5087,
                   5412, 5523, 5905, 7170, 8586, 10182, 10204, 12816,
                   12858, 12859, 61056)
   AND `faction` = 83;

UPDATE `creature_template` SET `faction` = 1619
 WHERE `entry` = 9118 AND `faction` = 106;             -- Larion, Marshal's Refuge
