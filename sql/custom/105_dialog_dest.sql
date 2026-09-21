-- ============================================================================
--  Dialog destinations: named places a BUTTON can send a PLAYER       (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\105_dialog_dest.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  APPLY BEFORE THE BINARY. `.dest` is a chat command in the core and
--  DialogDest::LoadAll runs during startup, and a missing table here is not a
--  warning: it is `ASSERT(false)` in DatabaseMysql.cpp and a crash dump. The
--  same rule `dialog_spot` and the bug tracker's table have.
--
--  ------------------------------------------------------- WHY NOT dialog_spot
--
--  Because a spot and a destination are two different things wearing the same
--  six columns, and merging them would have made every rule about one a lie
--  about the other:
--
--    * A SPOT IS WHERE AN NPC WALKS. Same map by definition -- MOVE_TO moves
--      its source from wherever it is standing and has no notion of a map at
--      all, so a spot's `map` column exists only so the editor can refuse.
--      A DESTINATION'S MAP IS LOAD BEARING: Player::TeleportTo is the whole
--      point and a cross-continent one is the ordinary case.
--
--    * A SPOT BELONGS TO AN NPC, by the `<NPC>: <name>` rule `.spot` enforces.
--      A destination belongs to a LIST, which is not an NPC: several NPCs may
--      send people to one list and one NPC may offer three.
--
--    * A SPOT IS CAPTURED WITH THE NPC SELECTED, which is exactly what you
--      cannot do standing on a beach in Tanaris with the NPC in Stormwind.
--
--    * AND THE BIG ONE: a script stores the POINT of a spot, not the spot, so
--      renaming or deleting one cannot reach into a scene. A random pick has
--      to happen when the button is pressed, so a teleport script stores the
--      LIST ID and resolves it at run time. That is a deliberate break from the
--      rule next door, and it is why this table is loaded into memory the way
--      dialog_spot is rather than merely read by the website.
--
--  ---------------------------------------------------------------- CAPTURE
--
--  In game, standing on the spot you mean:
--
--      .dest wandering the old well
--
--  First word is the LIST, the rest is the destination's name. The list is
--  created by naming it, which is the same bargain `.spot` makes: the capture
--  happens where the information is, and a round trip to a web form to declare
--  a list before you may stand in it would be a round trip for nothing. The
--  cost is that a typo makes a new list, which is what `.dest` with no
--  arguments is for -- it prints every list and how many places are in it.
--
--  A LIST NAME IS ONE WORD because the command has to split the line somewhere
--  and "first word" is the split that needs no punctuation. Hyphens and
--  underscores are words.
--
--  ---------------------------------------------------------------- ID BLOCKS
--
--  Both AUTO_INCREMENT from 1, and deliberately not one of this folder's
--  reserved ranges. A list id DOES travel: `gossip_scripts.datalong` holds one
--  under SCRIPT_COMMAND_TELEPORT_RANDOM (100). So unlike a spot id, deleting a
--  list out from under a button is a button that does nothing -- which is why
--  `.dest del` removes A PLACE and there is no chat command that removes a
--  LIST. An empty list is harmless by comparison: one line in the log when
--  somebody presses the button, and it can be refilled from where you stand.
--
--  Re-runnable: CREATE TABLE IF NOT EXISTS, and no seed data.
-- ============================================================================

CREATE TABLE IF NOT EXISTS `dialog_dest_list` (
  `id`      smallint(5) unsigned NOT NULL AUTO_INCREMENT,
  -- What the button says it sends you to: "wandering", "pads", "graveyards".
  `name`    varchar(48)          NOT NULL DEFAULT '',
  -- Who typed .dest first, so an odd list can be asked about.
  `author`  varchar(32)          NOT NULL DEFAULT '',
  PRIMARY KEY (`id`),
  UNIQUE KEY `name` (`name`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci;

CREATE TABLE IF NOT EXISTS `dialog_dest` (
  `id`      smallint(5) unsigned NOT NULL AUTO_INCREMENT,
  -- dialog_dest_list.id. Not a foreign key: MyISAM has none, and the loader
  -- is the thing that would notice anyway.
  `list`    smallint(5) unsigned NOT NULL DEFAULT 0,
  `name`    varchar(48)          NOT NULL DEFAULT '',
  -- READ, unlike dialog_spot.map. This is a Player::TeleportTo.
  `map`     smallint(5) unsigned NOT NULL DEFAULT 0,
  `x`       float                NOT NULL DEFAULT 0,
  `y`       float                NOT NULL DEFAULT 0,
  `z`       float                NOT NULL DEFAULT 0,
  -- Which way to face on arrival. Stored as captured, and used as captured:
  -- TeleportTo takes an orientation and has no "do not care" the way MoveTo
  -- does, so 0 here really is facing due east.
  `o`       float                NOT NULL DEFAULT 0,
  `author`  varchar(32)          NOT NULL DEFAULT '',
  PRIMARY KEY (`id`),
  -- One name per list, so typing the same name again MOVES the destination
  -- rather than making a second one. The REPLACE in `.dest` depends on this.
  UNIQUE KEY `list_name` (`list`, `name`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci;
