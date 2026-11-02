-- ============================================================================
--  Critter crates: animals you can buy, place and pick up          (tw_world)
-- ============================================================================
--  The critter half of sql/custom/042. Until now the only way to put a rabbit
--  in a house was `.house critter add 721`, a raw creature entry out of 129.
--  This makes an animal something a player can hold: an ordinary inventory
--  item that releases its critter when you right-click it inside your own
--  house, and comes back to your bags when you pick it up.
--
--  IT REUSES THE FURNITURE SCRIPT AND THE FURNITURE SPELL ON PURPOSE, and that
--  is the single most important line in this file. script_name is
--  'item_house_furniture' and the on-use spell is 33453, exactly as a chair
--  carries -- so:
--
--    * no new item script, and therefore no new AddSC line, no new file in
--      src/scripts/CMakeLists.txt, no configure re-run;
--    * NO NEW spell_template.script_name ROW, which is the one that bites:
--      spell script names are read ONCE AT STARTUP, so a new binding needs the
--      SQL in place BEFORE the restart meant to pick it up. Reusing 33453
--      means sql/custom/052 and 055 already did that, years of debugging ago.
--
--  The housing side tells the two apart by ITEM ENTRY: HouseFurnitureShouldCast
--  and HouseUseFurnitureItem look in house_furniture_item first and
--  house_critter_item second. One script, two catalogues.
--
--  SO THE ORDERING TRAP DOES NOT APPLY HERE. Housing's usual rule for an item
--  script is "binary leads the SQL", because SQL naming a script the running
--  binary lacks leaves pItemUse absent and the spell casts for real. Nothing
--  new is named here, so this file can be applied in either order -- though
--  058 must still come before the binary.
--
--    Get-Content sql\custom\059_critter_crates.sql | DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  Then, with no restart:
--    .\Reload-Server.ps1 item_template, creature_template, npc_vendor
--    .\Reload-Server.ps1 -Command 'house critter reload'
--
--  Spawn the vendor in-game as GM, standing where you want her:
--    .npc add 100027
--
--  RE-RUNNABLE, AND IT DELETES ITS OWN BLOCK FIRST. New critters go into THIS
--  FILE, not a new numbered one -- the DELETE below would silently wipe a
--  later file's rows the next time anybody re-ran this.
-- ============================================================================

SET @VENDOR := 100027;   -- 100000-100026 are taken; see CLAUDE.md
SET @WITCHCAT := 100046; -- the crated Witch Cat, a copy of creature 89005
SET @SCRIPT := 'item_house_furniture';
SET @DESC   := 'Right-click to let it out in your house.';

--  THE ICON IS INV_Box_PetCarrier_01, an actual pet carrier -- ItemDisplayInfo
--  20629, which 17 shipped items use (Black Tabby among them). 22271 and three
--  others carry the same picture; 20629 is simply the commonest.
--
--  This was 7913 (INV_Crate_01's neighbour) for one afternoon, picked by asking
--  which display ids Turtle's own companion pets use most and taking the top
--  answer. That method was sound and the answer was incomplete: 20629 was
--  SECOND on the same list and is the icon literally named for the job, which a
--  frequency count could not tell me. Searching ItemDisplayInfo.dbc field 5 for
--  the icon NAME is the check that was missing -- 4,448 distinct names are in
--  there, and it is the only place the artists' intent is written down.
--
--  Confirmed present in the client's archives before being used here; an icon
--  the client cannot resolve draws as a green square with no error.
SET @ICON   := 20629;

--  33453 "Over-Tinkered Lens": Targets 0x0 (no reticle -- the Decorator's
--  Chalk aims every route into housing), CastingTimeIndex 4 = 1000ms, and
--  SpellVisual 215, the crafting animation. sql/custom/052 binds
--  spell_house_furniture_place to THIS entry, which is what makes the number
--  load-bearing rather than cosmetic. Changing it here without changing 052
--  gives a crate that casts and places nothing, with nothing in the log.
--
--  A CRAFTING ANIMATION FOR LETTING A RABBIT OUT is a slight stretch and it is
--  kept deliberately: it is one second of "something is happening" that the
--  player already recognises from furniture, and inventing a second gesture
--  for the same act of putting something down would be two vocabularies for
--  one idea. If it ever grates, the fix is one @USE and one row in 052.
SET @USE    := 33453;

-- ---------------------------------------------------------------------------
--  The mapping table
-- ---------------------------------------------------------------------------
--  wander is the roaming radius in yards the animal is released with, the same
--  units `.house critter wander` takes. 0 means it stays where you put it.
--  Kept small on every row: map 28's navmesh describes the flat plain and
--  knows nothing about a spawned building, so a generous radius is how an
--  animal ends up somewhere surprising.
CREATE TABLE IF NOT EXISTS `house_critter_item` (
  `item_entry`    INT UNSIGNED NOT NULL           COMMENT 'item_template.entry',
  `critter_entry` INT UNSIGNED NOT NULL           COMMENT 'creature_template.entry',
  `wander`        FLOAT        NOT NULL DEFAULT 3 COMMENT 'yards it roams; 0 = stays put',
  PRIMARY KEY (`item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

DELETE FROM house_critter_item WHERE item_entry BETWEEN 100300 AND 100399;
DELETE FROM item_template      WHERE entry      BETWEEN 100300 AND 100399;
DELETE FROM npc_vendor         WHERE entry = @VENDOR;
DELETE FROM creature_template  WHERE entry = @VENDOR;
DELETE FROM creature_template  WHERE entry = @WITCHCAT;

-- ---------------------------------------------------------------------------
--  The Witch Cat
-- ---------------------------------------------------------------------------
--  A copy of Turtle's Midnight (89005: display 18628, the Hallow's End cat with
--  a witch hat, scale 0.8) under a name of its own. A creature's name comes
--  from creature_template, so the crate's cat would otherwise come out as
--  "Midnight". The copy leaves 89005 and its companion pet item (80010) alone.
--  Players can still rename their cat with `.house critter name`.
CREATE TEMPORARY TABLE witch_cat AS SELECT * FROM creature_template WHERE entry = 89005;
UPDATE witch_cat SET entry = @WITCHCAT, name = 'Witch Cat', subname = '';
INSERT INTO creature_template SELECT * FROM witch_cat;
DROP TEMPORARY TABLE witch_cat;

-- ---------------------------------------------------------------------------
--  The stock
-- ---------------------------------------------------------------------------
--  100300-100399 is the CRITTER block. 100100-100199 is furniture and
--  100200-100299 is housing tools (the Decorator's Chalk is 100200); stock
--  item entries resume at 279367, so there is room either side.
--
--  Every one of these is drawn from the 56 critter templates with no ai_name
--  and no script_name -- nothing here has behaviour of its own to fight. They
--  are all type 8 (CREATURE_TYPE_CRITTER), which is also what the core holds a
--  player to, so nothing in this file needs a developer to place it.
--
--  Priced by nothing more principled than how appealing they are. The
--  whelpling is dear because it is a dragon.
--
--  bonding = 0, tradeable. stackable = 1, deliberately: a stack would release
--  one and leave the rest, and the pick-up path would have to decide whether
--  to merge.
INSERT INTO item_template
  (entry, class, subclass, name, description, display_id, quality, flags,
   buy_price, sell_price, inventory_type, allowable_class, allowable_race,
   item_level, required_level, stackable, max_count, bonding, material, sheath,
   spellid_1, spelltrigger_1, spellcharges_1, script_name)
VALUES
  (100300,0,0,'Crated Rabbit',       @DESC,@ICON,1,0, 10000, 2000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100301,0,0,'Crated Cat',          @DESC,@ICON,1,0, 15000, 3000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100302,0,0,'Crated Baby Turtle',  @DESC,@ICON,1,0, 20000, 4000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100303,0,0,'Crated Squirrel',     @DESC,@ICON,1,0, 10000, 2000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100304,0,0,'Crated Prairie Dog',  @DESC,@ICON,1,0, 10000, 2000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100305,0,0,'Crated Toad',         @DESC,@ICON,1,0,  7500, 1500,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100306,0,0,'Crated Raven',        @DESC,@ICON,1,0, 20000, 4000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100307,0,0,'Crated Penguin',      @DESC,@ICON,1,0, 30000, 6000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100308,0,0,'Crated Hare',         @DESC,@ICON,1,0, 10000, 2000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100309,0,0,'Crated Fawn',         @DESC,@ICON,1,0, 25000, 5000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100310,0,0,'Crated Sheep',        @DESC,@ICON,1,0, 15000, 3000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100311,0,0,'Crated Whelpling',    @DESC,@ICON,1,0,150000,30000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100312,0,0,'Crated Witch Cat',    @DESC,@ICON,1,0, 20000, 4000,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT);

--  A LARGER ANIMAL GETS A SMALLER RADIUS, not a larger one. A sheep covers
--  three yards in about the time a toad covers one, so matching radius to size
--  would make the big ones the escape risks.
INSERT INTO house_critter_item (item_entry, critter_entry, wander) VALUES
  (100300,   721, 3),  -- Rabbit
  (100301,  6368, 3),  -- Cat
  (100302, 50141, 2),  -- Baby Turtle       Turtle WoW's own
  (100303,  1412, 3),  -- Squirrel          ships at scale 1.3
  (100304,  2620, 3),  -- Prairie Dog
  (100305, 60866, 2),  -- Toad
  (100306, 61643, 3),  -- Raven             ships at scale 1
  (100307, 70040, 2),  -- Glacier Penguin   ships at scale 0.7
  (100308,  5951, 3),  -- Hare
  (100309,   890, 2),  -- Fawn
  (100310,  1933, 2),  -- Sheep
  (100311, 50635, 2),  -- Onyxian Whelpling ships at scale 0.4
  (100312, @WITCHCAT, 3);  -- Witch Cat     a copy of Turtle's Midnight, below. Hallow's End,
                           --               not in Wren's stock. Reforged clients need patch-R
                           --               (patches/lib/cat-hallowsend.js)

-- ---------------------------------------------------------------------------
--  The vendor
-- ---------------------------------------------------------------------------
--  npc_flags = 4 is UNIT_NPC_FLAG_VENDOR in the 1.12 enum (not 128).
--  faction 35 = friendly to everyone. display 2661 is a human female commoner.
--
--  HER OWN NPC RATHER THAN MORE STOCK ON CARPENTER BRAM. He is 'Furnishings'
--  and already carries fifteen crates plus the chalk; a carpenter selling
--  rabbits is a joke that stops being funny the second time you look for one.
--  There is also a hard reason: MAX_VENDOR_ITEMS is 128 and shared catalogues
--  are how you find that out the expensive way.
INSERT INTO creature_template
  (entry, name, subname, display_id1, faction, npc_flags,
   level_min, level_max, health_min, health_max, armor, unit_class, type,
   speed_walk, speed_run, scale, base_attack_time, ranged_attack_time,
   dmg_min, dmg_max, movement_type, inhabit_type, regeneration, ai_name)
VALUES
  (@VENDOR, 'Critterkeeper Wren', 'Housepets', 2661, 35, 4,
   60, 60, 4007, 4007, 3272, 1, 7,
   1, 1.14286, 1, 2000, 2000,
   103.4, 129.8, 0, 3, 3, '');

--  maxcount 0 -> unlimited stock, so incrtime 0. Prices are buy_price above.
INSERT INTO npc_vendor (entry, slot, item, maxcount, incrtime) VALUES
  (@VENDOR,  1, 100300, 0, 0),  -- Rabbit          1g
  (@VENDOR,  2, 100301, 0, 0),  -- Cat             1g 50s
  (@VENDOR,  3, 100302, 0, 0),  -- Baby Turtle     2g
  (@VENDOR,  4, 100303, 0, 0),  -- Squirrel        1g
  (@VENDOR,  5, 100304, 0, 0),  -- Prairie Dog     1g
  (@VENDOR,  6, 100305, 0, 0),  -- Toad           75s
  (@VENDOR,  7, 100306, 0, 0),  -- Raven           2g
  (@VENDOR,  8, 100307, 0, 0),  -- Penguin         3g
  (@VENDOR,  9, 100308, 0, 0),  -- Hare            1g
  (@VENDOR, 10, 100309, 0, 0),  -- Fawn         2g 50s
  (@VENDOR, 11, 100310, 0, 0),  -- Sheep        1g 50s
  (@VENDOR, 12, 100311, 0, 0);  -- Whelpling      15g
