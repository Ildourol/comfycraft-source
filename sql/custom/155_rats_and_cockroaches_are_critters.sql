-- ============================================================================
--  RATS AND COCKROACHES ARE CRITTERS  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\155_rats_and_cockroaches_are_critters.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  Then, in the mangosd console:
--    reload creature_template
--
--  Rat (4075, 951 spawns) is type 8 in source/sql/base. Upstream's
--  20260603094447_world.sql (its creature_template_balor.sql section) set it
--  to type 0, and with 20260529182210_world.sql gave it unit_flags 524288
--  (UNIT_FLAG_IN_COMBAT) and dynamic_flags 12. The Balor work edited the
--  stock template. This puts the three values back to the base.
--
--  Cockroach (7395, 7 spawns: 6 in Undercity, 1 in Tanaris) is type 1 in
--  the base too. It becomes type 8. Its unit_flags 768 (immune to players and
--  to NPCs) stay.
--
--  Player::PetCritter and the /charts critter board take only type 8, so
--  before this a rat or a cockroach gave no hearts and no point.
--
--  Not changed: Dig Rat (3444) is a quest kill. Whiskers the Rat (16549) and
--  Polymorphed Cockroach (16374) are type 1 in the base.

UPDATE creature_template SET type = 8, unit_flags = 0, dynamic_flags = 0 WHERE entry = 4075;
UPDATE creature_template SET type = 8 WHERE entry = 7395;
