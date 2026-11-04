-- ============================================================================
--  Rares of a peace faction are hostile to everyone  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\233_peace_faction_rares.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then a restart. Spawns already in the world keep their old faction until
--  they respawn.
--
--  Bug 135. Digger Flameforge (5849) and Heggin Stonewhisker (5847) in Bael
--  Modan had template 122 (Ironforge). Factionless.PeaceFactions makes every
--  race Friendly with Ironforge, so they were green to everyone and nobody
--  could attack them. The Bael'dun Excavators and Foremen around them are red
--  through the war list (factionless_war_creature, quests 843 and 846). The
--  war list needs a quest against the creature in the log, and no quest
--  targets these two.
--
--  The rule: a rare that no quest targets gets a stock hostile template with
--  no reputation bar. Do not add a war list row for it.
--
--  The sweep, from the stock dumps in source/sql/base: every spawned creature
--  of rank 2 (rare elite) or 4 (rare) whose template's faction is in
--  Factionless.PeaceFactions and starts At War for at least one playable race.
--  No quest has a kill objective on any entry fixed here, and none of them
--  has a quest-only drop.
--
--  Fixed. Each gets template 14 (Monster: faction 14, no reputation bar,
--  hostile to every player, ourMask Monster), except Brother Ravenoak:
--     5849 Digger Flameforge          Barrens, Bael Modan      122 -> 14
--     5847 Heggin Stonewhisker        Barrens, Bael Modan      122 -> 14
--     5851 Captain Gerogg Hammertoe   Barrens, Bael'dun Keep   122 -> 14
--     5797 Aean Swiftriver            Barrens, Outrunners      124 -> 14
--     5798 Thora Feathermoon          Barrens, Outrunners      124 -> 14
--     5799 Hannah Bladeleaf           Barrens, Outrunners      124 -> 14
--     5800 Marcus Bel                 Barrens, Outrunners      124 -> 14
--     1920 Dalaran Spellscribe        Silverpine, Ambermill     76 -> 14
--     3735 Apothecary Falthis         Ashenvale, Bathran's     118 -> 14
--     5916 Sentinel Amarassan         Stonetalon               124 -> 14
--     8214 Jalinde Summerdrake        Hinterlands             1576 -> 14
--    10817 Duggan Wildhammer          Eastern Plaguelands     1055 -> 14
--    10824 Ranger Lord Hawkspear      Eastern Plaguelands     1576 -> 14
--     5915 Brother Ravenoak           Stonetalon               124 -> 168
--  Brother Ravenoak stands among the Mirkfallon Dryads and Keepers (template
--  79). Template 79 is hostile to the Monster mask, so on template 14 they
--  would fight him. Template 168 (faction 128 Enemy, no reputation bar) has
--  no mask of its own and is hostile to players only. No other fixed rare has
--  a creature hostile to the Monster mask within 250 yards of its spawn.
--
--  Left as they are:
--     5848 Malgin Barleybrew (Bael Modan): a vendor (npc_vendor, 5 items).
--    15197 Darkcaller Yanka: gives and ends quests.
--    61587 Bethelon Wildwhisper: gives quests.
--    60569 Dave "Ozy" Norton: a Stormwind city NPC beside a city guard.
--    61772 Tenris Mirkblood: a gossip NPC in Alah'Thalas among the refugees.
--     4066 Nal'taszar, 5809 Watch Commander Zalaphil: on the war list.
--
--  Re-runnable. Undo: set faction 122 on 5847, 5849 and 5851; 124 on 5797,
--  5798, 5799, 5800, 5915 and 5916; 76 on 1920; 118 on 3735; 1576 on 8214
--  and 10824; 1055 on 10817.
-- ============================================================================

UPDATE creature_template SET faction = 14  WHERE entry = 5849;
UPDATE creature_template SET faction = 14  WHERE entry = 5847;
UPDATE creature_template SET faction = 14  WHERE entry = 5851;
UPDATE creature_template SET faction = 14  WHERE entry = 5797;
UPDATE creature_template SET faction = 14  WHERE entry = 5798;
UPDATE creature_template SET faction = 14  WHERE entry = 5799;
UPDATE creature_template SET faction = 14  WHERE entry = 5800;
UPDATE creature_template SET faction = 14  WHERE entry = 1920;
UPDATE creature_template SET faction = 14  WHERE entry = 3735;
UPDATE creature_template SET faction = 14  WHERE entry = 5916;
UPDATE creature_template SET faction = 14  WHERE entry = 8214;
UPDATE creature_template SET faction = 14  WHERE entry = 10817;
UPDATE creature_template SET faction = 14  WHERE entry = 10824;
UPDATE creature_template SET faction = 168 WHERE entry = 5915;
