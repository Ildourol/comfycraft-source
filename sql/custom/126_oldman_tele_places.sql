-- ============================================================================
--  The Old Man's .tele places                                        (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\126_oldman_tele_places.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then `reload game_tele` in the mangosd console, unless the server is about
--  to start anyway.
--
--  Four places made locally with `.tele add` on 2026-09-27, for The Old Man's
--  "Away!" page, which teleports to one of them. A button names a place by
--  game_tele id, and content transfer carries places by NAME, so these must
--  exist on a box before a dialog naming them is imported there.
--
--  BY NAME, NOT BY ID. The box gives each its own id, which may differ from
--  the local 956-959. A re-run skips a name that is already there, so the id a
--  button holds never changes under it. Delete-and-insert would renumber the
--  rows and leave those buttons pointing at nothing.
-- ============================================================================

INSERT INTO game_tele (position_x, position_y, position_z, orientation, map, name)
SELECT v.x, v.y, v.z, v.o, v.map, v.name FROM (
  SELECT -9560.57 AS x, -3414.89 AS y, 171.376 AS z, 5.94816 AS o, 0 AS map, 'oldman_redridge'      AS name UNION ALL
  SELECT -7884.5,       -2682.29,      251.314,      0.638117,     0,        'oldman_burning_steps'         UNION ALL
  SELECT -4977.59,      -1931.95,      23.534,       1.63738,      1,        'oldman_thousand'              UNION ALL
  SELECT  3239.46,      -5384.18,      116.473,      6.16502,      1,        'oldman_azshara'
) v
WHERE NOT EXISTS (SELECT 1 FROM game_tele t WHERE t.name = v.name);
