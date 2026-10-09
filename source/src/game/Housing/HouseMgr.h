/*
 * Player housing.
 *
 * A house is one instance of the house map, owned by an account, with the
 * objects the owner has placed in it stored in `tw_char.house_object`.
 *
 * The core already has every mechanism this needs; the manager mostly wires
 * them together. Three of them are worth knowing before reading the .cpp:
 *
 *  - Which instance a player enters is decided in exactly one place,
 *    Player::GetBoundInstanceSaveForSelfOrGroup(), and a PERMANENT bind wins
 *    over a group bind. So "go to my house" is just a permanent bind plus an
 *    ordinary teleport.
 *  - A grid loads its objects from two sets: the global spawns keyed by map,
 *    and MapPersistentState's own per-instance set (ObjectGridLoader.cpp:207).
 *    Furniture goes in the second, which is why one player's chair is not in
 *    everybody else's house.
 *  - An instance with a far-future reset time is never collected:
 *    ScheduleAllDungeonResets queues it beyond any horizon,
 *    _CleanupExpiredInstancesAtTime only deletes rows whose resettime is in
 *    the past, and CleanupInstances only deletes rows nothing is bound to.
 *    That is why houses need no change to the reset machinery.
 */

#ifndef MANGOS_H_HOUSEMGR
#define MANGOS_H_HOUSEMGR

#include "Common.h"
#include "ObjectGuid.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class Player;
class Map;
class GameObject;
class Creature;
struct GameObjectData;
struct WorldLocation;

// map_template.entry 28, made instanceable by sql/custom/029_house_map_28.sql.
// Not a config: the house map is a content decision baked into that SQL and
// into the plot below.
//
// WAS 169 "Emerald Dream" until 2026-08-30 (sql/custom/015). 169 was chosen for
// having terrain worth standing on, which turned out to be optimising for the
// wrong thing -- a house plot wants flat ground, and 169 cost 33.4 MB of
// sculpted terrain with 81 yards of average relief AND SHIPPED NO MMAPS, so
// nothing could pathfind in a house. Map 28 is 1.4 MB, twelve tiles of one
// continuous plane at exactly z=0, has mmaps, and carries no creature,
// gameobject or areatrigger of its own -- which matters because spawns are
// keyed by map and would appear in every instance. See 029 for the measurements
// and for why the two PERFECTLY flat maps are traps.
#define HOUSE_MAP_ID            28

// Where a new house drops you. Picked by standing there and reading .gps rather
// than computed, which is why it is not a round number -- it sits in tile 02/01
// of map 28's flat block (the block spans x/y 14,933..17,067). That tile is
// dead flat with nothing built on it, and the ground is at exactly 0, not
// approximately: every tile of this map stores a single uniform height.
#define HOUSE_ENTRY_X           (15801.0f)
#define HOUSE_ENTRY_Y           (16394.0f)
#define HOUSE_ENTRY_Z           (0.0f)
#define HOUSE_ENTRY_O           (0.0f)

// house_object.id IS the gameobject low guid -- one number, so nothing can
// drift out of sync and .gobject takes it directly.
//
// The range is not arbitrary. GameObject low guids are 24 BITS here:
// ObjectGuid::HasEntry(HIGHGUID_GAMEOBJECT) is true, so the counter is masked
// to 0x00FFFFFF and the hard ceiling is 16,777,215. Of that:
//
//   1 .. 7,002,997     tw_world.gameobject, in use today
//   7,002,998 ..       .gobject add, from GuidReserveSize.GameObject
//   8,000,000 ..       house furniture, this block
//   12,002,998 ..      temporary summons, per map instance
//   .. 16,777,215      ceiling
//
// That layout REQUIRES GuidReserveSize.GameObject = 5000000 in mangosd.conf.
// It ships at 1000, which would put the temporary floor at 7,003,998 -- below
// us. HouseMgr::LoadFromDB refuses to run if the setting has not been raised,
// rather than letting furniture and temporary summons collide silently.
// CLICKING A HOUSING GAMEOBJECT, from the two places such a click arrives:
// GameObject::Use and the IsGameObject() branch of
// HandleGossipSelectOptionOpcode. Free functions rather than script hooks
// because furniture uses stock gameobject_template rows we do not own, so
// script_name is not ours to set and pGOHello / pGOGossipSelect can never fire
// for one. Defined in HouseHandle.cpp beside the menu they open.
//
// Both answer false unless the object is in that player's edit session -- the
// same question the per-viewer GOOBER override asks in
// Object::BuildValuesUpdate, so a click can only reach a menu on something the
// player was already being shown as clickable.
class Player;
bool HouseGameObjectUse(Player* player, uint32 goGuidLow);
bool HouseGameObjectGossipSelect(Player* player, uint32 goGuidLow, uint32 sender,
                                 uint32 action, const char* code);

// MAKING A SPAWNED CRITTER HARMLESS, called from Creature::LoadFromDB for
// anything in the house critter guid block. A free function for the same
// reason as the two above, and it lives on that call because LoadFromDB is the
// one funnel BOTH spawn routes share -- HouseMgr::SpawnCritterNow when you
// place one, and ObjectGridLoader when somebody walks into a house that
// already has one. Dressing at the placement site would give you a rabbit that
// stayed harmless only until the first relog.
void HouseDressCritter(Creature* creature);

// PATTING A HOUSE CRITTER, called from WorldSession::HandleTextEmoteOpcode for
// any creature target. `/pat` on the bunny plays hearts and turns it to face
// you.
//
// The core already has a seam for this -- Creature::AI()->ReceiveEmote, which
// that handler calls two lines later -- and it is NOT available to us: it needs
// our own AI class, bound through creature_template.script_name, and those 129
// critter rows are Turtle's. So this is a free function beside the gameobject
// click for exactly the same reason.
void HouseCritterEmote(Player* player, uint32 creatureGuidLow, uint32 textEmote);

// CLICKING A HOUSE CRITTER, from the two places such a click arrives:
// HandleGossipHelloOpcode and the IsAnyTypeCreature() branch of
// HandleGossipSelectOptionOpcode. Free functions for the same reason as the
// gameobject pair -- critters are stock creature_template rows we do not own,
// so script_name is not ours and pGossipHello / pGossipSelect can never fire.
//
// Both answer false unless the player OWNS the animal, which is the same
// question the per-viewer UNIT_NPC_FLAG_GOSSIP override asks in
// Object::BuildValuesUpdate -- so a click can only reach a menu on something
// the player was already being shown as talkable.
bool HouseCritterGossipHello(Player* player, uint32 critterGuidLow);
bool HouseCritterGossipSelect(Player* player, uint32 critterGuidLow, uint32 sender,
                              uint32 action, char const* code);

// THE PER-ANIMAL NAME, answered into WorldSession::SendPetNameQuery. False for
// anything that is not a house critter, and for one that has never been named
// -- so an unnamed rabbit answers with Creature::GetName(), the species, and
// the client caches that per pet number exactly as it would a real name.
bool HouseCritterPetName(uint32 creatureGuidLow, std::string& name);

#define HOUSE_GO_GUID_MIN       8000000
#define HOUSE_GO_GUID_MAX       12000000

// Every placed object is a GameObjectData entry held for the life of the
// process, so this is a memory bound as much as a gameplay one.
#define HOUSE_MAX_OBJECTS       100

// CRITTERS -- rabbits, cats, turtles -- live in the house the same way
// furniture does. house_critter.id IS the creature low guid, and the block is
// laid out on the same reasoning as the gameobject one above:
//
//   1 .. 2,902,672     tw_world.creature, in use today
//   2,902,673 ..       .npc add, static, global and persisted
//   8,000,000 ..       house critters, this block
//   8,202,673 ..       temporary summons, per map instance
//   .. 16,777,215      ceiling (creature low guids are 24 bits too --
//                      ObjectGuid::HasEntry(HIGHGUID_UNIT) is true)
//
// A SEPARATE GUID SPACE FROM THE FURNITURE, which is why this may share the
// 8,000,000 base with no ambiguity: HIGHGUID_UNIT and HIGHGUID_GAMEOBJECT are
// different high parts, so guid 8,000,001 as a creature and as a gameobject
// are different objects and always were.
//
// That layout REQUIRES GuidReserveSize.Creature = 5300000 in mangosd.conf; it
// ships at 1000, which would put the temporary floor at 2,903,673 -- far below
// us. LoadCritters refuses to load anything if the setting has not been
// raised, for exactly the reason the gameobject check exists: a collision here
// shows up as a critter vanishing at random, days later.
#define HOUSE_CRITTER_GUID_MIN  8000000
#define HOUSE_CRITTER_GUID_MAX  8100000

// Ten per house, agreed 2026-09-04. A critter is an AI tick and a grid update,
// not just a row, so this is a lower ceiling than the furniture's 100 on
// purpose -- and ten animals in one room is already a menagerie.
#define HOUSE_MAX_CRITTERS      10

// How far a critter may roam from where it was put. Small on purpose: map 28's
// navmesh describes the flat plain and knows NOTHING about a spawned WMO, so a
// critter given real range will happily walk out through an inn wall. Three
// yards keeps it pottering about where you put it.
#define HOUSE_CRITTER_WANDER    3.0f
#define HOUSE_CRITTER_WANDER_MAX 20.0f

// How close you have to stand for a bare `.house critter ...` to find one.
#define HOUSE_CRITTER_REACH     10.0f

// "Come here" WALKS. How long it is given to get there before it is simply put
// there, in HOUSE_EDIT_INTERVAL ticks -- 40 x 500ms = 20 seconds.
//
// THE BACKSTOP IS NOT PARANOIA. Map 28's navmesh describes the flat plain and
// knows nothing about a spawned building, so a path computed across a room can
// run straight through a wall the animal will then collide with. Usually it
// walks over charmingly; when it cannot, the order still has to be obeyed.
#define HOUSE_CRITTER_WALK_TICKS  40

// Near enough to call it arrived.
#define HOUSE_CRITTER_WALK_ARRIVED 1.5f

// The MotionMaster point id. Nothing reads it back -- there is no
// MovementInform hook available for a stock creature template -- but it has to
// be something.
#define HOUSE_CRITTER_WALK_ID     1

// Long enough for anything anybody types at a rabbit, short enough that a name
// cannot crowd out the rest of a chat line. Matches house_critter.custom_name.
#define HOUSE_CRITTER_NAME_MAX    24

// THE HEARTS, found rather than guessed. Reading Spell.dbc and reasoning about
// which field matters has failed three times in this repo, so the search went
// the other way: SpellVisualEffectName.dbc carries model paths with real names,
// and exactly one says what we want --
//
//   effectName 3016  "Holiday - Valentine - Heart State/Impact"
//                    spells\holidays\valentines_lookingforloveheart.mdx
//
// SMSG_PLAY_SPELL_VISUAL CARRIES A **SpellVisualKit** ID, NOT A SpellVisual ID.
// The comment on Unit::SendPlaySpellVisual says so outright and it is the whole
// difference between hearts and nothing at all: the chain runs effectName ->
// SpellVisualKit -> SpellVisual, and this packet stops at the MIDDLE table.
// Tracing all the way to SpellVisual and passing that shipped once, and did
// exactly nothing in game.
//
// Six kits use effect 3016. 6512, 6517, 6550 and 6612 are referenced from
// IMPACT slots -- the one-shot burst a pat wants -- while 6549 and 6552 are
// referenced from STATE slots, which is the persistent-aura shape.
// `.house critter visual <n>` compares them live, because which one looks right
// is still the client's business.
#define HOUSE_CRITTER_HEART_VISUAL  6512

// A pat is free but not infinitely free -- this stops a held-down macro turning
// into a packet storm for everybody in the room.
#define HOUSE_CRITTER_PAT_COOLDOWN  3

// The gear beside the way out, carrying the house's control panel. Same model
// and size as the entrance gear (100013) on purpose: two gears doing the same
// job should look like it.
#define HOUSE_CONTROL_ENTRY     100014

// The icon beside every option in the house menus. A small dot rather than the
// white chat bubble the client defaults to, because none of these is
// conversation: the handle, the control panel and the portal are all buttons a
// player presses, and a speech bubble on "Grow 50%" claims somebody is talking.
//
// GOSSIP_ICON_DOT is 10, and the macro is deliberately not resolved here --
// HouseMgr.h does not include GossipDef.h and should not start, since a macro
// expands at the use site and every file that draws a house menu includes it
// already.
//
// Changing this one line changes all three menus, which is the only reason it
// is a constant: it was sixty-odd literal GOSSIP_ICON_CHATs before, and the way
// you find out you missed one is a player noticing a single odd row.
#define HOUSE_GOSSIP_ICON       GOSSIP_ICON_DOT

// A BACKSTOP, NOT A SHELF SIZE, and the difference is the whole point of this
// number. It was 40 -- ten squares across and four down, a window you could
// fill -- and once the window became a list nothing about forty was visible or
// meaningful any more: a player saw "12 / 40" and a refusal, for a limit that
// existed because a grid has to end somewhere.
//
// So there is no shelf size now. This is the ceiling a runaway loop or a
// crafted packet hits, high enough that ordinary play never reaches it and low
// enough that one account cannot write unbounded rows. Raising it costs
// nothing; it is not a design decision, it is a fuse.
//
// THE SLOT COLUMN STAYS, and stays for a reason that survived the grid: it is
// the IDENTITY `.house storage take` acts on. Squares are no longer drawn and
// no longer bounded at forty, but a crate still has a number, so no schema
// changed when the limit went and `.house storage move` still works.
#define HOUSE_STORAGE_MAX       1000

// How far in front of the player a new object lands. Dropping it exactly at
// their feet means standing inside it, which is the worst possible angle from
// which to judge whether you want it there.
//
// TWO YARDS, not the three it was until 2026-09-03. Three put a chair beyond
// arm's reach, which reads as thrown rather than set down, and every placement
// then began with a nudge back towards you. `.house object add <entry> [yd]`
// still takes an explicit distance for anything that wants more.
#define HOUSE_PLACE_DISTANCE    2.0f
#define HOUSE_PLACE_DISTANCE_MAX 40.0f
// How far above or below you a clicked point may be. Only the reticle route
// can trip either of these -- the spell the crates carry reaches 25 yards, so
// an honest client refuses first. They exist so a crafted packet cannot
// furnish the far side of the map or hang a bookshelf in the sky.
#define HOUSE_PLACE_HEIGHT_MAX  20.0f
// A HAIR ABOVE WHATEVER YOU ARE STANDING ON, NEVER EXACTLY ON IT.
//
// A flat model -- a rug, a mat, a fallen banner -- placed at exactly the floor
// height is COPLANAR with the floor, and two surfaces in the same plane fight
// over the depth buffer: the rug flickers, or the floor shows through it in
// bands that swim as you walk. It reads as a broken model rather than a
// placement a nudge could fix, and no amount of getting the height "right"
// helps, because being right is the problem.
//
// So every route that decides a height adds this afterwards. It is applied
// ONCE per placement, from the player's own height or from the clicked point --
// never from the object's current z -- so picking a piece up and putting it
// down again does not walk it up the wall.
//
// TUNED DOWNWARDS, AND THE DIRECTION OF THE SEARCH IS THE POINT. Too much is a
// rug you can see daylight under, which is visible immediately and from one
// angle; too little is a flicker that comes back at distance, on one floor
// texture, on somebody else's machine -- so it is found by starting from a
// value that provably clears the fight and coming down until it does not,
// never by starting low and raising it when somebody complains. 0.05 yards
// (~4.5cm) worked; this is half of it, about two centimetres.
#define HOUSE_PLACE_LIFT        0.025f

// The most one `.house object move` may shift a piece, per axis, in yards.
// A room is a few dozen; a typo is thousands, and one of those put an object
// at z = -800046, which Relocate asserts on at the next login (2026-09-04).
#define HOUSE_MAX_NUDGE         100.0f

// The most pieces one furniture group may hold. Every member of a group is
// taken out of the map and put back on each click, so this keeps one click
// cheap. A table, its chairs and the things on it fit with room to spare.
#define HOUSE_GROUP_MAX         40

// The entrance. sql/custom/027 -- 100011 is the swirl out in the world, 100012
// the one inside the house that takes you back to it. See HousePortal.cpp for
// why this is a gameobject that polls rather than an areatrigger.
#define HOUSE_PORTAL_ENTRY      100011
#define HOUSE_EXIT_ENTRY        100012

// How close counts as walking through. Deliberately small: the portal model is
// a couple of yards across, so this is "standing in it", not "near it".
#define HOUSE_PORTAL_RANGE      2.0f

// How far ahead of you .house portal drops it, and how far in front of it you
// come back out. Both have to exceed HOUSE_PORTAL_RANGE or arriving re-triggers
// the portal you just arrived through.
#define HOUSE_PORTAL_PLACE_DISTANCE 3.0f

// What "the entrance you are standing at" means for the bare form of the
// entrance commands. Comfortably more than HOUSE_PORTAL_PLACE_DISTANCE, so the
// spot you placed a door from is in range of it, and more than
// HOUSE_PORTAL_RANGE, so you can address your own home door without walking
// into it. The id form stays unranged -- reaching a door on another map is
// exactly what it is for.
#define HOUSE_PORTAL_NEAR       5.0f

// Same idea inside the house: the exit portal stands this far behind the
// arrival point, so you land facing away from it.
#define HOUSE_EXIT_OFFSET       4.0f

// ARRIVE ABOVE THE FLOOR, NOT IN IT. A teleport lands the player before the
// destination has finished loading around them -- terrain, and any building
// they placed -- so arriving at exactly floor height drops them through it and
// they fall until the ground catches up. Coming in a couple of yards high means
// the worst case is a short drop onto the floor instead of a trip under it.
#define HOUSE_ARRIVE_LIFT       2.0f

// A DOOR ARRIVAL CAN STILL FALL THROUGH (2026-10-08, Goldshire Inn, floor 9.26,
// once in many entries). The lift and the settle do not catch a client that
// falls before the building is solid. A full fall ends on map 28's ground plane
// at z = 0, so the rescue looks for a player ON THE PLANE, not under the floor:
// a cellar is above the plane and is never mistaken for a fall. For WATCH ms
// after the settle, a player within PLANE_Z of the plane and RADIUS of the
// arrival point goes back to the arrival point. A floor under MIN_FLOOR stands
// on the plane and has nothing to fall through. RADIUS keeps a player who
// walked out of the building onto the plane from being pulled back.
#define HOUSE_FALL_WATCH        10000
#define HOUSE_FALL_PLANE_Z      1.0f
#define HOUSE_FALL_MIN_FLOOR    2.0f
#define HOUSE_FALL_RADIUS       10.0f

// THE CLICKABLE HEART OF A PORTAL. The swirl itself cannot be moused over:
// Creature_Spellportal_Purple.m2 carries no hit volume, so the client never
// sends CMSG_GAMEOBJ_USE for it. That is a property of the ART and is not
// derivable from anything on disk -- vmaps/temp_gameobject_models lists 21585
// as HAVING a collision model, and server collision has nothing to do with
// client mouse picking. Like which WMOs render their portals, it is a tested
// whitelist. So a small gear stands in the middle of each portal and carries
// the menu, exactly as the edit handles do.
// HOW CLOSE YOU STAND FOR A GEAR TO APPEAR, and how far you walk for it to go
// again. Two numbers rather than one, and the gap is the point: the poll runs
// every HOUSE_PORTAL_INTERVAL, so a single threshold would summon and despawn
// the gear several times a second for anybody standing on the line. A
// gameobject appearing is a fresh creation block to every client in range, so
// that flicker is not just ugly, it is chatter.
//
// SHOW is deliberately larger than HOUSE_PORTAL_RANGE: the gear has to be
// reachable BEFORE you are close enough to be taken through the door, or the
// only way to open its menu would be to stand in the spot that teleports you.
// That leaves a usable band of one yard, which is enough -- but it is also why
// the proximity test measures to the portal ON THE FLOOR and never to the gear
// floating HOUSE_PORTAL_MARK_LIFT above it. Two yards of lift would eat the
// band whole. See UpdateExitMarker, where exactly that happened.
// STARTING VALUES ONLY -- `.house entrance handle range <show> [hide]` tunes
// both live, because what reads as "walking up to it" is a matter of taste and
// costs a full build to guess at. Not saved, like every other knob on that
// command: a number somebody liked in play gets typed in here afterwards.
//
// 5 and 7, up from 3 and 4, which left one usable yard once HOUSE_PORTAL_RANGE
// had taken the first two.
#define HOUSE_MARK_SHOW_RANGE   5.0f
#define HOUSE_MARK_HIDE_RANGE   7.0f

#define HOUSE_PORTAL_MARK_ENTRY 100013
#define HOUSE_PORTAL_MARK_LIFT  2.0f

// Proximity is polled, because a gameobject gets no "player entered my radius"
// callback. 250ms is under one step at run speed.
#define HOUSE_PORTAL_INTERVAL   250

// A THOUSAND doors, taken off the TOP of the furniture block. Furniture
// allocates UPWARD from HOUSE_GO_GUID_MIN, so the two grow towards each other
// and would need nearly four million placed objects to meet.
//
// A portal's guid is HOUSE_PORTAL_GUID_MIN + its row id, which makes the id an
// ADDRESS and not merely a key: reuse one and a new door appears where an old
// one stood. house_portal must therefore never be renumbered -- see
// sql/custom/032.
//
// The `u` is not decoration. ObjectGuid declares a private
// ObjectGuid(HighGuid, uint32, uint64) purely to catch wrong-typed counters, so
// a plain int argument is ambiguous against the real uint32 constructor and
// fails to compile with a thoroughly misleading "cannot convert from
// initializer list".
#define HOUSE_PORTAL_GUID_MIN   (HOUSE_GO_GUID_MAX - 1000u)
#define HOUSE_PORTAL_GUID_MAX   (HOUSE_GO_GUID_MAX - 1u)
#define HOUSE_PORTAL_MAX_ID     999u

// EDIT MODE ONCE SUMMONED A GEAR ON EVERY OBJECT, and gameobject 100010 was
// that gear -- a small hovering handle you clicked, because a chair could not
// be clicked directly. The furniture carries its own menu now (see
// Object::BuildValuesUpdate, which tells the editing player alone that it is a
// GOOBER), so nothing summons one and the constants that shaped it are gone.
//
// The row survives in sql/custom/022-025, unreferenced and harmless. Two of
// its findings are worth keeping even though the object is not: a handle could
// never be the usual invisible stalker, because the whole point was clicking
// it; and it had to be a GAMEOBJECT rather than a creature, because a creature
// model carries an ambient loop through CreatureModelData -> CreatureSoundData
// that cannot be silenced from this side -- the client reads its own copy out
// of the MPQ. That is why the selection glow below is an ENCHANTMENT effect
// and not a fire or a portal, which would hum at the owner forever.

// The glow that marks the selected object. `SPELLS ENCHANTMENTS
// WhiteGlow_High.mdx` -- a STOCK Turtle template, so this needs no SQL of its
// own; it is only ever summoned, never edited, so a world update cannot take it
// away the way it would revert an edited row.
//
// An ENCHANTMENT glow specifically, and that is the reason it can be used at
// all. SummonHandle's comment records why the gear had to be solid geometry:
// effect models animate and carry their own sound emitters inside the .mdx,
// which no DBC edit can reach, so a fire or a portal effect would hum at the
// owner forever. The enchant shimmer is the quiet corner of that shelf.
// The two spells a furniture crate can carry, and the only difference that
// matters is Targets: 0x40 (TARGET_FLAG_DEST_LOCATION) makes the client raise
// its ground reticle and send back the point, 0x0 makes it a plain use.
// Measured against server/dbc/Spell.dbc 2026-09-03; both read Stances 0,
// powerType 0, SpellFamilyName 0, so neither is gated by class or form.
//
// 261 is the one sql/custom/042 settled on after three attempts -- see 046 for
// the two that failed and why reading the DBC harder was not the way to pick
// one. 482 is what the crates carried before that work, so it is proven rather
// than guessed: it shipped, and placement simply went ahead of the player.
// AND THE AHEAD ONE ALSO HAS TO CAST. With nothing to aim at, the moment
// between deciding and getting is empty, so 33453 "Over-Tinkered Lens" is
// there for its one-second bar and its crafting animation -- SpellVisual 215,
// which 24 real recipes use, so it is an animation the client already draws
// for making something rather than a guess at one.
//
// Chosen off the profile that settled 261, not off its description: Targets 0,
// Stances 0, StancesNot 0, powerType 0, manaCost 0, SpellFamilyName 0,
// Attributes 0x0, no reagent, no totem, no cooldown, effect DUMMY only, and a
// blank tooltip so the item's green Use line stays empty. That is Rough
// Dynamite's profile exactly, which is the bar sql/custom/046 sets.
//
// WHICH ANIMATION IT ACTUALLY DRAWS IS A QUESTION FOR THE CLIENT, and
// sql/custom/053 is the probe for it -- throwaway item entries, one per
// candidate, because a NEW entry is not in the client's WDB cache and can be
// compared with .additem in one sitting. Swapping this constant is the whole
// change once one wins.
// The spells the crates and the chalk carry live in sql/custom/042+055 and 054
// and are not named here any more. There were two constants for them while the
// server chose between them at runtime; nothing chooses now -- both are written
// once by SQL. A constant no code reads is a comment that can go stale without
// anybody noticing, which is the whole reason they went.
//
// For orientation only, and CHECK THE SQL BEFORE TRUSTING THESE: a crate
// carries 33453 (no reticle, 1s cast, crafting animation) and the chalk carries
// 13487 (reticle, no cast, no animation). This comment said 482 and 261 until
// 2026-09-03 -- both wrong, and describing the build where the crate's cast was
// removed along with its reticle, which sql/custom/055 reverted. Two decisions,
// only one of them taken.

// Where a server-wide setting lives in house_setting. Real account ids count
// up from 1, so 0 can never be a player's row.
#define HOUSE_SETTING_GLOBAL_ACCOUNT 0

#define HOUSE_SELECT_MARK_ENTRY 2000836

// Twice the size and a yard up, settled by trying it in game on 2026-09-02.
// The template's own size is 1 and its origin is at the model's foot, which put
// a small shimmer inside the furniture rather than around it.
//
// The SCALE is applied to the cached template at load rather than written into
// SQL, for the reason SetHandleLook exists: gameobject_template has no reload,
// and entry 2000836 is a stock Turtle row -- editing it in the world DB would
// be reverted by the next update and would change the model everywhere else it
// is used, which is not ours to do.
// A summon needs a lifetime. Belt and braces only: the glow is taken down when
// the selection is cleared, when edit mode ends and when the player leaves the
// map, so this is what catches a summon that somehow outlived all three.
#define HOUSE_SELECT_MARK_LIFETIME_SEC (30 * MINUTE)

#define HOUSE_SELECT_MARK_SCALE 2.0f
#define HOUSE_SELECT_MARK_LIFT  1.0f

// THE CHALK MARK USES THE SAME MODEL AS THE SELECTION GLOW, standing on the
// ground rather than a yard up. Same entry deliberately: one glow means "this
// is the spot housing is aiming at", and the two can never be confused in
// practice because one of them is wearing a piece of furniture and the other is
// standing on bare floor. Sharing the entry also means `.house object marker`
// tunes both at once, which is right -- they are one visual idea.
//
// Lift 0 and not m_selectLift: a mark is where the object will LAND, so
// floating it a yard up would be describing a different point from the one it
// means.
#define HOUSE_CHALK_MARK_LIFT   0.0f

// The item that sets one. sql/custom/054; entry block 100200-100299 is housing
// TOOLS, kept clear of the 100100-100199 furniture block that 042 deletes
// wholesale on every re-run.
#define HOUSE_CHALK_ITEM_ENTRY  100200

// How often each editing player's session is reconciled, from the instance
// script. It decides which objects the client has been told are clickable, and
// picks up anything placed since the last pass.
#define HOUSE_EDIT_INTERVAL     500

// Far enough out that no reset ever fires, near enough to stay inside time_t
// on every platform. See the header comment.
#define HOUSE_RESET_TIME        (time_t(4102444800))    // 2100-01-01

// A HOUSING TEMPLATE'S AUTHORING SPACE IS A HOUSE, owned by a synthetic
// account counting DOWN from the top of uint32 -- which satisfies house's
// UNIQUE(account, map), reuses every furniture path unchanged, and can never
// collide with tw_logon (real ids count up from 1 and are nowhere near).
// Nothing player-reachable resolves to these: VisitHouse looks accounts up by
// character name, and no character exists on them.
#define HOUSE_TEMPLATE_ACCOUNT_MAX  (0xFFFFFFFEu)
#define HOUSE_TEMPLATE_ACCOUNT_MIN  (0xFFFF0000u)

// Who put a piece of furniture in a house. Template-stamped rows are the
// house's fabric rather than the player's property: they cannot be deleted or
// cleared by the player, and they do not count against HOUSE_MAX_OBJECTS.
enum HouseObjectSource
{
    HOUSE_SOURCE_PLAYER   = 0,
    HOUSE_SOURCE_TEMPLATE = 1,
};

// WHAT A PLAYER HAS DECIDED FOR THEMSELVES. Both of these are choices with no
// right answer -- one person wants to aim a crate at the floor, another wants
// it in front of them and nudged; one wants the selected piece shimmering,
// another finds it in the way of seeing what they are arranging. Neither is a
// setting the server has an opinion about, which is exactly what makes it a
// setting rather than a decision.
//
// KEYED BY ACCOUNT, like the house and the storage shelf, so an alt walks into
// the same room set up the same way -- EXCEPT where the thing being set is not
// the server's to vary per player, which is a property of the setting and is
// recorded on it. A global one is stored under account 0 (real account ids
// start at 1, so the row can never collide) and only SEC_DEVELOPER may change
// it, because one player's preference would otherwise be everybody's.
//
// EVERY VALUE IS A BOOLEAN AND 1 IS ALWAYS THE OLD BEHAVIOUR. That is worth
// keeping to as more arrive: an absent row means the default, the default is
// what housing did before the setting existed, and so a table that has never
// been written to is indistinguishable from the server as it shipped.
// IDS ARE PERMANENT. A retired setting keeps its number for ever and the next
// one takes a fresh number -- ids are the primary key of house_setting rows
// that are already written, so renumbering does not remove a setting, it
// REINTERPRETS everybody's saved rows as the setting that moved into the
// vacated slot. Measured on this server before slot 0 was retired: one account
// held both placing=0 and glow=0, so sliding glow down to 0 would have read the
// dead placing row as a glow preference and dropped the real one.
enum HouseSetting
{
    // RETIRED 2026-09-03 -- was `point_and_click_placing`, and the slot stays
    // burnt rather than being reused. It made a crate raise the client's ground
    // reticle by rewriting spellid_1 on every furniture item; the Decorator's
    // Chalk does the aiming for everything now, so a second mechanism that
    // aimed only the twelve crates was one too many. The full design, the spell
    // profiling that settled it, and what to put back is in
    // docs/notes/point-and-click-placing.md.
    HOUSE_SETTING_RETIRED_PLACING = 0,

    // 1 = the selected object wears the shimmer; 0 = it does not. Turning it
    // off costs nothing but the glow: selecting still blinks the object, and
    // every command still names what it acted on.
    //
    // NOT the chalk mark's glow, which is deliberately never gated on this --
    // there the glow is the only way to see the mark at all, where here it is a
    // convenience on top of feedback that survives without it.
    HOUSE_SETTING_GLOW    = 1,

    // 1 = walking into a portal takes you through it; 0 = it does not, and the
    // gear beside it is the only way in or out.
    //
    // WHY THIS IS SAFE TO TURN OFF: the entrance gear has always offered "Go
    // home", and the exit gear grew a "Leave" the same day this setting did.
    // Without that second half the setting would be a way to lock yourself in
    // your own house, so the two shipped together and the Leave option is
    // drawn unconditionally rather than only while this is off -- a way out
    // that appears only in the state you cannot leave from is one conditional
    // away from being no way out at all.
    HOUSE_SETTING_WALK_IN = 2,

    // How long after an arrival the ping fires, in MILLISECONDS. See
    // NudgeOccupants for what the ping is for and how 3000 was measured.
    //
    // THE FIRST SETTING HERE THAT IS NOT ON OR OFF, and the first that is the
    // SERVER'S rather than a player's. Both are deliberate and they are the
    // same reason: this is a load race, so the right value differs by machine
    // and by client, which is exactly what an operator must be able to set once
    // and have stick. It is not a preference and no player should ever see it.
    //
    // The boolean vocabulary the other settings share was a rule about naming
    // PLAYER settings well -- "the name has to say what ON means" -- and it was
    // over-applied to this one for about ten minutes. Nothing in the storage
    // ever cared: `house_setting.value` has always been INT UNSIGNED, and
    // Get/SetSetting have always dealt in uint32.
    HOUSE_SETTING_ARRIVE_PING = 3,

    // 1 = the front door is locked: a party member who tries to come in
    // KNOCKS instead, and the owner answers with a popup. 0 = the party walks
    // straight in, which is how every house behaved before this existed.
    //
    // THE FIRST SETTING THAT DEFAULTS TO 0. "Every default is 1" was only ever
    // true because 1 was what housing did before each setting existed; the
    // same reasoning gives 0 here, so a house nobody has touched is unlocked.
    //
    // IT GATES THE DOOR, NOT THE ROOM. Guests already inside when it is locked
    // stay, and the party is still the whole permission: the lock only adds
    // the owner's yes to it. So EvictLapsedVisitors is unchanged.
    HOUSE_SETTING_LOCKED = 4,

    HOUSE_SETTING_MAX
};

struct HouseObject
{
    uint32 guid;                                        // == house_object.id
    uint32 houseId;

    // THE NUMBER A PLAYER TYPES. `guid` is allocated from one global block
    // starting at 8,000,000, which is right for the core and hostile to type;
    // `slot` is the same object counted from 1 within its own house. Every
    // command and every message uses it, and the guid is never printed --
    // though it is still accepted as input, because anything >= HOUSE_GO_GUID_MIN
    // cannot be a slot and so the two can share one argument with no ambiguity.
    //
    // 1-BASED, NOT 0-BASED, and that is forced rather than chosen:
    // ExtractOptUInt32 uses 0 for "no id given", so a slot 0 would be an object
    // no command could ever name.
    uint32 slot = 0;

    // THE FURNITURE GROUP, or 0. Numbered per house like `slot` (sql/custom/127).
    // A group is LIVE only while two or more loaded, non-fabric pieces of one
    // house share the number, and that is checked on every read by
    // HouseMgr::GroupMembers. A piece left alone after the others were picked
    // up keeps its number and acts ungrouped, so no path has to clean up.
    uint32 groupNo = 0;
    uint32 goEntry;

    // The furniture item this was placed from, or 0. Picking a row up with an
    // item entry hands that item back instead of destroying the object.
    //
    // The gameobject entry cannot answer this on its own: house_furniture_item
    // maps item -> gameobject, and nothing stops two items pointing at one
    // model (a quest reward and the vendor's copy of the same chair). So the
    // row remembers where it came from rather than the mapping being asked to
    // run backwards.
    uint32 itemEntry = 0;
    float  x, y, z, o;
    float  rot0, rot1, rot2, rot3;
    float  scale;                                       // 0 = the template's own size
    uint8  source = HOUSE_SOURCE_PLAYER;                // HouseObjectSource
};

// A critter living in a house. Deliberately the same shape as HouseObject, one
// field at a time, because everything it does is the furniture path with
// AddCreatureToGrid where AddGameobjectToGrid stood.
//
// NO `source`, and that absence is real rather than unfinished: a template
// cannot stamp critters, because nothing freezes them.
//
// `itemEntry` arrived with the crates (sql/custom/058) and means exactly what
// HouseObject::itemEntry means -- which crate this came out of, or 0 for
// `.house critter add`. The creature entry cannot answer it, because two
// crates are allowed to release the same animal.
struct HouseCritter
{
    uint32 guid;                                        // == house_critter.id
    uint32 houseId;

    // The number a player types, 1-based within its own house -- the same
    // bargain slots make for furniture, and forced 1-based for the same
    // reason: ExtractOptUInt32 uses 0 for "no id given".
    uint32 slot = 0;
    uint32 entry;                                       // creature_template.entry
    uint32 itemEntry = 0;                               // the crate, or 0

    // What the owner called it, or empty for the species name. Reaches the
    // client through the PET NAME query, which is keyed per instance -- see
    // HouseMgr::DressCritter and sql/custom/061.
    std::string customName;
    float  x, y, z, o;

    // Yards it may roam. 0 puts it on IDLE_MOTION_TYPE, which is a critter
    // standing where you left it playing its own idle animations -- not a
    // frozen one.
    float  wander = HOUSE_CRITTER_WANDER;
};

// "That came with the house." A template-stamped row is the BUILDING, not the
// furniture: the player may not move, turn, drag, resize, delete or put a gear
// on it. One predicate rather than five copies of a comparison, so the gates
// cannot drift apart -- the delete gate shipped first and the other four were
// added once it turned out you could still slide the walls around.
//
// No developer bypass, deliberately. source = TEMPLATE rows exist only in
// CLAIMED houses; authoring places ordinary player rows inside the template's
// own house, so a rank check here would have no legitimate use. Template
// furniture is changed in the template and re-stamped.
inline bool IsHouseFabric(HouseObject const& o) { return o.source == HOUSE_SOURCE_TEMPLATE; }

// One row of tw_world.house_furniture_item: the model an inventory item turns
// into, and the size to place it at (0 = whatever the model ships at).
struct FurnitureItem
{
    uint32      goEntry = 0;
    float       scale = 0.0f;

    // The tab this crate is sold under, and the same string the addon's
    // storage shelf filters by. Blank means untabbed: still sold, still
    // storable, simply not on a category page -- which is what an install
    // that has not applied sql/custom/062 degrades to.
    //
    // THE CORE KNOWS NONE OF THESE NAMES and must not learn them. It groups by
    // whatever it finds; the rule that produces the values lives in
    // tools/categorise.js, which is also where the catalogue browser and the
    // shelf get theirs. A copy of the word list here would drift the first
    // time a bucket was renamed -- the same reason HousePrintStorage groups by
    // item without categorising.
    std::string category;
};

// A crate that lets an animal out. The critter half of FurnitureItem, and
// deliberately a SEPARATE table rather than a `kind` column on that one: the
// payload differs (a creature entry, not a gameobject entry) and so does the
// second field (a roaming radius, not a scale), so one table would be two
// nullable columns and a discriminator to get wrong.
struct CritterItem
{
    uint32 critterEntry = 0;
    float  wander = HOUSE_CRITTER_WANDER;
};

// An animal on its way to where you called it. Memory-only: the row is already
// written to the destination when the walk starts, so a restart mid-walk simply
// spawns it there and nothing is left half-done.
struct HouseCritterWalk
{
    uint32 instanceId = 0;
    uint32 entry = 0;
    float  x = 0.0f, y = 0.0f, z = 0.0f;
    float  wander = 0.0f;
    uint32 ticks = 0;                       // counts down to the backstop
};

// One player's edit-mode session. `handles` is what turns a click on a gear
// back into the object it marks. It is keyed by the editing player, so
// somebody else clicking your handle simply finds nothing -- the ownership
// check falls out of the lookup rather than being a separate test.
// THREE STEPS, NOT ONE, because these are three different units and a single
// number cannot be all of them. Setting a fine 0.25 to seat a chair at a table
// and then finding the turn buttons had become a quarter of a degree is the
// failure a shared step invites.
//
// Each page cycles its own, and the defaults are exactly what the buttons used
// to be hardcoded to -- so a session that never touches a step behaves as the
// menu always did.
enum HouseStepKind
{
    HOUSE_STEP_MOVE = 0,                                // yards
    HOUSE_STEP_TURN = 1,                                // degrees
    HOUSE_STEP_SIZE = 2,                                // percent
};

struct HouseEdit
{
    uint32 houseId = 0;

    float  stepMove = 1.0f;                             // yards
    float  stepTurn = 15.0f;                            // degrees
    float  stepSize = 10.0f;                            // percent

    // `wanted` is what this session would show if you stood next to it;
    // `handles` is what is actually summoned right now. Keeping them apart is
    // what lets a gear come and go with the player without the session
    // forgetting the object it belongs to.
    bool   all = false;                                 // .house edit on, rather than one just placed
    std::set<uint32> wanted;                            // object guids

    // What the CLIENT currently believes is a GOOBER, which is what makes an
    // object hoverable and clickable. It lags `wanted` by one Reconcile pass,
    // because telling the client costs a despawn and a respawn of the object --
    // so the difference between the two sets is exactly the work to do.
    std::set<uint32> marked;
};

// TWO EDIT MODES, AND THEY ARE NOT THE SAME THING WITH DIFFERENT SCOPE.
//
//  - GLOBAL (`all` set, from `.house edit on`): every object you own in this
//    house is editable, and stays editable as the house changes -- Reconcile
//    rebuilds `wanted` from the house on every pass. Nothing is taken out of it
//    one object at a time; the mode owns the whole room and ends as a whole.
//
//  - INDIVIDUAL (`all` clear): the session holds exactly the objects put into
//    it, which is what `.house object edit <n>` toggles and what PlaceObject
//    opens so a piece you just put down can be clicked straight away. Its Done
//    takes that one object back out.
//
// A GLOBAL SESSION WITH A HOLE IN IT WAS BUILT AND REMOVED, 2026-09-03. An
// `excluded` set let Done drop one object while the mode ran, and it produced a
// state nothing else in housing has: one dark chair in a lit room, needing a
// chat line to explain itself. The message was the tell. Under global edit Done
// closes the window and that is all -- the room-wide exit is its own row.

// One world-side entrance: the doorway of an inn, a house in a village, a spot
// somebody stood in and decided was a door. Every one of them leads each player
// to THEIR OWN house, so a portal is shared furniture of the world rather than
// anybody's property -- which is why this carries no account.
// A pile of one kind of crate on the furniture shelf.
//
// GROUPED AT THE SOURCE, not at each caller. The window draws one row per kind,
// the chat list prints one line per kind, and the sync sends one record per
// kind -- three callers that all wanted the same collapse, and three places to
// get it subtly different if each did its own. `slot` is the lowest square
// holding one, so a row can still be handed to `.house storage take`.
// One armed arrival, waiting out the ping, then HOUSE_FALL_WATCH. Memory-only
// and erased when the watch ends.
//
// It carries the FLOOR because the floor cannot be asked for later:
// map->GetHeight reads the raw .map surface and map 28 is one plane at z = 0,
// so it answers 0 from any height and every WMO floor is invisible to it.
// GetArrivalPosition knew the real value; this is where it is written down.
struct HouseArrival
{
    uint32 instanceId = 0;
    uint32 ticks      = 0;
    float  x          = 0.0f;
    float  y          = 0.0f;
    float  floorZ     = 0.0f;
    uint32 watchTicks = 0;      // HOUSE_FALL_WATCH, counted after the settle
};

struct HouseStorageStack
{
    uint32 slot = 0;                                    // lowest square holding it
    uint32 itemEntry = 0;
    uint32 count = 0;
};

struct HousePortal
{
    uint32 id = 0;
    uint32 map = 0;
    float  x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
    std::string name;

    // Which inside this door leads to. 0 = an untemplated door: claiming
    // there gives an empty house, and the old non-destructive re-home stays
    // available on it -- the pre-template world, preserved per door.
    uint32 templateId = 0;
};

// A named inside a player's house starts as: the shell, the furniture, and the
// exit spot. `objects` is the FROZEN snapshot written by .house template save
// -- what stamping copies -- not the live contents of the authoring house.
// The guid field of a frozen HouseObject is meaningless (stamping allocates
// fresh ones); houseId is 0.
struct HouseTemplate
{
    uint32 id = 0;
    std::string name;
    uint32 houseId = 0;                                 // the authoring house
    bool   exitSet = false;
    float  exitX = 0.0f, exitY = 0.0f, exitZ = 0.0f, exitO = 0.0f;
    time_t savedAt = 0;                                 // 0 = never saved
    std::vector<HouseObject> objects;                   // frozen stamp source
};

struct House
{
    uint32 id = 0;
    uint32 accountId = 0;

    // Which entrance this house calls home. 0 = none chosen, and that is a
    // supported state: the way out falls back to the hearthstone, exactly as an
    // unplaced portal always did.
    uint32 portalId = 0;
    uint32 instanceId = 0;                              // plumbing; house_object is keyed by id
    std::string name;
    std::vector<uint32> objects;                        // house_object ids
    std::vector<uint32> critters;                       // house_critter ids

    // Which template this house was STAMPED from, purely historical -- editing
    // or deleting the template never reaches back into a stamped house. 0 =
    // bespoke: pre-template, or claimed at an untemplated door.
    uint32 templateId = 0;

    // Nonzero iff this house IS a template's authoring space (in-memory,
    // derived from house_template.house_id at load). The developer-edit bypass
    // and the template commands key off it.
    uint32 authorsTemplate = 0;

    // Where the way OUT stands, when a template author has chosen one -- it is
    // stamped in with the furniture, never set by the resident. Unset means the
    // constant every house used before exits existed -- see sql/custom/031. An
    // explicit flag rather than testing for 0,0,0, because
    // "nobody has chosen" and "somebody chose the origin" are different states
    // and only one of them should fall back.
    bool   exitSet = false;
    float  exitX = 0.0f, exitY = 0.0f, exitZ = 0.0f, exitO = 0.0f;
};

// WHOSE HOUSE A DOOR LEADS TO FOR A PARTY MEMBER. Resolved fresh every time --
// never cached across a click -- because a group is the most volatile thing
// housing has ever keyed off: it can disband, promote somebody else, or drop
// the member between the menu being drawn and an option being chosen.
//
// `leaderGuid` is what makes the staleness check possible: the gear records the
// leader it OFFERED, and the selection refuses if the party has moved on. Going
// somewhere other than the door you read is the failure worth spending a field
// to prevent.
struct HousePartyHost
{
    ObjectGuid  leaderGuid;
    uint32      leaderAccount = 0;
    uint32      houseId = 0;
    std::string leaderName;
};

// ONE NUMBER FOR THREE THINGS: how long a knock can be answered, how long the
// addon's popup stays up, and the least time between two popups from one
// visitor. 30 s is short enough to wait out and long enough that the owner is
// not pestered; past that, the owner leads the party and can kick them.
#define HOUSE_KNOCK_SECONDS     30

// How long a player can go unseen by a portal's poll and still count as
// standing in its ring. The poll runs every HOUSE_PORTAL_INTERVAL (250 ms), so
// this allows for a missed poll or two without calling it a fresh step.
#define HOUSE_KNOCK_TOUCH_MS    1000

// A KNOCK ON A LOCKED DOOR. Memory only: a restart losing one costs a second
// knock.
//
// `portalId` is the door they knocked at, so an admitted guest leaves by it
// like any other visitor. 0 for `.house visit`, which has no door.
//
// `answered` keeps the row after Open or Deny, until it expires, so that a
// Deny is not followed by a fresh popup the moment they step back in.
struct HouseKnock
{
    uint32     houseId = 0;
    uint32     portalId = 0;
    time_t     expires = 0;
    bool       answered = false;
};

// A player standing in a portal's ring after a knock there. The knock happens
// once per step into the ring; standing still in it does nothing more until
// they walk out and back.
struct HouseKnockTouch
{
    uint32     portalGuid = 0;
    uint32     lastSeen = 0;                            // WorldTimer ms
};

class HouseMgr
{
    public:
        HouseMgr() : m_enabled(false), m_nextObjectGuid(HOUSE_GO_GUID_MIN),
                     m_templateDisplay(0), m_templateSize(0.0f),
                     m_portalDisplay(0), m_portalScale(0.0f),
                     m_markShow(HOUSE_MARK_SHOW_RANGE), m_markHide(HOUSE_MARK_HIDE_RANGE),
                     m_markOffX(0.0f), m_markOffY(0.0f), m_markOffZ(HOUSE_PORTAL_MARK_LIFT),
                     m_markTemplateSize(0.0f),
                     m_portalTemplateDisplay(0), m_portalTemplateSize(0.0f) {}

        void LoadFromDB();
        bool IsEnabled() const { return m_enabled; }

        House* GetHouseByAccount(uint32 accountId);
        House* GetHouseByInstance(uint32 instanceId);
        House* GetHouseById(uint32 id);

        // Bind the player to their account's house and teleport them in.
        // TELEPORT ONLY: since templates, this no longer creates -- a house is
        // claimed at an entrance (ClaimHouseAt), and a houseless account is
        // pointed there instead. One check, four callers covered: .house go,
        // visiting yourself, walking into a door, and the door gear.
        bool SendPlayerHome(Player* player, std::string& error);

        // Somebody has just finished arriving on the house map. Called from
        // DungeonMap::Add, self-guarding on the map id exactly like the
        // ClearEdit / ClearSelection / ClearMark trio in DungeonMap::Remove,
        // so Map.cpp keeps carrying one unconditional line per hook and none of
        // housing's rules.
        //
        // ARRIVAL, NOT TELEPORT, and the difference is why this hangs off the
        // map rather than off SendPlayerHome. There are five ways into a house
        // -- .house go, the door gear, walking through the portal, a party
        // following its leader, and a relog inside one -- and only the map sees
        // all five. Greeting from the teleport would have missed the relog,
        // which is the one arrival where a returning player most needs the
        // reminder.
        //
        // Safe to talk to the client here: Add runs from
        // HandleMoveWorldportAckOpcode, so the load screen is already down.
        void OnPlayerArrive(Player* player);

        // One synthetic MSG_MOVE_HEARTBEAT from everybody in the house, because
        // a player standing still INSIDE A SPAWNED BUILDING is not drawn to
        // somebody who has just walked in -- the client cannot resolve the
        // WMO's portals, so it never places the unit. See the long note at the
        // definition for the three measured arrivals that isolated it, and for
        // what it corrects about the building whitelist.
        void NudgeOccupants(Player* player);
        void RunPendingNudges(Map* map);
        void SettleArrival(Player* player, HouseArrival const& a);
        void RescueFall(Player* player, HouseArrival const& a);
        void BeatOccupants(Map* map);

        // THE ANIMALS NEED THE SAME PACKET, and for the same reason -- proven
        // in play 2026-09-04. A critter standing in a spawned building is not
        // drawn to somebody who walks in, and `.house critter drag` made it
        // appear instantly: drag goes through Unit::NearTeleportTo, which sends
        // MSG_MOVE_TELEPORT to observers. So the creature was always there and
        // the guest's client had discarded its create block, exactly as it
        // discards a player's.
        //
        // This is the consequence CLAUDE.md flagged as untested when the player
        // fix went in. It is the same fault, so it gets the same fix, on the
        // same already-measured delay.
        // Watch the animals that were called over: re-anchor the one that has
        // arrived, and put down the one that cannot get there. Rides the
        // instance script's existing throttle beside the arrival ping.
        void RunCritterWalks(Map* map);
        void StartCritterWalk(Creature* creature, HouseCritter const& c);

        void BeatCritters(Map* map);

        // Arm that beat for an instance with nobody arriving -- placing a
        // critter while a guest is already standing in the room. An arrival
        // beats the animals anyway, so this is only for the case where the
        // animal is what is new.
        void ArmCritterBeat(Map* map);

        // Templates -- a named inside, authored in a house owned by a
        // synthetic account so every normal command works there, frozen by
        // Save into house_template_object, stamped into real houses on claim.
        void LoadTemplates();
        HouseTemplate* GetTemplate(uint32 id);
        HouseTemplate* GetTemplateByName(std::string const& name);
        std::vector<HouseTemplate const*> GetTemplates() const;
        bool CreateTemplate(Player* dev, std::string const& name, std::string& error);
        bool SaveTemplate(Player* dev, uint32& savedObjects, bool& savedExit, std::string& error);
        bool GotoTemplate(Player* dev, uint32 id, std::string& error);
        bool DeleteTemplate(Player* dev, uint32 id, std::string& error);

        // Claiming and moving, driven by the door gear's menu. Claim creates
        // the house and stamps the door's template; Move purges everything --
        // template rows dropped, player rows deleted until the furniture chest
        // exists to stow them -- and re-stamps from the new door. Both check
        // combat BEFORE mutating anything.
        bool ClaimHouseAt(Player* player, uint32 portalId, std::string& error);
        bool MoveHouseTo(Player* player, uint32 portalId, std::string& error);

        // .house reset, standing inside. A claimed home goes back to what its
        // own door hands out today (re-stamped, or emptied at an untemplated
        // door); a template is emptied BUT KEEPS ITS EXIT -- the arrival spot
        // is the one thing worth keeping when starting a layout over, and the
        // frozen snapshot changes only on save.
        bool ResetHouse(Player* player, std::string& error);

        // "May this player rearrange this house?" The owner may; a developer
        // may inside a template's authoring house. BOTH ownership checks --
        // CheckOwner here and SetEditMode's own -- route through this, so the
        // two can never disagree.
        bool CanEditHouse(Player* player, House const& house) const;

        // The object cap counts only what the player placed; the template's
        // own furniture is the house's fabric and rides for free.
        uint32 CountPlayerObjects(House const& house) const;

        // Visiting. A visitor is put into the host's instance with
        // Player::SetForcedInstance rather than a bind of their own -- binds are
        // one per (character, map), so a visitor who owns a house cannot be
        // bound to somebody else's.
        //
        // THE PARTY IS THE WHOLE PERMISSION (2026-09-03). There was a guest
        // list beside it -- `house_guest`, three commands, a load, a cache --
        // and two permission systems for one question meant every answer had to
        // be given twice: could a party visitor be kicked (no, they had no
        // row), should a party kick eject (unclear, they might hold a row
        // anyway). Collapsing to one made both questions disappear rather than
        // answering them.
        //
        // What it buys, and the reason it is a feature rather than a
        // restriction: **the permission revokes itself**. A guest row is state
        // somebody has to remember to clean up; a party ends on its own, and
        // when it does the visit is over with nothing to tidy.
        //
        // What it costs, chosen knowingly: nobody can look at your house while
        // you are offline.
        bool VisitHouse(Player* player, std::string const& ownerName, std::string& error);

        // PARTY TRAVEL. Being in somebody's group IS the invitation -- no
        // guest-list row, nothing to add or drop, and it ends the moment the
        // group does.
        //
        // ONE RESOLVER, THREE ROUTES IN: walking through an entrance, the door
        // gear's menu, and `.house visit`. They cannot drift into disagreeing
        // about whose house is reachable, because none of them decides it.
        //
        // GetPartyLeader is the permission with no door in it, which is the
        // form `.house visit` needs. GetPartyLeaderHouse adds the door, which
        // is what makes the entrance offer narrow: it is only ever made at the
        // leader's OWN home door, so a party is never pulled through one none
        // of them chose to stand in.
        //
        // Both return false, leaving `out` untouched, for every ordinary
        // reason: no group, you ARE the leader, the leader is another of your
        // own characters, or the leader has no house.
        bool GetPartyLeader(Player* player, HousePartyHost& out);
        bool GetPartyLeaderHouse(Player* player, uint32 portalId, HousePartyHost& out);

        // The trip itself. `expectedLeader` is the staleness guard: pass the
        // guid the menu offered and it refuses if the party has changed leader
        // underneath it; pass an empty guid to accept whoever leads now, which
        // is what walking through the door does because nothing was promised.
        bool VisitPartyLeaderHouse(Player* player, ObjectGuid expectedLeader, uint32 portalId, std::string& error);

        // What the gear last offered this player, so the selection can tell
        // "the leader I named" from "whoever leads now". Taking it clears it.
        void SetPartyOffer(Player* player, ObjectGuid leaderGuid);
        ObjectGuid TakePartyOffer(Player* player);

        // THE LOCK. Read off the OWNER's account, never the visitor's, so it
        // is one answer for everybody standing at that door.
        bool IsLocked(House const& house) const;

        // The owner's answer to a knock, from `.house admit` and `.house
        // refuse`, which is what the ComfyUI popup types. Everything the knock
        // was made on is checked again: the popup is a photograph too.
        bool AnswerKnock(Player* owner, std::string const& visitorName, bool admit, std::string& error);

        // THE SELECTED OBJECT -- "the one I mean". Housing commands are
        // position-based because gameobjects cannot be targeted in 1.12, and
        // "nearest within reach" is a good default but a bad only-option: two
        // chairs at a table are half a yard apart, and the piece you want to
        // nudge is often one you cannot stand next to at all.
        //
        // VALIDATED ON EVERY READ rather than kept in step by hooks. A
        // selection can go stale half a dozen ways -- the object deleted, the
        // house cleared or reset, the owner moving house, the player walking
        // into somebody else's -- and one check that it still exists and still
        // belongs to the house you are standing in covers every one of them.
        // There is nothing to unhook and nothing to forget.
        // Turn what a player typed into a guid: a slot if it is small, the guid
        // itself if it is already one. Returns 0 for a number that names
        // nothing in this house.
        uint32 ResolveObjectRef(House const& house, uint32 typed) const;

        // What to CALL a piece of furniture. 9,577 of this DB's 21,200
        // gameobject_template rows carry their model PATH in the name column
        // with the backslashes turned into spaces -- "World EXPANSION01 DOODADS
        // GENERIC BLOODELF TABLES BE_Table_Large01.mdx" -- because that is how
        // the auto-generated catalogue rows were built. There is no shorter
        // column to reach for; the leaf has to be cut out of the path.
        //
        // THE SAME RULE AS THE ADDON'S CATALOGUE, which is the point: this is a
        // port of `prettify` in tools/gen-housing-catalog.js, and the two have
        // to agree or the card you clicked and the line you get back name the
        // same object differently. Change one, change the other.
        static std::string ObjectDisplayName(uint32 goEntry);

        // The same piece of furniture, written for a person: "Wooden Chair (3)".
        //
        // ONE SPELLING, EVERYWHERE. Placing, selecting, nudging, turning,
        // resizing, picking up and the gear's own greeting all name their
        // object through here, so the thing you clicked and the thing the line
        // came back about are provably the same words. It lived as a static in
        // HouseCommands.cpp until the gear wanted it too; the gear was the one
        // route printing a bare number, and that was the whole reason it read
        // differently from every other message housing sends.
        //
        // The number is house_object.slot, never the guid -- slots are what a
        // player types, and guids are what nothing prints.
        static std::string ObjectLabel(uint32 objectGuid);

        // The name half of ObjectLabel: the furniture item's name when the
        // piece came from one, else ObjectDisplayName of the model. Every line
        // that names a PLACED piece uses this; ObjectDisplayName alone is for
        // a model with no piece behind it (the catalogue, `.house object add`).
        static std::string PieceName(HouseObject const& o);

        uint32 GetSelectedObject(Player* player);

        // BLINK ONE PIECE OF FURNITURE, which is how "that one" is said in the
        // world when the sparkle will not draw. It fell out of the attempt at a
        // glow and earned its place: it needs no model, no SQL and no client
        // cooperation, it works on every furniture type, and it points at the
        // object rather than decorating it.
        void FlashObject(Player* player, uint32 objectGuid);

        // Keep a glow standing on whatever this player has selected, or take it
        // down when nothing is. Idempotent and cheap to over-call, which is
        // what lets every mutator invoke it without knowing whether the object
        // it just moved was the selected one.
        void RefreshSelectionMarker(Player* player);
        void DespawnSelectionMarker(Player* player);

        // The glow's size and height, live. Same reason as the handle and the
        // portal: gameobject_template has no reload, so tuning this from SQL
        // would cost a restart per attempt -- and picking how big a marker
        // should be is exactly the job that wants twenty tries.
        void  SetSelectMarkLook(uint32 displayId, float scale, float lift);
        float GetSelectMarkLift() const { return m_selectLift; }
        bool   SelectObject(Player* player, uint32 objectGuid, std::string& error);
        void   ClearSelection(Player* player);

        // ---- the chalk mark --------------------------------------------------
        //
        // A point on the floor that the next object placed will land on, set by
        // right-clicking the Decorator's Chalk and clicking the ground.
        //
        // WHY THE POINT HAS TO BE STORED AT ALL. Ground targeting is raised
        // entirely client-side and no opcode induces a cast, so a dot command
        // can never put the cursor into targeting mode -- using an item is the
        // only route to a reticle. An item that places nothing itself therefore
        // has nowhere to put the point except here. This is forced by the
        // protocol, not chosen over a one-step alternative.
        //
        // MEMORY ONLY, and validated on read exactly like m_selected: GetMark
        // re-checks the house the player is standing in NOW, which collapses
        // logging out, moving house, visiting somebody else's, a reset and a
        // restart into one check rather than five hooks.
        bool SetMark(Player* player, float const* at, std::string& error);
        // Fills `out` with x/y/z and returns true when there is a live mark in
        // the house the player is currently in. Anything else answers false and
        // quietly forgets the stale one.
        bool GetMark(Player* player, float* out);
        bool HasMark(Player* player) { float t[3]; return GetMark(player, t); }
        // Spent by a placement, rubbed out by the player, or dropped on the way
        // out of the map. Takes the glow down with it.
        void ClearMark(Player* player);

        void RefreshMarkMarker(Player* player);
        void DespawnMarkMarker(Player* player);

        // Placement. All four take a player standing in their own house.
        //
        // itemEntry and scale are defaulted rather than split into a second
        // function, so there stays exactly ONE place that writes a house_object
        // row. A second inserter is a second thing to forget a column in.
        //
        // `at` is a three-float spot to stand it on, and null means "I was not
        // aimed" -- NOT "put it ahead of the player". PlaceObject then looks for
        // a chalk mark, and only falls back to `distance` yards ahead when there
        // is no mark either. Given a spot, distance is ignored and a standing
        // mark is left alone: an explicit click has already been aimed.
        //
        // It is a pointer rather than an x/y/z triple plus a flag because "no
        // spot" then has exactly one spelling, and a caller cannot pass
        // coordinates it forgot to enable.
        bool PlaceObject(Player* player, uint32 goEntry, float distance, std::string& error,
                         uint32 itemEntry = 0, float scale = 0.0f, float const* at = nullptr);
        bool DragObject(Player* player, uint32 guid, std::string& error);
        bool OffsetObject(Player* player, uint32 guid, float dx, float dy, float dz, std::string& error);
        bool TurnObject(Player* player, uint32 guid, float degrees, bool faceMe, std::string& error);
        bool ScaleObject(Player* player, uint32 guid, float factor, bool relative, std::string& error);
        // returnedItem is set to the furniture item handed back, or 0 when the
        // object was simply destroyed -- so the caller can say "picked up"
        // rather than "removed" without looking the row up again after it is
        // gone.
        bool RemoveObject(Player* player, uint32 guid, std::string& error, uint32* returnedItem = nullptr);

        // ---- furniture groups ------------------------------------------------
        //
        // A group is a set of pieces that move, turn and resize as one. THE
        // RULE LIVES IN THE FOUR MUTATORS ABOVE, not in the menu, so the menu,
        // the chat commands and the addon's nudge pad all get it. Each mutator
        // computes every member's new place, checks all of them, and only then
        // applies any: one bad member refuses the whole action, so a set never
        // half-moves.
        //
        // The clicked piece is the pivot for a turn and a resize.

        // Every live member of o's group, o first. Only o when the group is
        // not live (see HouseObject::groupNo). The one place that decides
        // membership.
        void   GroupMembers(HouseObject const& o, std::vector<HouseObject*>& out);
        // How many pieces move when this one moves: 1 when it is not grouped.
        uint32 GroupSize(uint32 guid);
        // Put guid in the same group as withGuid. When guid is already in a
        // live group, that WHOLE group joins: a group is a unit, and joining
        // two units makes one.
        bool   JoinGroup(Player* player, uint32 guid, uint32 withGuid, std::string& error);
        // Take one piece out. When that leaves one piece, it is let go too.
        bool   LeaveGroup(Player* player, uint32 guid, std::string& error);

        // ---- furniture items -------------------------------------------------
        //
        // tw_world.house_furniture_item, item entry -> gameobject entry. The
        // whole of the furniture-item feature that is content rather than code:
        // adding a piece later is rows here and in item_template.
        void LoadFurnitureItems();
        uint32 FurnitureItemCount() const { return uint32(m_furnitureItems.size()); }

        // ---- the shop's tabs -------------------------------------------------
        //
        // The distinct non-blank categories, in the order their FIRST item
        // entry appears. That is the whole ordering rule and it needs no sort
        // column: the order tabs are drawn in is the row order in
        // sql/custom/042, which is where somebody deciding it would look.
        std::vector<std::string> const& FurnitureCategories() const
        { return m_furnitureCategories; }

        // Every furniture item filed under one category, as the whitelist
        // WorldSession::SendListInventory takes. Built per click rather than
        // cached: it is sixty-odd map entries walked when a player opens a
        // page, against a second copy of the catalogue to keep in step with
        // `.house furniture reload`.
        void FurnitureItemsIn(std::string const& category, std::set<uint32>& out) const;

        // Right-clicking a furniture item in your bags. Refuses -- without
        // consuming anything -- anywhere but inside your own house; every
        // ownership and capacity message comes back from PlaceObject unchanged.
        //
        // A CRATE NO LONGER AIMS, AND TAKES NO POINT. It carries an inert spell
        // with no TARGET_FLAG_DEST_LOCATION, so no client raises a reticle for
        // one and none is sent; aiming is the chalk's job for every route into
        // housing rather than a second mechanism owned by twelve items.
        //
        // There is deliberately no `at` parameter to ignore. A client running on
        // a stale WDB cache can still draw the old circle and send a point --
        // there is simply nothing here that could read it, which is a stronger
        // guarantee than a parameter somebody could start honouring again.
        //
        // Is this item entry one of ours at all? The map is the whole answer,
        // which is why nothing else needs to know what a crate looks like.
        bool IsFurnitureItem(uint32 itemEntry) const
        { return m_furnitureItems.find(itemEntry) != m_furnitureItems.end(); }

        bool UseFurnitureItem(Player* player, uint32 itemEntry, std::string& error);

        // Everything PlaceObject would refuse for, asked WITHOUT placing.
        // Deliberately not a second copy of those rules: same CheckOwner, same
        // cap, so a rule added there is a rule this asks about.
        //
        // It exists because placing a crate is a one-second cast, and a cast
        // that runs and then says "your house is full" is a worse answer than
        // an instant one. Ask first, then start the bar.
        bool CanPlaceFurniture(Player* player, std::string& error);

        // ---- critter crates --------------------------------------------------
        //
        // tw_world.house_critter_item, the exact counterpart of
        // house_furniture_item. sql/custom/059.
        //
        // THE TWO SHARE ONE ITEM SCRIPT AND ONE SPELL. A crate of either kind
        // carries script_name 'item_house_furniture' and spell 33453, and
        // HouseFurnitureShouldCast / HouseUseFurnitureItem tell them apart by
        // looking in this map when the furniture one misses. That is worth more
        // than the tidiness of a second script: a new spell script name is read
        // ONCE AT STARTUP, so it would have brought back the SQL-before-restart
        // ordering trap that sql/custom/052 and 055 exist to document.
        void LoadCritterItems();

        bool IsCritterItem(uint32 itemEntry) const
        { return m_critterItems.find(itemEntry) != m_critterItems.end(); }

        bool UseCritterItem(Player* player, uint32 itemEntry, std::string& error);
        uint32 CritterItemCount() const { return uint32(m_critterItems.size()); }

        // WHAT THE SHELF ACCEPTS, in one place. It has to be both catalogues:
        // StowHouseFurniture packs critter crates on a reset, so a shelf can
        // already hold one, and a deposit rule that refused them would leave a
        // crate you can take out and cannot put back.
        bool IsStorableItem(uint32 itemEntry) const
        { return IsFurnitureItem(itemEntry) || IsCritterItem(itemEntry); }

        // The critter twin of CanPlaceFurniture, and it exists for the same
        // reason: the cast is a second long and "your house is full of animals"
        // is a better answer before the bar than after it.
        bool CanPlaceCritter(Player* player, std::string& error);

        // ---- per-account settings --------------------------------------------
        //
        // tw_char.house_setting, one row per account per setting that has been
        // changed away from its default. An absent row IS the default, so the
        // table stays empty for anyone who never opens the page and there is no
        // backfill to run when a new setting is added here.
        void LoadSettings();

        // Is this a setting anybody can still see? A retired one keeps its id
        // for ever so that saved rows are never reinterpreted, which means the
        // table has holes in it and every loop over HOUSE_SETTING_MAX has to
        // skip them. Retired rows carry a null name, and this is the one place
        // that knows it.
        static bool IsSettingLive(HouseSetting setting);

        // Both forms answer the default for an account that has never set it,
        // so no caller ever has to know whether a row exists. The Player form is
        // the one to reach for; the account form is for the paths that have an
        // id and no session.
        uint32 GetSetting(uint32 accountId, HouseSetting setting) const;
        uint32 GetSetting(Player* player, HouseSetting setting) const;

        // Writes through, and re-applies anything the setting controls right
        // away -- turning the glow off has to take the glow down, not wait for
        // the next selection. Silently does nothing for a player with no
        // session, which cannot happen from a command.
        void SetSetting(Player* player, HouseSetting setting, uint32 value);

        // ONE PLACE THAT KNOWS WHAT THE SETTINGS ARE CALLED, because the chat
        // command and the panel on the exit gear both have to say the same
        // words about the same state -- and a label copied is a label that
        // drifts.
        //
        // Name is what you TYPE and says what ON means all by itself
        // (`point_and_click_placing`); Title is the same setting written for a
        // menu row ("Point-and-click placing"); Label is the value, which is
        // always "on" or "off"; Hint is one line about what turning it off
        // gets you, which is the only half the name does not already cover.
        static char const* SettingName(HouseSetting setting);
        static char const* SettingTitle(HouseSetting setting);
        static std::string SettingLabel(HouseSetting setting, uint32 value);
        static std::string SettingHint(HouseSetting setting);

        // The clause a state has to carry to avoid reading as broken, or "".
        // Only `placing` off has one -- the ground circle still appears, and
        // nothing server-side can stop it -- and it lives here so the command
        // and the gear say it in the same words, or not at all.
        static char const* SettingNote(HouseSetting setting, uint32 value);

        // "placing" -> HOUSE_SETTING_PLACING, by prefix, so `.house setting p`
        // works the way `.house obj a` does.
        static bool SettingFromWord(std::string const& word, HouseSetting& out);

        // Whether this one belongs to the server rather than to the player.
        // Global settings read and write account HOUSE_SETTING_GLOBAL_ACCOUNT
        // and are refused below SEC_DEVELOPER.
        static bool SettingIsGlobal(HouseSetting setting);

        // Milliseconds to instance ticks, rounded UP and never zero. Public
        // because the setting's own label prints what it rounded to.
        static uint32 ArrivePingTicks(uint32 ms);

        // on/off/yes/no/1/0 for any of them, plus the pair of words that reads
        // naturally for that particular setting (point/ahead).
        static bool SettingValueFromWord(HouseSetting setting, std::string const& word, uint32& out);

        // ---- furniture storage -----------------------------------------------
        //
        // tw_char.house_storage: an account's crates, held as item entry +
        // count. Keyed by ACCOUNT because a house is, so furniture bought on one
        // character is reachable from another -- which is the one thing the
        // player's own bank cannot do, being per character.
        //
        // Every entry point refuses outside your own house. That is not a
        // permission so much as a place: storage is a cupboard in the house, and
        // the panel on the way out is the door to it.
        void LoadStorage();

        // The one gate every storage entry point goes through, and the reason
        // there is no second rule to drift. Public because the bare
        // `.house storage` command has to ask it too -- GetStorage answers with
        // an empty shelf whether you are at home or in Ironforge, so listing
        // has to be refused explicitly rather than by accident.
        //
        // TWO PLACES NOW, NOT ONE: inside your own house (CheckOwner, as it
        // always was) or standing at your own front door (AtOwnHomeDoor). The
        // second is what lets the entrance gear carry a Furniture Storage row,
        // and it is the same shelf either side of the doorway -- storage is
        // keyed by account, so there was never a second one to confuse it with.
        //
        // The gate had to widen rather than the gear bypassing it. A window
        // that opened at the door while every put and take behind it was
        // refused would be worse than no row at all.
        bool StorageAllowed(Player* player, std::string& error);

        // Standing close enough to your own front door to reach the cupboard
        // behind it. Any door whose template matches your house's counts, for
        // the same reason walking into one takes you home: IsHomePortal is the
        // whole rule, and this is that rule plus a distance.
        //
        // MEASURED TO THE PORTAL ON THE FLOOR, never to the gear floating above
        // it -- the same care UpdateExitMarker needed, and for the same reason.
        // The range is the gear's HIDE range rather than its show range, so the
        // shelf never closes while the gear you opened it from is still there.
        bool AtOwnHomeDoor(Player* player);

        // The shelf, ONE ROW PER KIND. It used to be a fixed-length array with
        // a 0 in every empty square, because the caller was drawing a grid and
        // the gaps were the point. Nothing draws gaps now, and a fixed-length
        // answer to "what have I got" would have to be as long as the backstop.
        //
        // `slot` is the LOWEST square holding that kind, which is what makes
        // this answer usable rather than merely tidy: the server's verb is
        // still `.house storage take <square>`, so every row carries a square
        // even though nothing shows one.
        std::vector<HouseStorageStack> GetStorageStacks(Player* player) const;

        // How many crates in total -- the sum of the counts above. Its own
        // function because the two callers that want it (the window's counter
        // and the stow forecast) would otherwise both build the whole grouped
        // list to add up its numbers.
        uint32 GetStorageCount(Player* player) const;

        // What is in one square, or 0. The narrow read `take` needs: it has to
        // know WHICH crate it is about to hand over so it can name it, and it
        // has to ask before the withdraw rather than after.
        uint32 GetStorageAt(Player* player, uint32 slot) const;

        // slot 0 means "the first free square" -- what a right-click in the bags
        // and a bare command both want. A given slot is refused if it is taken,
        // rather than swapped, because there is no way to put the displaced
        // crate on the cursor to hand back.
        bool StorageDeposit(Player* player, uint32 itemEntry, uint32 slot, std::string& error);

        // Every crate in the bags, into whatever squares are free. `moved`
        // counts what actually went: a shelf that fills part way through is a
        // success with a smaller number, not an error.
        bool StorageDepositAll(Player* player, uint32& moved, std::string& error);

        // What a house full of furniture would come to if it were packed up.
        // Three answers, and each is a DIFFERENT KIND of loss, which is the
        // whole reason this is not one number:
        //
        //   savable  -- placed from an item, and there is room on the shelf
        //   noRoom   -- placed from an item, but the shelf fills up first
        //   noItem   -- never came from an item at all (.house object add and
        //               the addon place a model straight from the catalogue),
        //               so there is nothing to hand back and no shelf square
        //               would help
        //
        // Asked twice, and that is the point: once to write the confirm page,
        // and once as the thing StowHouseFurniture actually does. Counting is
        // pure -- it writes nothing -- so a player can be told exactly what a
        // move will cost BEFORE they agree to it, rather than reading the
        // damage afterwards.
        void CountStowable(House const& house, uint32& savable, uint32& noRoom, uint32& noItem) const;

        // The same three numbers written as one sentence for a confirm page,
        // or empty when there is nothing to warn about. Shared because BOTH
        // routes that tear a house down ask it -- `.house reset` and the door
        // gear's "Move here" -- and a forecast that disagreed with itself
        // between a command and a menu would be worse than none.
        std::string StowForecast(House const& house) const;

        // Pack the house up. Everything the player placed that came from an
        // item goes onto free shelf squares; `stowed` and `lost` come back the
        // way CountStowable predicted. Template fabric is never touched -- it
        // is not the player's, and the re-stamp is about to replace it.
        //
        // IT DOES NOT PURGE. The caller still tears the house down; this only
        // rescues what it can first, so a failure here can never be the reason
        // a move half-happened.
        void StowHouseFurniture(House const& house, uint32& stowed, uint32& lost);

        // Bag space is checked BEFORE the square is cleared, the same ordering
        // that makes picking an object up safe: full bags leave storage
        // untouched rather than losing the crate between the two.
        bool StorageWithdraw(Player* player, uint32 slot, std::string& error);

        // Rearranging within the shelf. A swap when the target is occupied,
        // which is safe here in a way that swapping with a BAG item is not:
        // both crates stay in storage, so there is never a displaced one that
        // needs somewhere to go.
        bool StorageMove(Player* player, uint32 from, uint32 to, std::string& error);

        // Empty the house in one go. Deliberately a separate entry point rather
        // than something the caller loops over: RemoveObject mutates the object
        // list it is iterating, so the copy has to be made somewhere and here is
        // the only place that can see both lists. `kept` is what stayed because
        // it came with the house -- always 0 for a developer.
        bool ClearHouse(Player* player, uint32& removed, uint32& kept, uint32& noRoom, std::string& error);

        // Edit mode -- see HouseHandle.cpp. Turning it on tears down any
        // previous session first, so it is always re-runnable.
        bool  SetEditMode(Player* player, bool on, std::string& error);
        bool  IsEditing(Player* player) const;
        bool  IsEditingAll(Player* player) const;
        void  ClearEdit(Player* player);

        // IS THIS OBJECT IN THIS PLAYER'S EDIT SESSION -- which is now the same
        // question as "is it clickable", because that is exactly what the
        // session decides. Object::BuildValuesUpdate asks it for every housing
        // gameobject it writes, and both core click hooks ask it again before
        // opening a menu.
        bool  InEditSession(Player* player, uint32 objectGuid) const;

        float  GetEditStep(Player* player, HouseStepKind kind) const;
        void   CycleEditStep(Player* player, HouseStepKind kind);

        // Keep the session in step with the house: an object placed while edit
        // mode is on joins it, and one that is picked up leaves. Without these,
        // edit mode only ever describes the house as it was at the moment it
        // was switched on.
        void   AddToEdit(Player* player, uint32 objectGuid);
        void   DropFromEdit(Player* player, uint32 objectGuid);

        // Called from the instance script every map update, on a throttle that
        // lives there because per-map state does.
        void   UpdateEditSessions(Map* map);


        HouseObject const* GetObject(uint32 guid) const;

        // The entrance portals -- see HousePortal.cpp. Placed by hand with
        // .house portal, so a doorway is a choice made by standing in one
        // rather than a constant compiled in here. There may be many.
        void LoadPortals();
        bool AddPortal(Player* player, std::string& name, uint32 templateId, uint32& newId, std::string& error);
        bool MovePortal(Player* player, uint32 id, std::string& error);
        bool RemovePortal(uint32 id, std::string& error);
        bool SetPortalTemplate(uint32 portalId, uint32 templateId, std::string& error);
        HousePortal const* GetPortal(uint32 id) const;

        // Two lookups, and picking the wrong one is destructive. GetNearestPortal
        // is unranged -- "which is closest", which is all `entrance where` wants.
        // GetPortalNear is "the one you are standing at" and is what every bare
        // form must use: without the range, .house entrance remove in Ironforge
        // deletes a door in Stormwind, silently, because it is the nearest one
        // on map 0.
        HousePortal const* GetNearestPortal(Player* player) const;
        HousePortal const* GetPortalNear(Player* player) const;
        std::vector<HousePortal const*> GetPortals() const;
        bool HasPortals() const { return !m_portals.empty(); }

        // Which door is yours. Set by clicking a portal and choosing, or with
        // .house portal home <id>.
        bool SetHomePortal(Player* player, uint32 portalId, std::string& error);

        // The guid carries the id, so a portal can say which one it is without
        // a lookup. Zero means it is not one of ours. Public because the menu
        // is a free function rather than a member.
        static uint32 PortalIdFromGuid(uint32 lowGuid);
        void RememberEntrance(Player* player, uint32 portalId);

        // Keeps a clickable gear standing in each entrance. Called from the
        // portal AI, which is the only thing that ticks out in the world.
        void UpdatePortalMarker(GameObject* portal);
        uint32 GetPortalIdByMarker(ObjectGuid marker) const;

        // What the gear looks like and how high it floats, live -- same reason
        // as the portal model and the edit handle: gameobject_template has no
        // reload, so tuning it from SQL would cost a restart per attempt, and
        // picking a size is exactly the job that wants twenty tries.
        // Deliberately NOT persisted; whatever it settles on belongs in
        // sql/custom/034.
        void SetPortalMarkScale(float scale);
        void SetPortalMarkOffset(float x, float y, float z);
        float GetPortalMarkScale() const;
        // Live-tunable proximity for both gears. SetMarkRange keeps hide
        // strictly beyond show -- equal or inverted numbers would summon and
        // despawn the gear on alternate ticks, which is the one state these two
        // exist to prevent.
        void  SetMarkRange(float show, float hide);
        float GetMarkShow() const { return m_markShow; }
        float GetMarkHide() const { return m_markHide; }

        float GetPortalMarkX() const { return m_markOffX; }
        float GetPortalMarkY() const { return m_markOffY; }
        float GetPortalMarkZ() const { return m_markOffZ; }
        // Take one gear down, by portal id. Every path that unspawns or moves a
        // portal has to call this: the AI only ever grows a marker that is
        // MISSING, so a gear nobody took down is a gear that stays exactly where
        // it was, and once its portal is gone DespawnPortalMarkers cannot find
        // the map to reach it on either.
        void DespawnPortalMarker(uint32 portalId, uint32 mapId);
        void DespawnPortalMarkers();

        // Called from both portal scripts every HOUSE_PORTAL_INTERVAL.
        void CheckPortal(GameObject* portal, bool leaving);

        // The trip back out, and the counterpart to SendPlayerHome. Falls back
        // to the hearthstone when no entrance has been placed.
        // IS THIS DOOR THIS HOUSE'S? One place, because five call sites used to
        // spell it `house->portalId == id` and a rule written five times is a
        // rule that changes in four.
        //
        // TWO WAYS TO BE HOME, and the second is why this exists. A house
        // stamped from a template is at home behind ANY door bound to that same
        // template -- the fabric behind them is identical, so which of them you
        // walk through is not a distinction the house has any reason to make.
        // Before this, a door removed and re-created came back with a new id
        // and left the house pointing at nothing: same room, same template, and
        // "this is not your door".
        //
        // The exact-id form stays first and stays authoritative for houses with
        // no template at all -- the pre-template world, where a door is the
        // only thing a house can be tied to.
        bool IsHomePortal(House const& house, HousePortal const& portal) const;
        bool IsHomePortal(House const& house, uint32 portalId) const;

        // A door this house is at home behind, for the paths that need a
        // concrete one rather than a yes/no: the way out after a relog, when
        // the id it was tied to is gone. Prefers the recorded id.
        HousePortal const* GetHomePortal(House const& house) const;

        bool SendPlayerToPortal(Player* player, std::string& error);

        // Where a login inside a house puts the player: outside the door, for
        // Player::LoadFromDB. False means no door, and the caller uses the
        // hearthstone. instanceId is the house they logged out in, 0 if unknown.
        bool GetLoginExit(Player* player, uint32 instanceId, WorldLocation& loc);

        // Keeps one exit portal standing in a house. Called from the instance
        // script's update, alongside UpdateHandles.
        void UpdateExitPortal(Map* map);

        // And the gear beside it, which carries the house's control panel. Same
        // shape, same throttle, and the same reason for being a temporary
        // summon: it costs no guid block and comes back when the instance does.
        void UpdateExitMarker(Map* map);
        void DespawnExitMarker(Map* map);

        // Show out anyone standing in this house whose party ticket has lapsed
        // -- through the door, on `SendPlayerToPortal`, which is where the exit
        // portal's own Leave puts them. Same throttle as the two above, and a
        // POLL rather than a hook for a reason worth keeping:
        //
        // A party can end in six ways -- the leader kicks you, you leave, the
        // group disbands, either of you logs out, the leader is promoted away,
        // or the leader simply zones. Hooking `Group::RemoveMember` would catch
        // one of them and couple housing to the group code for the privilege.
        // Asking the same question the DOOR asks, on a timer, catches all six
        // and costs one loop over the players in one instance.
        //
        // It is the exact mirror of entry: `GetPartyLeaderHouse` is resolved
        // live and never stored, so the permission cannot go stale in either
        // direction. Nothing here has to be undone when a party ends, because
        // nothing was written when it began.
        //
        // A DEVELOPER IS EXEMPT. They get in without a party at all
        // (VisitHouse's staff bypass, which exists to keep them off `.appear`),
        // so a rule that evicted anyone without a live ticket would bounce them
        // out within seconds of arriving.
        void EvictLapsedVisitors(Map* map);

        // Which house a clicked house-side gear belongs to, or nullptr. Scans
        // m_exitMarkers, exactly as GetPortalIdByMarker scans its own map --
        // few houses loaded at once, rare clicks, and a second index is a
        // second thing to keep in step.
        House* GetHouseByExitMarker(ObjectGuid marker);

        // Where that portal stands. `.house template exit` puts it where the
        // AUTHOR is standing, which is the whole point of it: a doorway is
        // something you build, not a constant somebody compiled in.
        // GetExitPosition is the single answer -- the chosen spot, or the old
        // offset -- so the summon and the report can never drift apart.
        bool SetExitPortal(Player* player, std::string& error);
        bool ClearExitPortal(Player* player, std::string& error);
        void GetExitPosition(House const& house, float& x, float& y, float& z, float& o) const;

        // Where you LAND when you come home. Derived from the way out rather
        // than stored beside it, because a door is one thing: arriving somewhere
        // you cannot see your own exit from is how you get lost in your own
        // house. Setting a template's exit therefore moves the arrival too,
        // and the two can never be configured into disagreeing.
        void GetArrivalPosition(House const& house, float& x, float& y, float& z, float& o) const;

        // "An author, standing in the template they are authoring." Narrower
        // than CheckOwner on purpose: the way out belongs to the TEMPLATE, so
        // a resident cannot move their own and a developer cannot move one in
        // somebody else's house. HousePortal.cpp needs the same test the
        // command makes, and a permission check that exists twice is one that
        // will eventually disagree with itself.
        House* GetAuthoredTemplateAt(Player* player, std::string& error);

        // What the portals look like, live, for the same reason as the handle:
        // gameobject_template has no reload.
        void SetPortalLook(uint32 displayId, float scale);
        uint32 GetPortalDisplay() const { return m_portalDisplay; }
        float  GetPortalScale() const { return m_portalScale; }

        // Called by the instance script when a copy of the house map is created,
        // before any grid loads. Registers this house's furniture with the
        // instance so ObjectGridLoader picks it up.
        void OnHouseMapCreated(Map* map);

        // The house the player is standing in, or nullptr.
        House* GetHouseAt(Player* player);

        std::vector<HouseObject const*> GetObjects(uint32 houseId);

        //== critters ========================================================
        // Everything here is defined in HouseCritter.cpp. It is the furniture
        // path with AddCreatureToGrid in place of AddGameobjectToGrid, which is
        // genuinely the whole difference -- ObjectGridLoader::Visit reads the
        // per-instance grid set for both.

        // Loaded after the houses, so a row can find its house. Refuses to load
        // anything at all if the creature guid reserve was left too low; see
        // the block map in this file.
        void LoadCritters();

        // itemEntry is the crate it came out of, or 0 for `.house critter add`
        // -- recorded on the row so picking it up can hand the right item back.
        bool PlaceCritter(Player* player, uint32 entry, float wander, std::string& error,
                          uint32 itemEntry = 0);
        bool RemoveCritter(Player* player, uint32 guid, std::string& error,
                           uint32* returnedItem = nullptr);

        // Bring one to where the player stands, and/or change its range. Both
        // in one function because both rewrite the row and re-seat the live
        // creature, and two copies of that is two ways to forget the re-seat.
        bool MoveCritter(Player* player, uint32 guid, bool drag,
                         bool setWander, float wander, std::string& error);

        std::vector<HouseCritter const*> GetCritters(uint32 houseId);
        HouseCritter const* GetCritter(uint32 guid) const;

        // The number a player typed, else the nearest within reach. There is no
        // selection for critters -- they are things you walk up to, and a house
        // holds ten at most, so the furniture's three-answer ladder would be
        // machinery for a problem this does not have.
        uint32 FindCritter(Player* player, uint32 typed) const;

        // The name to print. creature_template.name is authored for every one
        // of these, so there is no ObjectDisplayName-style path-mangling to do.
        static std::string CritterName(uint32 entry);
        static std::string CritterLabel(uint32 guid);

        // The name WITHOUT the slot number: what the animal is called, full
        // stop. CritterLabel is this plus "(n)", so there is still one place
        // that decides whether a given name beats the species.
        static std::string CritterShortName(uint32 guid);

        void RegisterCritterWithMap(Map* map, HouseCritter const& c);

        // What makes a rabbit a house rabbit rather than a hunting target:
        // immune to everything, and no attackable icon. Applied to the SPAWNED
        // CREATURE, never to creature_template -- those 129 rows are Turtle's,
        // so an edit there would be reverted by the next world update and would
        // change the animal everywhere else in the world at the same time.
        //
        // Public because the HouseDressCritter free function calls it, which is
        // the seam Creature::LoadFromDB reaches housing through.
        void DressCritter(Creature* creature, HouseCritter const& c);

        // Being patted. Anyone may pat -- a guest included. It changes nothing,
        // so gating it behind CheckOwner would be a rule with nothing to
        // protect, and a bunny only its owner can say hello to is a sadder
        // thing than an unguarded one.
        void PatCritter(Player* player, Creature* creature);

        // Is this player the one the animal answers to? Asked by the per-viewer
        // npc-flag override AND by both gossip hooks, so what the client is
        // shown and what the server accepts can never disagree.
        bool CanOwnerTalkTo(Player* viewer, uint32 critterGuidLow) const;

        void SendCritterMenu(Player* player, HouseCritter const& c, ObjectGuid guid);
        void HandleCritterMenu(Player* player, uint32 guid, uint32 action, char const* code);

        // Name it. Sanitised and capped rather than rejected on a bad
        // character: somebody typing an apostrophe at a rabbit should get a
        // named rabbit, not a lecture.
        bool NameCritter(Player* player, uint32 guid, std::string name, std::string& error);
        float DefaultWanderFor(HouseCritter const& c) const;

        // DEV, live-tuned like `.house object marker` and `.house entrance
        // model`: play a SpellVisual on the nearest critter so the six heart
        // candidates can be compared in one sitting. Not saved anywhere -- once
        // the right one is known it belongs in HOUSE_CRITTER_HEART_VISUAL.
        void ShowCritterVisual(Player* player, uint32 guid, uint32 visualId);

    private:
        House* CreateHouse(uint32 accountId);

        // The next free slot in a house: one past the highest live one.
        // COMPUTED, never stored -- a counter would be a second source of truth
        // for something the rows already say, and would drift the first time a
        // purge emptied a house behind its back. It does mean a deleted
        // object's number can come back on the next placement, which is the
        // trade: numbers that stay small, against a stale reference to
        // something that no longer exists.
        uint32 AllocateSlot(House const& house) const;
        bool   PrepareInstance(House& house);

        // The arrival half of a visit, with no permission check of its own:
        // prepare the instance, force it, land beside the way out. Shared by
        // the guest list and by a party following its leader, so the two can
        // never drift on the forced instance or the teleport flag.
        bool   EnterHouseAsVisitor(Player* player, House& house, std::string& error);

        // A party member at a LOCKED door: record the knock and ask the owner.
        // Always returns false, since nobody travelled. `error` is empty when
        // the knock went out, because the knock prints its own line.
        bool   Knock(Player* visitor, House const& house, HousePartyHost const& host, uint32 portalId, std::string& error);
        uint32 AllocateObjectGuid();

        // Copy a template's frozen rows into a house: fresh guids, source =
        // TEMPLATE, the template's exit, and house.template_id. Registers and
        // spawns live if the instance happens to be loaded (a guest inside
        // during a move); otherwise OnHouseMapCreated picks the rows up.
        void   StampTemplate(House& house, HouseTemplate const& tpl);

        // RemoveObject without the player: the teardown half, shared with
        // moves and template deletion, where nobody is standing in the house.
        void   PurgeObject(House& house, uint32 guid);

        // The shared middle of moving and resetting a claimed home: purge
        // every row, take the exit portal down, zero exit + template_id, then
        // stamp the given template (0 = leave it bare). One function, so the
        // two commands cannot drift.
        void   RebuildHouse(House& house, uint32 templateId);
        void   RegisterObjectWithMap(Map* map, HouseObject const& obj);
        GameObject* SpawnObjectNow(Map* map, HouseObject const& obj);

        // The critter half of the same three. PurgeCritters is the whole-house
        // form, called from RebuildHouse beside the furniture purge -- a reset
        // that left the animals standing in an empty room would be a fine bug
        // to explain and a poor one to ship.
        Creature* SpawnCritterNow(Map* map, HouseCritter const& c);
        void   PurgeCritter(House& house, uint32 guid);
        void   PurgeCritters(House& house);
        uint32 AllocateCritterGuid();
        uint32 AllocateCritterSlot(House const& house) const;
        uint32 CountCritters(House const& house) const;
        // Where one piece is to stand: position, facing and size. scale 0
        // leaves the size alone.
        struct Placement
        {
            HouseObject* obj;
            float x, y, z, o;
            float scale;
        };

        // Position, facing and size in ONE take-out-and-put-back. A group turn
        // moves and turns every member, and a group resize moves and resizes
        // them; two separate updates would blink each piece twice.
        void   ApplyPlacement(Map* map, Placement const& p);
        // Check all, then apply all. The group mutators end here.
        bool   PlaceAll(Map* map, std::vector<Placement> const& all, std::string& error);

        // One past the highest group number in this house. Computed, like
        // AllocateSlot, for the same reason.
        uint32 AllocateGroupNo(House const& house) const;

        // The one place that decides whether a gear exists. Every route that
        // changes a session ends here, so no two of them can disagree.
        void   Reconcile(Player* player, HouseEdit& session);

        void   SpawnPortals();
        void   SpawnPortalObject(HousePortal const& p);
        void   DespawnPortalObject(HousePortal const& p);
        void   DespawnExitPortal(Map* map);

        bool m_enabled;
        uint32 m_nextObjectGuid;

        // 0 until LoadCritters is satisfied the guid block is safe. Nothing
        // places a critter while it is 0, so a server whose reserve was never
        // raised keeps its houses and simply has no animals in them.
        uint32 m_nextCritterGuid = 0;
        std::map<uint32, House> m_houses;                // by house id
        std::map<uint32, uint32> m_houseByAccount;       // account -> house id
        std::map<uint32, uint32> m_houseByInstance;      // instance -> house id
        // account -> (slot -> item entry). Loaded once at startup and written
        // through on every change, like the houses themselves. Sparse: a square
        // with nothing in it has no row.
        std::map<uint32, std::map<uint32, uint32>> m_storage;
        // account -> (setting -> value). Sparse in the same way and for a
        // stronger reason: a missing entry is the default, so an account that
        // has never changed anything costs no memory and no row.
        std::map<uint32, std::map<uint32, uint32>> m_settings;
        std::map<uint32, HouseObject> m_objects;         // by guid
        std::map<uint32, HouseCritter> m_critters;       // by guid
        std::map<uint32, uint32> m_critterBeats;         // instance id -> ticks left
        std::map<uint32, time_t> m_critterPatted;        // critter guid -> next pat allowed
        std::map<uint32, HouseCritterWalk> m_critterWalks; // critter guid -> where it is headed
        std::map<uint32, FurnitureItem> m_furnitureItems; // item entry -> model
        std::vector<std::string> m_furnitureCategories;  // the shop's tabs, in order
        std::map<uint32, CritterItem> m_critterItems;    // item entry -> animal
        std::map<uint32, HouseTemplate> m_houseTemplates;// by template id
        std::map<std::string, uint32> m_templateByName;  // name -> template id
        std::map<uint32, HouseEdit> m_editing;           // player guid -> session

        // The object each player has chosen, so a bare command stops meaning
        // "whatever is nearest". Memory only, and never swept: every read
        // re-validates it against the house the player is actually in, so a
        // stale entry is harmless and self-clearing.
        std::map<uint32, uint32> m_selected;             // player guid -> object guid

        // The glow standing on each player's selection. Memory only, like the
        // edit handles it sits beside: a summon does not survive a restart and
        // neither should the record of one.
        std::map<uint32, ObjectGuid> m_selectMarkers;    // player guid -> the glow
        float m_selectLift = HOUSE_SELECT_MARK_LIFT;

        // The chalked spot, and the glow standing on it. Both memory only, for
        // the same reason the selection is: a mark is a working state that
        // lasts until the next thing is placed, and a summon does not survive a
        // restart so neither should the record of one.
        //
        // The house id is stored WITH the point because a bare x/y/z is not
        // enough to validate: map 28 is one plane and every house sits on the
        // same coordinates, so a mark made in your own house would otherwise
        // read as live in somebody else's.
        struct ChalkMark { uint32 houseId; float x, y, z; };
        std::map<uint32, ChalkMark>  m_marks;            // player guid -> the spot
        std::map<uint32, ObjectGuid> m_markMarkers;      // player guid -> the glow on it
        uint32 m_templateDisplay;                        // the row as loaded, for reset
        float  m_templateSize;

        // Every entrance in the world, by id. A row present means placed; there
        // is deliberately no "map 0 means unset" convention, because map 0 is
        // Eastern Kingdoms and the likeliest place of all to want a door.
        std::map<uint32, HousePortal> m_portals;

        // Which door each player actually walked in through, so the way out
        // returns them to it rather than to whichever one they call home.
        // MEMORY ONLY and deliberately so: it is true for the length of a
        // visit, and a restart while somebody is indoors simply falls back to
        // their home portal, which is a good answer rather than a wrong one.
        std::map<uint32, uint32> m_enteredBy;            // player guid -> portal id
        std::map<uint32, HouseArrival> m_arrivals;       // player guid -> armed ping

        // Which party leader the door gear NAMED to this player, so a click
        // arriving after a promotion or a disband is refused rather than
        // quietly honoured against somebody else. Memory only, and cleared the
        // moment it is read: it is true only for as long as one menu is open.
        std::map<uint32, ObjectGuid> m_partyOffer;       // player guid -> the leader offered

        // Knocks waiting on an owner. Keyed by the VISITOR, so one person has
        // one knock at a time: standing in the ring re-trips the door every
        // few seconds, and each trip must not be a new popup.
        std::map<uint32, HouseKnock> m_knocks;           // visitor guid -> knock
        std::map<uint32, HouseKnockTouch> m_knockTouch;  // visitor guid -> ring they knocked from

        uint32 m_portalDisplay;                          // 0 = whatever the template says
        float  m_portalScale;
        uint32 m_portalTemplateDisplay;                  // the row as loaded, for reset
        float  m_portalTemplateSize;
        std::map<uint32, time_t> m_portalCooldown;       // player guid -> when it may fire again
        std::map<uint32, ObjectGuid> m_exitPortals;      // instance id -> the portal in it
        std::map<uint32, ObjectGuid> m_portalMarkers;    // portal id -> its clickable gear
        std::map<uint32, ObjectGuid> m_exitMarkers;      // instance id -> the house-side gear
        // Yards from the portal, on the WORLD axes with Z up -- the same
        // convention .house move uses, rather than a facing-relative one, so
        // that "up a bit" means the same thing everywhere in housing.
        float  m_markShow;
        float  m_markHide;
        float  m_markOffX;
        float  m_markOffY;
        float  m_markOffZ;
        float  m_markTemplateSize;                       // the row as loaded, for reset
};

// A plain global, like sZoneScriptMgr -- no locking wrapper, because every
// entry point is either startup or the world thread.
extern HouseMgr sHouseMgr;

// Chat output shared between the commands that print it and the control panel
// on the exit gear, which shows the same things. Defined in HouseCommands.cpp;
// one implementation, so a menu and a command can never disagree.
void HousePrintObjectList(Player* player);
void HousePrintStorage(Player* player);
void HousePrintHouseHelp(Player* player);

// Sends the shelf to the addon and asks it to open the window. Harmless with no
// addon listening, which is why the gossip option prints the list beside it.
void HouseOpenStorageWindow(Player* player);

void AddSC_player_house();
void AddSC_house_portal();
void AddSC_house_control();

#endif
