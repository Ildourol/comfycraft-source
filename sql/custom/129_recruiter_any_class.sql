-- Helper Elowen offers paladin and shaman companions to both sides (tw_world).
--
-- Every race is every class (115), and RecruitBot.cpp no longer refuses a
-- Horde paladin or an Alliance shaman. 068 hid those buttons from the other
-- side with the stock team conditions 3 (Alliance) and 2 (Horde). Clear them.
--
-- Re-runnable. Then, in the console: reload gossip_menu_option

UPDATE gossip_menu_option SET condition_id = 0
WHERE (menu_id = 64023 AND id = 3)
   OR (menu_id = 64024 AND id IN (3, 5))
   OR (menu_id = 64025 AND id IN (9, 10));
