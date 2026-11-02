-- ============================================================================
--  Reedwoven Epaulets: 1% hit instead of ranged attack power  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\226_reedwoven_epaulets_hit.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console:  reload item_template
--  then restart the client.
--
--  Item 33040 is mail shoulders at required level 24. Its equip spell was
--  21432 (+17 ranged attack power). A hunter cannot wear mail before level 40,
--  so that bonus did nothing. Spell 15464 (Increased Hit Chance 1, aura 54
--  mod hit chance, value 1) is the stock "Improves your chance to hit by 1%."
--  that Stormshroud Shoulders and Darksoul Shoulders use. The spell is in
--  spellid_1 with trigger 1 (on equip), as 21432 was. Stats, armour, level
--  and price do not change. Re-runnable.
--
--  Undo: set spellid_1 back to 21432, then reload item_template.
--
--  The bot item info cache sees the change at the next start (its fingerprint
--  checksums item_template). Do not empty the bot equip cache for this.
-- ============================================================================

UPDATE item_template
   SET spellid_1 = 15464,
       spelltrigger_1 = 1
 WHERE entry = 33040;
