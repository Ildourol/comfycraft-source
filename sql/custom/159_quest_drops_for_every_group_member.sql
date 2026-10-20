-- ============================================================================
--  QUEST DROPS FOR EVERY GROUP MEMBER  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\159_quest_drops_for_every_group_member.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  Then, in the mangosd console, in this order:
--    reload item_template
--    reload creature_loot_template
--
--  1. The party loot flag (ITEM_FLAG_PARTY_LOOT, 0x800) on quest-only drops.
--
--  A corpse makes one roll for the whole group. Without the flag the corpse
--  holds one copy of a quest item: the first member to loot it takes it, and
--  it goes away for the others (LootHandler.cpp, "if (!item->freeforall)").
--  With the flag, each member who needs the item gets a copy. 1022 quest-only
--  drops had the flag; the 417 below did not, among them every collection
--  quest in Northshire, Elwynn Forest and Westfall.
--
--  The list is every item that has a negative (quest-only) chance in
--  creature_loot_template or reference_loot_template, does not drop with a
--  positive chance from any loot table, and is white quality or lower.
--  Excluded:
--    - 351 items that also drop normally (Jade, Aquamarine, green weapons).
--      The flag would give each member a copy of the normal drop too.
--    - Essence of the Firelord (19017) and Head of the Broodlord Lashlayer
--      (20383). They drop once for each raid on purpose.
--
--  2. The Intact Animation Rune rolls on its own.
--
--  The Harvest Golem (36), Harvest Watcher (114) and Harvest Reaper (115) had
--  the Cracked Animation Rune (60676, 80%) and the Intact Animation Rune
--  (60677, 40%) in one loot group. A group makes one roll from 0 to 100 and
--  tests its items in order, so the Cracked rune took rolls 0 to 80 and the
--  Intact rune had 20%. The group roll does not check whether the player needs
--  the item, so a player on The Harvest Golem Mystery (40471) lost 80% of
--  kills to a rune they cannot see. With groupid 0 the Intact rune rolls 40%
--  on every kill, as it does on the Rusty Harvest Golem (480).
--
--  To undo part 1:  UPDATE item_template SET flags = flags & ~0x800 WHERE entry IN (<the list>);
--  To undo part 2:  UPDATE creature_loot_template SET groupid = 6 WHERE entry IN (36, 115) AND item = 60677;
--                   UPDATE creature_loot_template SET groupid = 8 WHERE entry = 114 AND item = 60677;

UPDATE item_template SET flags = flags | 0x800 WHERE entry IN (
  725, 750, 752, 772, 773, 780, 782, 829, 884, 915, 1013, 1019, 1075, 1129,
  1130, 1221, 1256, 1261, 1467, 1519, 1524, 1532, 1598, 1894, 2378, 2466,
  2536, 2606, 2611, 2636, 2671, 2676, 2702, 2834, 2843, 2855, 2856, 2858,
  2859, 2872, 2875, 2876, 2909, 2923, 2925, 2939, 3014, 3083, 3084, 3110,
  3156, 3157, 3162, 3163, 3218, 3236, 3253, 3254, 3256, 3257, 3264, 3265,
  3266, 3297, 3337, 3348, 3354, 3397, 3409, 3411, 3412, 3476, 3496, 3505,
  3509, 3510, 3658, 3692, 3714, 3715, 3716, 3720, 3838, 3863, 3901, 3910,
  3911, 3915, 3916, 3917, 3918, 3919, 3923, 4016, 4029, 4053, 4104, 4106,
  4435, 4450, 4495, 4506, 4512, 4513, 4626, 4627, 4628, 4630, 4702, 4739,
  4740, 4742, 4751, 4752, 4753, 4758, 4759, 4769, 4770, 4801, 4802, 4803,
  4848, 4849, 4862, 4868, 4870, 4871, 4886, 4887, 4888, 4892, 4893, 4894,
  4895, 4896, 4897, 4898, 5017, 5018, 5019, 5026, 5030, 5040, 5055, 5062,
  5064, 5065, 5078, 5085, 5086, 5087, 5096, 5098, 5143, 5165, 5166, 5168,
  5170, 5203, 5204, 5336, 5339, 5348, 5360, 5385, 5386, 5412, 5413, 5414,
  5481, 5490, 5582, 5584, 5585, 5620, 5659, 5664, 5675, 5794, 5795, 5796,
  5797, 5801, 5803, 5805, 5808, 5809, 5810, 5840, 5841, 5842, 5848, 5853,
  5854, 5855, 5879, 5952, 5959, 6067, 6074, 6079, 6080, 6166, 6168, 6169,
  6181, 6184, 6246, 6247, 6248, 6249, 6250, 6251, 6252, 6257, 6258, 6259,
  6281, 6443, 6464, 6486, 6634, 6640, 6652, 6799, 6809, 6838, 6839, 6844,
  6845, 6846, 6847, 6930, 6952, 6989, 6991, 6994, 6995, 7119, 7126, 7127,
  7128, 7209, 7231, 7267, 7270, 7273, 7293, 7846, 7847, 7848, 7906, 7968,
  8009, 8052, 8073, 8074, 8587, 8973, 9237, 9238, 9247, 9309, 9320, 9369,
  9437, 9439, 9442, 9460, 9523, 9589, 9591, 9592, 10338, 10509, 10511, 10551,
  10639, 10664, 11114, 11129, 11148, 11266, 11269, 11316, 11318, 11405,
  11467, 11468, 11472, 11477, 11478, 11479, 11480, 11503, 11509, 11725,
  11829, 11830, 11831, 11834, 12220, 12230, 12235, 12237, 12291, 12366,
  12367, 12444, 12622, 12623, 12822, 12829, 13157, 13174, 13350, 13853,
  14544, 14619, 15447, 15849, 15852, 16312, 16743, 16786, 17309, 17684,
  18148, 18590, 18947, 18952, 18953, 18954, 18955, 18956, 18958, 18961,
  20017, 20023, 20027, 20028, 20029, 20087, 20373, 20374, 20375, 20376,
  20377, 20378, 20394, 20395, 20396, 20519, 20610, 20611, 20613, 20614,
  21027, 21137, 21138, 21142, 21145, 21928, 21982, 22094, 22138, 22381,
  22435, 22892, 40063, 41800, 50063, 50064, 51000, 51001, 51325, 51822,
  51860, 51868, 53002, 53004, 53014, 59997, 60176, 60246, 60856, 70021,
  70022, 70035, 70038, 80100, 80103, 80112, 80119, 80170, 80232, 80233,
  80421, 80422, 80862, 80880, 81013, 81181, 81280, 81281, 81282, 81287,
  81292, 81312, 81314, 81323, 81327, 81335, 81346, 81400, 81401
);

UPDATE creature_loot_template SET groupid = 0 WHERE entry IN (36, 114, 115) AND item = 60677;
