-- ============================================================================
--  SUMMONING STONES HAVE NO LEVEL REQUIREMENT (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\229_meeting_stones_any_level.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  Then `reload gameobject_template` in the mangosd console, and restart the
--  client. The client caches gameobject query data; the launcher wipes the
--  WDB cache on start.
--
--  A meeting stone (gameobject type 23) has a level range: data0 is minLevel,
--  data1 is maxLevel, data2 is the AreaTable id. The client gets the range in
--  the gameobject query response and checks it itself. Outside the range it
--  says "You do not meet the requirements" and never sends
--  CMSG_MEETINGSTONE_JOIN. The server does not read data0 or data1.
--
--  This sets every stone to level 1 up to 255, so any character can click
--  any stone. data2 stays: the group finder fallback maps the stone to its
--  dungeons by it. The group finder still drops the wings the party is too
--  low for.
--
--  Re-runnable: the UPDATE sets fixed values.
-- ============================================================================

UPDATE gameobject_template
SET data0 = 1,
    data1 = 255
WHERE type = 23;
