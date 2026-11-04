-- Land mines never dodge, parry or block (bug 130).
--
-- Dark Iron Land Mine (8035, summoned by the Dark Iron Agent in Gnomeregan) and Goblin
-- Land Mine (7527, the engineering mine, npc_goblin_land_mine) had flags_extra 0. Every
-- creature then has a 5% dodge, parry and block chance.
--
-- 276 = CREATURE_FLAG_EXTRA_NO_PARRY (4) + NO_BLOCK (16) + NO_DODGE (256).
-- NO_DODGE is our flag, added to the core with this script (Creature.h).
--
-- Safe to run twice. Takes effect after `reload creature_template`: the core reads
-- flags_extra from the template on each attack.

UPDATE creature_template
SET flags_extra = flags_extra | 276
WHERE entry IN (8035, 7527);
