-- ============================================================================
--  FACTIONLESS: QUEST MOBS THAT STAY AT WAR  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\205_factionless_war_creatures.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then a restart. mangosd reads the table once at startup.
--
--  Factionless.PeaceFactions lifts every race to Friendly with the other
--  side's factions. That also made the other side's quest mobs friendly: the
--  Hillsbrad militia, Kul Tiras at Tiragarde, Northwatch, Kalman Azureborn.
--  Until 2026-10-24 players went round it with the At War box in the
--  reputation pane. The bug 79 fix (PEACE_FORCED) closed that.
--
--  A creature in this table is hostile to a player who has a quest against
--  it in the log, and friendly to everyone else. A quest is against it when
--  the quest needs it killed, or needs its quest-only drop as an objective
--  item or a source item (The Sacred Flame, 1195, and the Laughing Sisters'
--  Etched Phial, bug 143), and the quest's stock races are born at war with
--  its faction. The viewer at war is sent
--  faction template 14 (Monster), so the client draws it red and lets the
--  player attack it (FactionlessWar.h). Every race can take every quest.
--
--  The seed is every spawned creature of a peace faction that a quest needs
--  killed (no ReqSpellCast) or that drops a quest-only item, where the
--  quest's own races are born at war with that faction. 112 entries,
--  2026-10-25. comment is the creature name and the quests.
--
--  mangosd probes information_schema before it reads, so a binary that meets
--  a database without this table keeps every creature at peace.
--
--  Re-runnable: creates if missing and replaces the seed rows.
-- ============================================================================

CREATE TABLE IF NOT EXISTS factionless_war_creature (
    entry   INT UNSIGNED NOT NULL,
    comment VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (entry)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;

REPLACE INTO factionless_war_creature (entry, comment) VALUES
(861, 'Stonard Scout: 40079'),
(1867, 'Dalaran Apprentice: 1961'),
(1888, 'Dalaran Watcher: 1961'),
(1889, 'Dalaran Wizard: 1961'),
(1891, 'Pyrewood Watcher: 99'),
(1894, 'Pyrewood Sentry: 99'),
(1895, 'Pyrewood Elder: 99'),
(1912, 'Dalaran Protector: 479, 1961'),
(1913, 'Dalaran Warder: 1961'),
(1914, 'Dalaran Mage: 479, 1961'),
(1915, 'Dalaran Conjuror: 479, 1961'),
(2120, 'Archmage Ataeric: 480'),
(2265, 'Hillsbrad Apprentice Blacksmith: 529, 546'),
(2266, 'Hillsbrad Farmer: 527, 546'),
(2267, 'Hillsbrad Peasant: 528, 546'),
(2268, 'Hillsbrad Footman: 546'),
(2269, 'Hillsbrad Miner: 539, 546, 1838'),
(2270, 'Hillsbrad Sentry: 546'),
(2271, 'Dalaran Shield Guard: 556'),
(2272, 'Dalaran Theurgist: 556'),
(2332, 'Valdred Moray: 530'),
(2358, 'Dalaran Summoner: 545, 556'),
(2359, 'Elemental Slave: 545, 557'),
(2360, 'Hillsbrad Farmhand: 527, 546'),
(2387, 'Hillsbrad Councilman: 532, 546'),
(2415, 'Warden Belamoore: 556'),
(2503, 'Hillsbrad Foreman: 546'),
(2583, 'Stromgarde Troll Hunter: 640'),
(2584, 'Stromgarde Defender: 640'),
(2585, 'Stromgarde Vindicator: 640'),
(2607, 'Prince Galen Trollbane: 644'),
(2612, 'Lieutenant Valorcall: 643'),
(2628, 'Dalaran Worker: 556'),
(2659, 'Razorbeak Skylord: 7830'),
(2691, 'Highvale Outrunner: 7841'),
(2692, 'Highvale Scout: 7841'),
(2693, 'Highvale Marksman: 7841'),
(2694, 'Highvale Ranger: 7841'),
(3128, 'Kul Tiras Sailor: 784, 791'),
(3129, 'Kul Tiras Marine: 784, 791'),
(3192, 'Lieutenant Benedict: 784, 791'),
(3374, 'Bael''dun Excavator: 843'),
(3375, 'Bael''dun Foreman: 843'),
(3376, 'Bael''dun Soldier: 846'),
(3377, 'Bael''dun Rifleman: 846'),
(3378, 'Bael''dun Officer: 846'),
(3385, 'Theramore Marine: 891'),
(3386, 'Theramore Preserver: 891'),
(3392, 'Prospector Khazgorm: 843'),
(3393, 'Captain Fairmount: 891'),
(3454, 'Cannoneer Smythe: 891'),
(3455, 'Cannoneer Whessan: 891'),
(3528, 'Pyrewood Armorer: 99'),
(3530, 'Pyrewood Tailor: 99'),
(3532, 'Pyrewood Leatherworker: 99'),
(3577, 'Dalaran Brewmaster: 1961'),
(3578, 'Dalaran Miner: 1961'),
(3986, 'Sarilus Foulborne: 1017'),
(4051, 'Cenarion Botanist: 1087'),
(4053, 'Daughter of Cenarius: 1087'),
(4054, 'Laughing Sister: 1945'),
(4057, 'Son of Cenarius: 1087'),
(4066, 'Nal''taszar: 1058'),
(4273, 'Keeper Ordanus: 1088'),
(4479, 'Fardel Dabyrie: 1164'),
(4480, 'Kenata Dabyrie: 1164'),
(4481, 'Marcel Dabyrie: 1164'),
(4834, 'Theramore Infiltrator: 1201'),
(5278, 'Sprite Darter: 2973, 3128'),
(5682, 'Dalin Forgewright: 1476'),
(5683, 'Comar Villard: 1476'),
(5809, 'Watch Commander Zalaphil: 791'),
(6668, 'Lord Cyrik Blackforge: 846'),
(7997, 'Captured Sprite Darter: 2973, 3128'),
(8518, 'Rynthariel the Keymaster: 3514'),
(8563, 'Woodsman: 6133, 80303'),
(8564, 'Ranger: 6133, 80303'),
(8565, 'Pathstrider: 6133, 80303'),
(11022, 'Alexi Barov: 5344'),
(11023, 'Weldon Barov: 5342'),
(11878, 'Nathanos Blightcaller: 6187'),
(12856, 'Ashenvale Outrunner: 6503'),
(12864, 'Warsong Outrider: 40802'),
(60845, 'Kalman Azureborn: 40513'),
(60876, 'Broken Arcane Golem: 99'),
(60893, 'Expedition Commando: 40665'),
(60894, 'Expedition Cleric: 40672'),
(60895, 'Expedition Sailor: 40672'),
(60896, 'Expedition Officer: 40673'),
(60897, 'Expedition Soldier: 40672'),
(60898, 'Expedition Scout: 40665'),
(60899, 'Hydromancer Shaleborn: 40673'),
(60900, 'Deckmaster Darkhollow: 40673'),
(60901, 'Lieutenant Alverold: 40674'),
(60902, 'Sir Danuvis: 40674'),
(61785, 'Ithirilen Shadewind: 41262'),
(62014, 'Private William: 41365'),
(62525, 'SI:7 Scout: 41828'),
(70023, 'Commander Grushak: 40803, 40809'),
(70025, 'Ashenvale Stalker: 70025'),
(70027, 'Farseer Grimeye: 40803, 40809'),
(91293, 'Archivist Landas: 55041'),
(91350, 'Magus Bromley: 556'),
(91352, 'Remnants Footman: 40277'),
(91714, 'Foreman Bill: 546'),
(91719, 'Magister Salorn: 40084'),
(91721, 'Duchess Faliona: 40277'),
(91773, 'Remnants Scout: 40096'),
(91974, 'Captain Silas: 40277'),
(91975, 'Remnants Priest: 40277'),
(91980, 'Remnants Farmer: 40099'),
(91981, 'Remnants Militia: 40099');
