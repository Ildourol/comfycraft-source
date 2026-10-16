-- ============================================================================
--  Jason Mathers, the Goldshire fishmonger, sells fishing supplies  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\139_goldshire_fishmonger.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console, no restart, in this order:
--
--    reload creature_template
--    reload npc_vendor
--
--  Jason Mathers (383) stands beside Lee Brown (1651), the fishing trainer at
--  Crystal Lake. Upstream gives him only the quest giver flag (2), and he has
--  no quests and no gossip, so he did nothing. Nobody near the trainer sold a
--  pole. He now sells three items from the Stormwind fishing supplier's stock
--  (Catherine Leland, 5494), at their usual prices. npc_flags = 4 opens the
--  shop on a click, with no gossip menu in front of it. Re-runnable.
-- ============================================================================

UPDATE creature_template SET npc_flags = 4 WHERE entry = 383;

DELETE FROM npc_vendor WHERE entry = 383;
INSERT INTO npc_vendor (entry, slot, item) VALUES
    (383, 0, 6256),   -- Fishing Pole
    (383, 1, 6529),   -- Shiny Bauble
    (383, 2, 6530);   -- Nightcrawlers
