-- Deeprun Tram: every player sees all six cars.
--
-- The client moves each car from TransportAnimation.dbc. The server keeps the car at its
-- spawn point and sends it only to players within 250 yards of that point. Each track's
-- three cars spawn at one station, so a player at a station never got the cars of the
-- other track.
--
-- spawn_flags 1 (SPAWN_FLAG_ACTIVE) keeps the car's grid loaded and puts the car in the
-- map's active list, which is checked against every player on the map. visibility_mod
-- counts only for an active object. 3000 yards covers the 2482-yard track.
--
-- Needs a restart: spawn data and active objects load when the map is made.

UPDATE gameobject
SET spawn_flags = spawn_flags | 1,
    visibility_mod = 3000
WHERE map = 369 AND guid BETWEEN 18802 AND 18807 AND id BETWEEN 176080 AND 176085;
