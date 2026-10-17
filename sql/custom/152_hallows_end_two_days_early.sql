-- ============================================================================
--  HALLOW'S END: TWO DAYS EARLIER, SAME END  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\152_hallows_end_two_days_early.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  Hallow's End (game_event 12) starts on 17 October 03:00 UTC, two days
--  before the stock 19 October 03:00 UTC. The length goes from 14 days to
--  16, so the event still ends on 2 November 03:00 UTC.
--
--  start_time is read in the server's time zone. FROM_UNIXTIME gives the same
--  instant on a server in any time zone.
--
--  The server reads game_event once at startup: apply, then restart. The
--  event starts at startup when the time is inside its window. Safe to run
--  again.
-- ============================================================================

UPDATE `game_event`
SET `start_time` = FROM_UNIXTIME(1602903600),   -- 2020-10-17 03:00:00 UTC
    `length`     = 23040                        -- 16 days, in minutes
WHERE `entry` = 12;
