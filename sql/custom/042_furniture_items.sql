-- ============================================================================
--  Furniture items, and the two people who sell them  (tw_world)
-- ============================================================================
--  Housing milestone M5. Until now the only way to put a chair in a house was
--  .house object add 2413 - a developer command taking a raw gameobject entry
--  out of 15,550. This turns furniture into something a player can hold: an
--  ordinary inventory item that places its model when you right-click it
--  inside your own house, and comes back to your bags when you pick it up.
--
--  THREE PIECES, and they are separate on purpose:
--
--    house_furniture_item   item entry -> gameobject entry. The whole mapping.
--                           Adding furniture later is rows here plus rows in
--                           item_template; no code changes, and
--                           `.house furniture reload` picks it up live.
--    item_template          the items themselves, in 100100-100199 and then
--                           100400-100999. See the DELETEs for why it is two
--                           ranges and not one.
--    creature 100026        Carpenter Bram, a plain vendor window with the
--                           original fifteen in it.
--    creature 100028        Furnisher Adela, the same crates plus everything
--                           since, behind a menu with one page per category.
--                           She supersedes Bram; he is kept because deleting
--                           a spawned NPC is the owner's call, not this file's.
--
--  THE ITEMS ARE ALL ONE ICON - INV_Crate_01, a plain wooden crate (display
--  7914). Vanilla has no chair or table icon, so per-item icons would be a
--  column of near-misses; a flat-pack crate reads as furniture and is honest
--  about it. The ComfyHousing addon shows the actual model on hover, which is
--  what the picture is FOR.
--
--  bonding = 0 - tradeable. Some furniture is meant to be soulbound later;
--  that is bonding = 1 on those rows and nothing else.
--
--  stackable = 1 - deliberately not stackable. A stack of chairs would place
--  one and leave the rest, and the pick-up path would have to decide whether
--  to merge.
--
--  THE DATABASE OWNS FURNITURE NOW, AND THIS FILE IS A SEED (2026-10-08).
--  New crates are made, renamed and repriced in the website's Workshop, at
--  /admin/furniture, on whichever server the site runs, production included.
--  That page writes item_template, house_furniture_item and Adela's npc_vendor
--  directly. tools/furniture-list.js and tools/gen-furniture-sql.js are
--  retired; git has them.
--
--  So this file DELETES NO FURNITURE and every furniture INSERT is an INSERT
--  IGNORE. The rows below are the 65 crates as they stood on 2026-10-08. A
--  re-run adds only what is missing and never undoes an edit made on the page.
--  The deploy re-applies a file whose hash changed, which is why that matters.
--  Do not add crates here: add them on the page.
--
--  Re-runnable. Apply with:
--    Get-Content sql\custom\042_furniture_items.sql | DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  APPLY 041 (tw_char) FIRST, and before the new binary - it adds the column
--  the loader now selects, and a missing column is a hard crash. SAME FOR 062,
--  which adds house_furniture_item.category to an install that already has the
--  table; CREATE TABLE IF NOT EXISTS below will not add a column to one.
--
--  Spawn a vendor in-game as GM, standing where you want them:
--    .npc add 100026        Carpenter Bram, flat window, the original fifteen
--    .npc add 100028        Furnisher Adela, tabbed, all sixty-four
-- ============================================================================

SET @VENDOR := 100026;   -- 100000-100025 are taken; see CLAUDE.md
SET @SHOP   := 100028;   -- Furnisher Adela, the tabbed shop
SET @ICON   := 7914;     -- ItemDisplayInfo 7914 -> INV_Crate_01, a wooden crate
SET @SCRIPT := 'item_house_furniture';
SET @DESC   := 'Right-click to place this in your house.';

-- The item's on-use spell exists ONLY so the client offers "Use" and sends
-- CMSG_USE_ITEM. It is never cast: the pItemUse hook returns true, which makes
-- WorldSession::HandleUseItemOpcode skip CastItemUseSpell entirely - so a
-- refusal outside your own house costs the player nothing.
--
-- IT ALSO DECIDES HOW YOU AIM. The client raises the ground reticle - the one
-- dynamite uses - when and only when the spell's Targets mask carries
-- TARGET_FLAG_DEST_LOCATION (0x40), and then puts the clicked point in the
-- packet for SpellCastTargets::read to pick up. Nothing else is consulted:
-- Target Dummy (4071) and the Field Repair Bot (22700) carry implicit target 0
-- and still reticle, while dynamite carries 16.
--
-- PROFILE THE CANDIDATE AGAINST A SPELL THAT PROVABLY WORKS, do not reason
-- about which fields the client checks. Rough Dynamite (4054) works from a bag
-- for any class in any stance, and reads:
--
--   Stances 0, StancesNot 0, powerType 0, manaCost 0, SpellFamilyName 0
--
-- Anything not matching that is a guess. 30012 "Chess Move (DND)" was tried
-- here for one afternoon and is the cautionary tale: right mask, right range,
-- a 5-yard circle identical to dynamite's - and Stances 65536 (Battle Stance),
-- powerType 1 (rage), SpellFamilyName 4 (warrior). Using a crate switched a
-- warrior's stance and drew the red refusal cursor. See sql/custom/046.
--
-- AND ONE MORE FIELD DECIDES WHETHER A GIVEN POINT IS VALID, separately from
-- whether the reticle appears at all: EffectImplicitTargetA. 27651 was tried
-- here after 30012 - it passes the whole profile above - and drew the red
-- refusal cursor for points NEAR the player, green further out, because it
-- carries tgtA 52 (an enumeration target). 0 and 16 are the values that work,
-- and both proven item spells use one of them.
--
-- A CRATE NO LONGER CARRIES A RETICLE SPELL AT ALL. Everything from here to
-- @USE is the research that settled the AIMING spell, and aiming moved to the
-- Decorator's Chalk on 2026-09-03: one item aims every route into housing,
-- where a per-item spell could only ever aim these twelve. See sql/custom/054
-- for the chalk, 055 for what a crate carries now and why its CAST stayed when
-- its reticle went, and docs/notes/point-and-click-placing.md for the retired
-- design. The profiling METHOD below is the part still worth reading -- it is
-- what stopped a third wrong pick.
--
-- 261 "Summon Skeleton" was the settled answer for that job:
--
--   Targets   0x40    the reticle
--   tgtA      0       same as Target Dummy (4071), which works point-blank
--   Stances   0       any class, any form, no stance dance
--   powerType 0       no rage, no mana, no cost of any kind
--   family    0       generic, not somebody's class ability
--   range     0-20    minimum 0, so it is green at your feet; and 20 is under
--                     the server's own 40-yard clamp, so the client's range
--                     check does all the bounding and it can never aim
--                     somewhere the server would then refuse
--   radius    none    NO CIRCLE, just the placement cursor - what Target Dummy
--                     and Field Repair Bot (22700) draw, both shipped items
--                     that work. It is also the SMALLEST indicator available:
--                     no fully-gated spell draws a circle under 10 yards,
--                     which is twice dynamite's.
--   effect    42      never ran; back then pItemUse returned true before the
--                     cast, which is no longer how a crate works -- see 055
--   text      empty description AND tooltip, so the green Use line stays blank
--   attrs     0x0     nothing conditional at all
--   visual    3       confirmed in play to trigger no animation
--   cost      no reagent, no totem, no cooldown, instant
--
-- and it is present in server/dbc/Spell.dbc as well as the client's.
--
-- This was 482 "Reset" until 2026-09-02 - also blank, but Targets 0x0, so
-- placement went three yards ahead of the player and had to be nudged from
-- there. A client that has not re-queried the item still behaves that way; the
-- core falls back to it whenever the flag is absent.
--
-- If a client ever refuses Use on a reticle spell, 46096 - Turtle's own toy
-- dummy, which 75 shipped items prove passes the item cast check from a bag -
-- is the fallback, at the cost of the reticle AND an "Adds a toy to the
-- player's toy collection." line.
--
-- ---------------------------------------------------------------------------
--  WHAT A CRATE ACTUALLY CARRIES: 33453 "Over-Tinkered Lens"
-- ---------------------------------------------------------------------------
--  Targets 0x0 (no reticle -- the chalk aims), a 1000ms cast and SpellVisual
--  215, which 24 real recipes wear: putting furniture down should feel like
--  making something. sql/custom/052 binds the placement script to THIS entry,
--  which is what makes the number here load-bearing rather than cosmetic.
--
--  THIS WAS 261 UNTIL 2026-09-03 AND A RE-RUN WOULD HAVE BROKEN PLACEMENT.
--  Nothing is bound to 261, so crates carrying it cast a reticle spell that
--  reaches no script: right-click, no furniture, nothing in the log. 055 sets
--  this column too and would have put it back -- but only if somebody knew to
--  re-run it, and the file you re-ran was this one.
SET @USE    := 33453;

-- ---------------------------------------------------------------------------
--  The mapping table
-- ---------------------------------------------------------------------------
--  scale 0 means "the size the model ships at", the same convention
--  house_object.scale uses. Set it per row to ship a model bigger or smaller
--  than the world uses it; nothing does yet.
--
--  category IS THE VENDOR'S TAB AND THE SHELF'S FILTER, one string doing both
--  jobs. Any label works -- the core groups by whatever it finds and knows
--  none of these names -- but every value below was COMPUTED by
--  tools/categorise.js rather than typed, so the tab a crate is bought from,
--  the filter it lands under on the shelf, and the bucket the addon's
--  catalogue browser files its model in are the same answer from one rule.
--  gen-furniture-items.js re-derives it and warns if this column has drifted.
--
--  The order the tabs are drawn in is the order the categories FIRST APPEAR
--  by item entry, so it is the row order below and needs no sort column:
--  Seating (100100), Tables (100102), Storage (100104), Odds & Ends (100106).
CREATE TABLE IF NOT EXISTS `house_furniture_item` (
  `item_entry` INT UNSIGNED NOT NULL              COMMENT 'item_template.entry',
  `go_entry`   INT UNSIGNED NOT NULL              COMMENT 'gameobject_template.entry',
  `scale`      FLOAT        NOT NULL DEFAULT 0    COMMENT '0 = the model at its own size',
  `category`   VARCHAR(24)  NOT NULL DEFAULT ''   COMMENT 'vendor tab / shelf filter; blank = untabbed',
  PRIMARY KEY (`item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

--  An install that already has the table needs the column adding, which
--  CREATE TABLE IF NOT EXISTS will not do -- that is sql/custom/062, and it
--  has to be applied BEFORE the binary that selects the column.

--  TWO ITEM RANGES, AND THEY HAVE TO STAY TWO. 100100-100199 was the original
--  block and it is nearly full; the next hundred are not available, because
--  100200-100299 is housing tools (the Decorator's Chalk is 100200) and
--  100300-100399 is critter crates. So furniture continues at 100400-100999.
--
--  NOTHING IS RENUMBERED INTO ONE CONTIGUOUS BLOCK. tw_char.item_instance,
--  house_object.item_entry and house_storage.item_entry all reference these
--  entries and there are live rows in all three -- moving one orphans somebody's
--  furniture. Two ranges is the correct answer, not a wart to tidy.
--
--  The ranges live in website/lib/furniture.js now, which allocates from them.
--
--  NO FURNITURE IS DELETED HERE (see the header). The two vendors' creature
--  rows are still owned by this file and are rewritten. Their stock is not:
--  deleting npc_vendor would wipe crates the page added to Adela, and the
--  Decorator's Chalk that 054 puts on Bram's shelf.
DELETE FROM creature_template    WHERE entry IN (@VENDOR, @SHOP);

-- ---------------------------------------------------------------------------
--  The stock
-- ---------------------------------------------------------------------------
--  The first twelve were chosen off the ComfyHousing catalogue, every one of
--  them from World/Generic/Human/Passive Doodads - one art set, so they furnish
--  a room together instead of looking like a sample sheet.
--
--  THE BEDS AT 100112-100114 BREAK THAT RULE ON PURPOSE (2026-09-03). A bed is
--  the one piece somebody picks for its LOOK rather than for matching the
--  chairs, so dwarf, night elf and Westfall are three different rooms to build
--  around rather than three near-identical planks. The human Feather Bed
--  (100103) is still there for anyone who wants the matched set.
--
--  100115-100163 (2026-09-04) ABANDON IT ENTIRELY, and that is what the tabbed
--  vendor is for. Forty-nine pieces across six art sets is not a matched room,
--  it is a shop - and a shop is only browsable if it is sorted, which is the
--  whole reason Adela has tabs and Bram does not.
--
--  EVERY NAME HERE WAS READ OFF THE RENDERED MODEL, not off the file path, and
--  that is not fussiness. "Garden Bench 02" is a TREE STUMP and "Garden Bench
--  03" is a mossy rock; "Chair" and "Bench" at 136929/136945 are both Dark Iron
--  chairs; "Sherpa_Bedroll_0" is unrolled and "_2" is tied shut. A player
--  buying furniture is buying a shape, so a name derived from a filename sells
--  them the wrong thing. Render one with:
--    node tools/model-browser/render.js --display <id> --sheet
--
--  DROPPED FROM THE REQUESTED LIST, both silently useless rather than wrong:
--    2003189  AbbeyShelf01_Unselectable - the same model AND the same thumbnail
--             as 2003188, so it would be a second identical shelf at the same
--             price with no way to tell them apart in the bag.
--    2003178  already sold as 100103 Feather Bed.
--    2002232  already sold as 100114 Westfall Bed.
--
--  Every entry here has a thumbnail already, which is what the addon's hover
--  preview and its storage list draw. Check before adding one:
--    grep -o "\[<goEntry>\]=[0-9]*" addon/ComfyHousing/Thumbs.lua
--  and confirm the numbered .blp exists under addon/ComfyHousing/thumbs/.
--
--  PRICES GO BY KIND, not by art set: a seat is 30s-75s, a table 75s-1g50s, a
--  shelf 1g50s-2g50s, a bed 2g50s, a cut flower 25s, a banner 1g50s, a rug
--  2g-3g and a framed painting 4g. Charging more for the dwarven table than
--  the gnome one would be pricing taste. sell_price is a fifth of buy_price
--  throughout, which is what the original fifteen already did.
--
--  material 2 is wood, which is only the pick-up sound.
INSERT IGNORE INTO item_template
  (entry, class, subclass, name, description, display_id, quality, flags,
   buy_price, sell_price, inventory_type, allowable_class, allowable_race,
   item_level, required_level, stackable, max_count, bonding, material, sheath,
   spellid_1, spelltrigger_1, spellcharges_1, script_name)
VALUES
-- the seed: item_template, frozen 2026-10-08
  (100100,0,0,'Wooden Chair',            @DESC,@ICON,1,0,5000  ,1000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100101,0,0,'Wooden Bench',            @DESC,@ICON,1,0,7500  ,1500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100102,0,0,'Inn Table',               @DESC,@ICON,1,0,10000 ,2000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100103,0,0,'Feather Bed',             @DESC,@ICON,1,0,25000 ,5000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100104,0,0,'Tall Bookshelf',          @DESC,@ICON,1,0,20000 ,4000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100105,0,0,'Foot Locker',             @DESC,@ICON,1,0,12500 ,2500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100106,0,0,'Iron Lantern',            @DESC,@ICON,1,0,7500  ,1500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100107,0,0,'Stone Brazier',           @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100108,0,0,'Bearskin Rug',            @DESC,@ICON,1,0,30000 ,6000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100109,0,0,'Framed Painting',         @DESC,@ICON,1,0,40000 ,8000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100110,0,0,'Stack of Books',          @DESC,@ICON,1,0,5000  ,1000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100111,0,0,'Ale Keg',                 @DESC,@ICON,1,0,10000 ,2000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100112,0,0,'Dwarven Bed',             @DESC,@ICON,1,0,25000 ,5000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100113,0,0,'Elven Bed',               @DESC,@ICON,1,0,25000 ,5000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100114,0,0,'Westfall Bed',            @DESC,@ICON,1,0,25000 ,5000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100115,0,0,'Dark Iron Chair',         @DESC,@ICON,1,0,5000  ,1000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100116,0,0,'Dark Iron Bench',         @DESC,@ICON,1,0,7500  ,1500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100117,0,0,'Dark Iron Seat',          @DESC,@ICON,1,0,7500  ,1500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100118,0,0,'Stone Bench',             @DESC,@ICON,1,0,7500  ,1500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100119,0,0,'Stormwind Bench',         @DESC,@ICON,1,0,7500  ,1500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100120,0,0,'Log Seat',                @DESC,@ICON,1,0,3000  ,600  ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100121,0,0,'Westfall Stool',          @DESC,@ICON,1,0,3000  ,600  ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100122,0,0,'Garden Bench',            @DESC,@ICON,1,0,7500  ,1500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100123,0,0,'Stump Seat',              @DESC,@ICON,1,0,3000  ,600  ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100124,0,0,'Mossy Stone Seat',        @DESC,@ICON,1,0,5000  ,1000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100125,0,0,'Dark Iron Bed',           @DESC,@ICON,1,0,25000 ,5000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100126,0,0,'Gnome Bed',               @DESC,@ICON,1,0,25000 ,5000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100127,0,0,'Bedroll',                 @DESC,@ICON,1,0,10000 ,2000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100128,0,0,'Packed Bedroll',          @DESC,@ICON,1,0,10000 ,2000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100129,0,0,'Dwarven Table',           @DESC,@ICON,1,0,10000 ,2000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100130,0,0,'Checkered Table',         @DESC,@ICON,1,0,12500 ,2500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100131,0,0,'Long Checkered Table',    @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100132,0,0,'Round Wooden Table',      @DESC,@ICON,1,0,10000 ,2000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100133,0,0,'Draped Stone Table',      @DESC,@ICON,1,0,12500 ,2500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100134,0,0,'Small Inn Table',         @DESC,@ICON,1,0,7500  ,1500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100135,0,0,'Uldaman Stone Table',     @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100136,0,0,'Reading Lectern',         @DESC,@ICON,1,0,12500 ,2500 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100137,0,0,'Abbey Bookshelf',         @DESC,@ICON,1,0,20000 ,4000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100138,0,0,'Dwarven Shelf',           @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100139,0,0,'Dwarven Bookcase',        @DESC,@ICON,1,0,25000 ,5000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100140,0,0,'Tall Dwarven Bookshelf',  @DESC,@ICON,1,0,20000 ,4000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100141,0,0,'Low Dwarven Shelf',       @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100142,0,0,'Vase of Marigolds',       @DESC,@ICON,1,0,5000  ,1000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100143,0,0,'Flower Planter',          @DESC,@ICON,1,0,5000  ,1000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100144,0,0,'Orange Dreamflower',      @DESC,@ICON,1,0,2500  ,500  ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100145,0,0,'Purple Dreamflower',      @DESC,@ICON,1,0,2500  ,500  ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100146,0,0,'Drooping Dreamflower',    @DESC,@ICON,1,0,2500  ,500  ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100147,0,0,'Elwynn Wildflowers',      @DESC,@ICON,1,0,2500  ,500  ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100148,0,0,'Bunch of White Flowers',  @DESC,@ICON,1,0,2500  ,500  ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100149,0,0,'Troll Stone Tablet',      @DESC,@ICON,1,0,20000 ,4000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100150,0,0,'Eagle Statue',            @DESC,@ICON,1,0,35000 ,7000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100151,0,0,'Uldaman Banner',          @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100152,0,0,'Wide Blue Banner',        @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100153,0,0,'Wide Purple Banner',      @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100154,0,0,'Wide Red Banner',         @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100155,0,0,'Tall Red Banner',         @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100156,0,0,'Horde Banner',            @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100157,0,0,'Planet Painting',         @DESC,@ICON,1,0,40000 ,8000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100158,0,0,'Dark Forest Painting',    @DESC,@ICON,1,0,40000 ,8000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100159,0,0,'Oval Portrait',           @DESC,@ICON,1,0,40000 ,8000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100160,0,0,'Dalaran Landscape',       @DESC,@ICON,1,0,40000 ,8000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100161,0,0,'Purple Rug',              @DESC,@ICON,1,0,30000 ,6000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100162,0,0,'Small Blue Rug',          @DESC,@ICON,1,0,20000 ,4000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100163,0,0,'Small Purple Rug',        @DESC,@ICON,1,0,20000 ,4000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT),
  (100164,0,0,'Woven Tauren Hanging',    @DESC,@ICON,1,0,15000 ,3000 ,0,-1,-1,1,0,1,0,0,2,0,@USE,0,0,@SCRIPT);

--  CATEGORIES ARE COMPUTED, NEVER TYPED - see the CREATE TABLE above. Regenerate
--  this block rather than editing a label by hand, or the vendor tab and the
--  shelf filter start disagreeing about where a keg lives.
INSERT IGNORE INTO house_furniture_item (item_entry, go_entry, scale, category) VALUES
-- the seed: house_furniture_item, frozen 2026-10-08
  (100100, 2413,    0, 'Seating'     ), -- Wooden Chair
  (100101, 24538,   0, 'Seating'     ), -- Wooden Bench
  (100102, 2003527, 0, 'Tables'      ), -- Inn Table
  (100103, 2003178, 0, 'Seating'     ), -- Feather Bed
  (100104, 1000013, 0, 'Storage'     ), -- Tall Bookshelf
  (100105, 2003323, 0, 'Storage'     ), -- Foot Locker
  (100106, 2003377, 0, 'Odds & Ends' ), -- Iron Lantern
  (100107, 2003202, 0, 'Odds & Ends' ), -- Stone Brazier
  (100108, 2003428, 0, 'Odds & Ends' ), -- Bearskin Rug
  (100109, 2010964, 0, 'Odds & Ends' ), -- Framed Painting
  (100110, 2003196, 0, 'Odds & Ends' ), -- Stack of Books
  (100111, 2003179, 0, 'Odds & Ends' ), -- Ale Keg
  (100112, 2002699, 0, 'Seating'     ), -- Dwarven Bed
  (100113, 2003599, 0, 'Seating'     ), -- Elven Bed
  (100114, 2002232, 0, 'Seating'     ), -- Westfall Bed
  (100115, 136929,  0, 'Seating'     ), -- Dark Iron Chair
  (100116, 136945,  0, 'Seating'     ), -- Dark Iron Bench
  (100117, 2002620, 0, 'Seating'     ), -- Dark Iron Seat
  (100118, 24397,   0, 'Seating'     ), -- Stone Bench
  (100119, 2003187, 0, 'Seating'     ), -- Stormwind Bench
  (100120, 2004608, 0, 'Seating'     ), -- Log Seat
  (100121, 2002271, 0, 'Seating'     ), -- Westfall Stool
  (100122, 2003611, 0, 'Seating'     ), -- Garden Bench
  (100123, 2003612, 0, 'Seating'     ), -- Stump Seat
  (100124, 2003613, 0, 'Seating'     ), -- Mossy Stone Seat
  (100125, 2002612, 0, 'Seating'     ), -- Dark Iron Bed
  (100126, 2002936, 0, 'Seating'     ), -- Gnome Bed
  (100127, 3000303, 0, 'Seating'     ), -- Bedroll
  (100128, 3000304, 0, 'Seating'     ), -- Packed Bedroll
  (100129, 2002873, 0, 'Tables'      ), -- Dwarven Table
  (100130, 2002879, 0, 'Tables'      ), -- Checkered Table
  (100131, 2002882, 0, 'Tables'      ), -- Long Checkered Table
  (100132, 2002884, 0, 'Tables'      ), -- Round Wooden Table
  (100133, 2002943, 0, 'Tables'      ), -- Draped Stone Table
  (100134, 180885,  0, 'Tables'      ), -- Small Inn Table
  (100135, 2006364, 0, 'Tables'      ), -- Uldaman Stone Table
  (100136, 2002890, 0, 'Tables'      ), -- Reading Lectern
  (100137, 2003188, 0, 'Storage'     ), -- Abbey Bookshelf
  (100138, 2002704, 0, 'Storage'     ), -- Dwarven Shelf
  (100139, 2002710, 0, 'Storage'     ), -- Dwarven Bookcase
  (100140, 2002711, 0, 'Storage'     ), -- Tall Dwarven Bookshelf
  (100141, 2002715, 0, 'Storage'     ), -- Low Dwarven Shelf
  (100142, 2000456, 0, 'Odds & Ends' ), -- Vase of Marigolds
  (100143, 2000402, 0, 'Odds & Ends' ), -- Flower Planter
  (100144, 2002331, 0, 'Odds & Ends' ), -- Orange Dreamflower
  (100145, 2002332, 0, 'Odds & Ends' ), -- Purple Dreamflower
  (100146, 2000337, 0, 'Odds & Ends' ), -- Drooping Dreamflower
  (100147, 2001853, 0, 'Odds & Ends' ), -- Elwynn Wildflowers
  (100148, 2003315, 0, 'Odds & Ends' ), -- Bunch of White Flowers
  (100149, 2009735, 0, 'Odds & Ends' ), -- Troll Stone Tablet
  (100150, 2009734, 0, 'Odds & Ends' ), -- Eagle Statue
  (100151, 2010594, 0, 'Odds & Ends' ), -- Uldaman Banner
  (100152, 2008609, 0, 'Odds & Ends' ), -- Wide Blue Banner
  (100153, 2008610, 0, 'Odds & Ends' ), -- Wide Purple Banner
  (100154, 2008611, 0, 'Odds & Ends' ), -- Wide Red Banner
  (100155, 2002487, 0, 'Odds & Ends' ), -- Tall Red Banner
  (100156, 2000387, 0, 'Odds & Ends' ), -- Horde Banner
  (100157, 1000016, 0, 'Odds & Ends' ), -- Planet Painting
  (100158, 2010941, 0, 'Odds & Ends' ), -- Dark Forest Painting
  (100159, 2010942, 0, 'Odds & Ends' ), -- Oval Portrait
  (100160, 2010646, 0, 'Odds & Ends' ), -- Dalaran Landscape
  (100161, 2008707, 0, 'Odds & Ends' ), -- Purple Rug
  (100162, 2008711, 0, 'Odds & Ends' ), -- Small Blue Rug
  (100163, 2008712, 0, 'Odds & Ends' ), -- Small Purple Rug
  (100164, 2004709, 0, 'Odds & Ends' ); -- Woven Tauren Hanging

-- ---------------------------------------------------------------------------
--  The vendors
-- ---------------------------------------------------------------------------
--  TWO OF THEM, AND THE SECOND SUPERSEDES THE FIRST. Carpenter Bram is
--  untouched: the same fifteen crates, one flat vendor window, no dialog. He
--  is left exactly as he was so an existing spawn keeps working and nothing
--  a player has already bought changes.
--
--  Furnisher Adela carries ALL SIXTY-FOUR behind a menu, one page per
--  category. Sixty-four rows in one vendor window is four pages of scrolling
--  past beds to reach a rug, which is the whole reason she exists.
--
--  So Bram is now redundant. Keep him as a starter counter or .npc delete
--  him; both are fine and neither needs an SQL change.
--
--  npc_flags: 4 is UNIT_NPC_FLAG_VENDOR in the 1.12 enum (not 128), 1 is
--  GOSSIP. ADELA NEEDS BOTH AND FOR DIFFERENT REASONS - gossip so the client
--  sends CMSG_GOSSIP_HELLO instead of opening a vendor window over the menu,
--  and vendor so GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_VENDOR) inside
--  SendListInventory and BuyItemFromVendor still finds her. Drop either one
--  and the tabs are unreachable or unbuyable.
--
--  script_name is what makes her tabbed rather than flat: it binds
--  pGossipHello/pGossipSelect, and the menu is built in code so none of this
--  costs a gossip_menu row out of the nearly-full smallint space. Point it at
--  a second creature and there is a second tabbed shop, no code change.
--
--  faction 35 = friendly to everyone.
INSERT INTO creature_template
  (entry, name, subname, display_id1, faction, npc_flags,
   level_min, level_max, health_min, health_max, armor, unit_class, type,
   speed_walk, speed_run, scale, base_attack_time, ranged_attack_time,
   dmg_min, dmg_max, movement_type, inhabit_type, regeneration, ai_name, script_name)
VALUES
  (@VENDOR, 'Carpenter Bram', 'Furnishings', 3264, 35, 4,
   60, 60, 4007, 4007, 3272, 1, 7,
   1, 1.14286, 1, 2000, 2000,
   103.4, 129.8, 0, 3, 3, '', ''),
  (@SHOP, 'Furnisher Adela', 'Fine Furnishings', 3260, 35, 5,
   60, 60, 4007, 4007, 3272, 1, 7,
   1, 1.14286, 1, 2000, 2000,
   103.4, 129.8, 0, 3, 3, '', 'npc_house_furniture_shop');

--  maxcount 0 -> unlimited stock, so incrtime 0. Prices are buy_price above.
--
--  MAX_VENDOR_ITEMS IS 128 and it is a PROTOCOL limit, not a tuning knob - the
--  item count in SMSG_LIST_INVENTORY is one byte's worth of slots. Adela is at
--  64, so there is room for another sixty-odd pieces before a second shop NPC
--  is forced. The tabs do NOT help with this: they filter what is SENT, and
--  every tab is drawn from the one npc_vendor list below.
INSERT IGNORE INTO npc_vendor (entry, slot, item, maxcount, incrtime) VALUES
-- the seed: Adela's stock, frozen 2026-10-08. The page adds the rest.
  (@SHOP, 1  , 100100, 0, 0), -- Seating     Wooden Chair
  (@SHOP, 2  , 100101, 0, 0), -- Seating     Wooden Bench
  (@SHOP, 3  , 100102, 0, 0), -- Tables      Inn Table
  (@SHOP, 4  , 100103, 0, 0), -- Seating     Feather Bed
  (@SHOP, 5  , 100104, 0, 0), -- Storage     Tall Bookshelf
  (@SHOP, 6  , 100105, 0, 0), -- Storage     Foot Locker
  (@SHOP, 7  , 100106, 0, 0), -- Odds & Ends Iron Lantern
  (@SHOP, 8  , 100107, 0, 0), -- Odds & Ends Stone Brazier
  (@SHOP, 9  , 100108, 0, 0), -- Odds & Ends Bearskin Rug
  (@SHOP, 10 , 100109, 0, 0), -- Odds & Ends Framed Painting
  (@SHOP, 11 , 100110, 0, 0), -- Odds & Ends Stack of Books
  (@SHOP, 12 , 100111, 0, 0), -- Odds & Ends Ale Keg
  (@SHOP, 13 , 100112, 0, 0), -- Seating     Dwarven Bed
  (@SHOP, 14 , 100113, 0, 0), -- Seating     Elven Bed
  (@SHOP, 15 , 100114, 0, 0), -- Seating     Westfall Bed
  (@SHOP, 16 , 100115, 0, 0), -- Seating     Dark Iron Chair
  (@SHOP, 17 , 100116, 0, 0), -- Seating     Dark Iron Bench
  (@SHOP, 18 , 100117, 0, 0), -- Seating     Dark Iron Seat
  (@SHOP, 19 , 100118, 0, 0), -- Seating     Stone Bench
  (@SHOP, 20 , 100119, 0, 0), -- Seating     Stormwind Bench
  (@SHOP, 21 , 100120, 0, 0), -- Seating     Log Seat
  (@SHOP, 22 , 100121, 0, 0), -- Seating     Westfall Stool
  (@SHOP, 23 , 100122, 0, 0), -- Seating     Garden Bench
  (@SHOP, 24 , 100123, 0, 0), -- Seating     Stump Seat
  (@SHOP, 25 , 100124, 0, 0), -- Seating     Mossy Stone Seat
  (@SHOP, 26 , 100125, 0, 0), -- Seating     Dark Iron Bed
  (@SHOP, 27 , 100126, 0, 0), -- Seating     Gnome Bed
  (@SHOP, 28 , 100127, 0, 0), -- Seating     Bedroll
  (@SHOP, 29 , 100128, 0, 0), -- Seating     Packed Bedroll
  (@SHOP, 30 , 100129, 0, 0), -- Tables      Dwarven Table
  (@SHOP, 31 , 100130, 0, 0), -- Tables      Checkered Table
  (@SHOP, 32 , 100131, 0, 0), -- Tables      Long Checkered Table
  (@SHOP, 33 , 100132, 0, 0), -- Tables      Round Wooden Table
  (@SHOP, 34 , 100133, 0, 0), -- Tables      Draped Stone Table
  (@SHOP, 35 , 100134, 0, 0), -- Tables      Small Inn Table
  (@SHOP, 36 , 100135, 0, 0), -- Tables      Uldaman Stone Table
  (@SHOP, 37 , 100136, 0, 0), -- Tables      Reading Lectern
  (@SHOP, 38 , 100137, 0, 0), -- Storage     Abbey Bookshelf
  (@SHOP, 39 , 100138, 0, 0), -- Storage     Dwarven Shelf
  (@SHOP, 40 , 100139, 0, 0), -- Storage     Dwarven Bookcase
  (@SHOP, 41 , 100140, 0, 0), -- Storage     Tall Dwarven Bookshelf
  (@SHOP, 42 , 100141, 0, 0), -- Storage     Low Dwarven Shelf
  (@SHOP, 43 , 100142, 0, 0), -- Odds & Ends Vase of Marigolds
  (@SHOP, 44 , 100143, 0, 0), -- Odds & Ends Flower Planter
  (@SHOP, 45 , 100144, 0, 0), -- Odds & Ends Orange Dreamflower
  (@SHOP, 46 , 100145, 0, 0), -- Odds & Ends Purple Dreamflower
  (@SHOP, 47 , 100146, 0, 0), -- Odds & Ends Drooping Dreamflower
  (@SHOP, 48 , 100147, 0, 0), -- Odds & Ends Elwynn Wildflowers
  (@SHOP, 49 , 100148, 0, 0), -- Odds & Ends Bunch of White Flowers
  (@SHOP, 50 , 100149, 0, 0), -- Odds & Ends Troll Stone Tablet
  (@SHOP, 51 , 100150, 0, 0), -- Odds & Ends Eagle Statue
  (@SHOP, 52 , 100151, 0, 0), -- Odds & Ends Uldaman Banner
  (@SHOP, 53 , 100152, 0, 0), -- Odds & Ends Wide Blue Banner
  (@SHOP, 54 , 100153, 0, 0), -- Odds & Ends Wide Purple Banner
  (@SHOP, 55 , 100154, 0, 0), -- Odds & Ends Wide Red Banner
  (@SHOP, 56 , 100155, 0, 0), -- Odds & Ends Tall Red Banner
  (@SHOP, 57 , 100156, 0, 0), -- Odds & Ends Horde Banner
  (@SHOP, 58 , 100157, 0, 0), -- Odds & Ends Planet Painting
  (@SHOP, 59 , 100158, 0, 0), -- Odds & Ends Dark Forest Painting
  (@SHOP, 60 , 100159, 0, 0), -- Odds & Ends Oval Portrait
  (@SHOP, 61 , 100160, 0, 0), -- Odds & Ends Dalaran Landscape
  (@SHOP, 62 , 100161, 0, 0), -- Odds & Ends Purple Rug
  (@SHOP, 63 , 100162, 0, 0), -- Odds & Ends Small Blue Rug
  (@SHOP, 64 , 100163, 0, 0), -- Odds & Ends Small Purple Rug
  (@SHOP, 65 , 100164, 0, 0); -- Odds & Ends Woven Tauren Hanging

--  Bram keeps the original fifteen and nothing else.
INSERT IGNORE INTO npc_vendor (entry, slot, item, maxcount, incrtime) VALUES
  (@VENDOR, 1  , 100100, 0, 0), -- Seating     Wooden Chair
  (@VENDOR, 2  , 100101, 0, 0), -- Seating     Wooden Bench
  (@VENDOR, 3  , 100102, 0, 0), -- Tables      Inn Table
  (@VENDOR, 4  , 100103, 0, 0), -- Seating     Feather Bed
  (@VENDOR, 5  , 100104, 0, 0), -- Storage     Tall Bookshelf
  (@VENDOR, 6  , 100105, 0, 0), -- Storage     Foot Locker
  (@VENDOR, 7  , 100106, 0, 0), -- Odds & Ends Iron Lantern
  (@VENDOR, 8  , 100107, 0, 0), -- Odds & Ends Stone Brazier
  (@VENDOR, 9  , 100108, 0, 0), -- Odds & Ends Bearskin Rug
  (@VENDOR, 10 , 100109, 0, 0), -- Odds & Ends Framed Painting
  (@VENDOR, 11 , 100110, 0, 0), -- Odds & Ends Stack of Books
  (@VENDOR, 12 , 100111, 0, 0), -- Odds & Ends Ale Keg
  (@VENDOR, 13 , 100112, 0, 0), -- Seating     Dwarven Bed
  (@VENDOR, 14 , 100113, 0, 0), -- Seating     Elven Bed
  (@VENDOR, 15 , 100114, 0, 0); -- Seating     Westfall Bed
