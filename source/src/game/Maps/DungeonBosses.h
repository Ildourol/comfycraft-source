#pragma once

// The bosses of each 5-man dungeon, for the run timer (DungeonRun.h).
//
// A run is cleared when every required slot of its dungeon (or wing) has a
// kill. A row
// is one slot, except rows that share a oneOf id: they are one slot, filled by
// whichever of them dies (an encounter that picks one boss of several).
//
// A required boss is present in every instance and on every normal route:
// spawned, or summoned by an event the clear always triggers. A boss that
// needs an optional event (a gong, a gossip, a click, a key a group can
// skip), or that some routes go past, is optional. Rare spawns in pools,
// quest-item summons and seasonal bosses are left out. A missing required
// boss ends a run early; a required boss that does not always appear makes a
// run that never ends. Raids are not listed.
//
// The rows were checked against the spawns, pools and instance scripts on
// 2026-10-20. Two traps found then:
// - Timmy the Cruel (Stratholme Live) is summoned when one Crimson Guardsman
//   dies. If that guardsman evades first, Timmy never comes and the run
//   cannot end.
// - Dragonmaw Retreat and Stormwrought Ruins have a front and a back
//   entrance. A run of one half never ends.
//
// mod-dungeon-clear keeps its own list (DcBossEntries1121.h) with the bots'
// route order. The two are separate: the bots' list leaves out bosses they
// cannot route to.

#include "Common.h"

struct DungeonBossWing
{
    uint16 mapId;
    uint8 wing;
    char const* name;
};

struct DungeonBoss
{
    uint16 mapId;
    uint8 wing;     // 0 on a map with one part
    uint8 oneOf;    // 0, or a group id: the rows of one group are one slot
    bool optional;  // the kill is a split, but the run does not wait for it
    uint32 entry;
};

struct DungeonLastBoss
{
    uint16 mapId;
    uint8 wing;
    uint32 entry;
};

// The last boss of each dungeon or wing: the end boss where the dungeon has
// one, else the highest-level boss (the owner's rule). The last-boss time is
// the kill that leaves every listed row of the dungeon or wing dead, so two
// rows mean one fight that ends with both. Each must be a required boss.
inline constexpr DungeonLastBoss DUNGEON_LAST_BOSSES[] = {
    { 389, 0, 11519 },  // Bazzalan (the owner's pick)
    {  43, 0,  3654 },  // Mutanus the Devourer
    {  36, 0,   639 },  // Edwin VanCleef
    {  33, 0,  4275 },  // Archmage Arugal
    {  34, 0,  1716 },  // Bazil Thredd (highest level)
    {  48, 0,  4829 },  // Aku'mai
    {  90, 0,  7800 },  // Mekgineer Thermaplugg
    {  47, 0,  4421 },  // Charlga Razorflank
    { 189, 1,  4543 },  // Bloodmage Thalnos
    { 189, 2,  6487 },  // Arcanist Doan
    { 189, 3,  3975 },  // Herod
    { 189, 4,  3977 },  // High Inquisitor Whitemane
    { 129, 0,  7358 },  // Amnennar the Coldbringer
    {  70, 0,  2748 },  // Archaedas
    { 209, 0,  7267 },  // Chief Ukorz Sandscalp
    { 349, 0, 12201 },  // Princess Theradras
    { 109, 0,  5709 },  // Shade of Eranikus
    { 230, 0,  9019 },  // Emperor Dagran Thaurissan
    { 229, 1,  9568 },  // Overlord Wyrmthalak
    { 229, 2, 10363 },  // General Drakkisath
    { 289, 0,  1853 },  // Darkmaster Gandling
    { 329, 1, 10813 },  // Balnazzar (his slot also takes Dathrohan)
    { 329, 2, 10440 },  // Baron Rivendare
    { 429, 1, 11492 },  // Alzzin the Wildshaper
    { 429, 2, 11486 },  // Prince Tortheldrin
    { 429, 3, 11501 },  // King Gordok
    { 802, 0, 92110 },  // Master Raxxieth (highest level)
    { 816, 0, 62037 },  // Zuluhed the Whacked (three tie on level; a guess)
    { 818, 0, 62671 },  // Ighal'for and
    { 818, 0, 62673 },  //   Mergothid, one fight
    { 820, 0, 62782 },  // Chieftain Shalk Blackwind (highest level)
    { 822, 0, 63130 },  // Hailar the Frigid (all level 16; a guess)
    { 815, 0, 61418 },  // Genn Greymane
    { 808, 0, 60737 },  // Har'gesh Doomcaller
    { 800, 0, 91928 },  // Alarus
    { 269, 0, 65125 },  // Antnormi
    {  35, 0, 93107 },  // Arc'tiras
};

// Maps with isolated parts. The first boss kill of a run picks its wing.
inline constexpr DungeonBossWing DUNGEON_BOSS_WINGS[] = {
    { 189, 1, "Graveyard" },
    { 189, 2, "Library" },
    { 189, 3, "Armory" },
    { 189, 4, "Cathedral" },
    { 229, 1, "Lower" },
    { 229, 2, "Upper" },
    { 329, 1, "Live" },
    { 329, 2, "Undead" },
    { 429, 1, "East" },
    { 429, 2, "West" },
    { 429, 3, "North" },
};

inline constexpr DungeonBoss DUNGEON_BOSSES[] = {
    // Ragefire Chasm (map 389)
    { 389, 0, 0, false, 11517 },  // Oggleflint
    { 389, 0, 0, false, 11518 },  // Jergosh the Invoker
    { 389, 0, 0, false, 11519 },  // Bazzalan
    { 389, 0, 0, false, 11520 },  // Taragaman the Hungerer

    // Wailing Caverns (map 43)
    {  43, 0, 0, false,  3671 },  // Lady Anacondra
    {  43, 0, 0, false,  3669 },  // Lord Cobrahn
    {  43, 0, 0, false,  3670 },  // Lord Pythas
    {  43, 0, 0, false,  3673 },  // Lord Serpentis
    {  43, 0, 0, false,  3674 },  // Skum
    {  43, 0, 0, false,  5775 },  // Verdan the Everliving
    {  43, 0, 0, false, 61965 },  // Vangros
    {  43, 0, 0, false, 61968 },  // Zandara Windhoof
    {  43, 0, 0, false,  3654 },  // Mutanus the Devourer
    {  43, 0, 0, true,  3653 },  // Kresh (optional)
    {  43, 0, 0, true,  5912 },  // Deviate Faerie Dragon (optional)

    // Deadmines (map 36)
    {  36, 0, 0, false,   644 },  // Rhahk'Zor
    {  36, 0, 0, false,   643 },  // Sneed
    {  36, 0, 0, false,  1763 },  // Gilnid
    {  36, 0, 0, false, 61961 },  // Jared Voss
    {  36, 0, 0, false, 61963 },  // Masterpiece Harvester
    {  36, 0, 0, false,   646 },  // Mr. Smite
    {  36, 0, 0, false,   647 },  // Captain Greenskin
    {  36, 0, 0, false,   639 },  // Edwin VanCleef
    {  36, 0, 0, false,   645 },  // Cookie

    // Shadowfang Keep (map 33)
    {  33, 0, 0, false,  3914 },  // Rethilgore
    {  33, 0, 0, false,  3886 },  // Razorclaw the Butcher
    {  33, 0, 0, false,  3887 },  // Baron Silverlaine
    {  33, 0, 0, false,  4278 },  // Commander Springvale
    {  33, 0, 0, false,  4279 },  // Odo the Blindwatcher
    {  33, 0, 0, false,  4274 },  // Fenrus the Devourer
    {  33, 0, 0, false,  3927 },  // Wolf Master Nandos
    {  33, 0, 0, false,  4275 },  // Archmage Arugal
    {  33, 0, 0, false, 61969 },  // Prelate Ironmane

    // Stormwind Stockade (map 34)
    {  34, 0, 0, false,  1696 },  // Targorr the Dread
    {  34, 0, 0, false,  1666 },  // Kam Deepfury
    {  34, 0, 0, false,  1717 },  // Hamhock
    {  34, 0, 0, false,  1716 },  // Bazil Thredd
    {  34, 0, 0, false,  1663 },  // Dextren Ward

    // Blackfathom Deeps (map 48)
    {  48, 0, 0, false,  4887 },  // Ghamoo-ra
    {  48, 0, 0, false,  4831 },  // Lady Sarevess
    {  48, 0, 0, false,  6243 },  // Gelihast
    {  48, 0, 0, false, 12902 },  // Lorgus Jett
    {  48, 0, 0, false, 62530 },  // Velthelaxx the Defiler
    {  48, 0, 0, false,  4830 },  // Old Serra'kis
    {  48, 0, 0, false,  4832 },  // Twilight Lord Kelris
    {  48, 0, 0, false,  4829 },  // Aku'mai
    {  48, 0, 0, true, 12876 },  // Baron Aquanis (optional)

    // Gnomeregan (map 90)
    {  90, 0, 0, false,  7079 },  // Viscous Fallout
    {  90, 0, 0, false,  6235 },  // Electrocutioner 6000
    {  90, 0, 0, false,  6229 },  // Crowd Pummeler 9-60
    {  90, 0, 0, false,  7800 },  // Mekgineer Thermaplugg
    {  90, 0, 0, true,  7361 },  // Grubbis (optional)

    // Razorfen Kraul (map 47)
    {  47, 0, 0, false,  6168 },  // Roogug
    {  47, 0, 0, false,  4424 },  // Aggem Thorncurse
    {  47, 0, 0, false,  4428 },  // Death Speaker Jargba
    {  47, 0, 0, false,  4420 },  // Overlord Ramtusk
    {  47, 0, 0, false, 62503 },  // Rotthorn
    {  47, 0, 0, false,  4422 },  // Agathelos the Raging
    {  47, 0, 0, false,  4421 },  // Charlga Razorflank

    // Scarlet Monastery (map 189)
    // Graveyard
    { 189, 1, 0, false,  3983 },  // Interrogator Vishas
    { 189, 1, 0, false,  4543 },  // Bloodmage Thalnos
    { 189, 1, 0, false, 61972 },  // Duke Dreadmoore
    // Library
    { 189, 2, 0, false,  3974 },  // Houndmaster Loksey
    { 189, 2, 0, false,  6487 },  // Arcanist Doan
    { 189, 2, 0, false, 61983 },  // Brother Wystan
    // Armory
    { 189, 3, 0, false,  3975 },  // Herod
    { 189, 3, 0, false, 61982 },  // Armory Quartermaster Daghelm
    // Cathedral
    { 189, 4, 0, false,  4542 },  // High Inquisitor Fairbanks
    { 189, 4, 0, false,  3976 },  // Scarlet Commander Mograine
    { 189, 4, 0, false,  3977 },  // High Inquisitor Whitemane

    // Razorfen Downs (map 129)
    { 129, 0, 0, false,  7357 },  // Mordresh Fire Eye
    { 129, 0, 0, false,  8567 },  // Glutton
    { 129, 0, 0, false, 62679 },  // Death Prophet Rakameg
    { 129, 0, 0, false,  7358 },  // Amnennar the Coldbringer
    { 129, 0, 0, true,  7355 },  // Tuten'kash (optional)
    { 129, 0, 0, true,  7356 },  // Plaguemaw the Rotting (optional)

    // Uldaman (map 70)
    {  70, 0, 0, false,  6910 },  // Revelosh
    {  70, 0, 0, false,  7228 },  // Ironaya
    {  70, 0, 0, false,  7023 },  // Obsidian Sentinel
    {  70, 0, 0, false,  7206 },  // Ancient Stone Keeper
    {  70, 0, 0, false,  7291 },  // Galgann Firehammer
    {  70, 0, 0, false,  4854 },  // Grimlok
    {  70, 0, 0, false,  2748 },  // Archaedas
    {  70, 0, 0, true,  6906 },  // Baelog (optional)
    {  70, 0, 0, true,  6907 },  // Eric "The Swift" (optional)
    {  70, 0, 0, true,  6908 },  // Olaf (optional)

    // Zul'Farrak (map 209)
    { 209, 0, 0, false,  8127 },  // Antu'sul
    { 209, 0, 0, false,  7272 },  // Theka the Martyr
    { 209, 0, 0, false,  7271 },  // Witch Doctor Zum'rah
    { 209, 0, 0, false,  7795 },  // Hydromancer Velratha
    { 209, 0, 0, false, 62495 },  // Zel'jeb the Ancient
    { 209, 0, 0, false,  7796 },  // Nekrum Gutchewer
    { 209, 0, 0, false,  7275 },  // Shadowpriest Sezz'ziz
    { 209, 0, 0, false,  7797 },  // Ruuzlu
    { 209, 0, 0, false,  7267 },  // Chief Ukorz Sandscalp
    { 209, 0, 0, true,  7604 },  // Sergeant Bly (optional)
    { 209, 0, 0, true, 62498 },  // Champion Razjal the Quick (optional)
    { 209, 0, 0, true, 62496 },  // Kath'zen the Brutal (optional)
    { 209, 0, 0, true, 62497 },  // Juthza the Cunning (optional)

    // Maraudon (map 349)
    { 349, 0, 0, false, 13282 },  // Noxxion
    { 349, 0, 0, false, 12258 },  // Razorlash
    { 349, 0, 0, false, 12236 },  // Lord Vyletongue
    { 349, 0, 0, false, 12225 },  // Celebras the Cursed
    { 349, 0, 0, false, 12203 },  // Landslide
    { 349, 0, 0, false, 13601 },  // Tinkerer Gizlock
    { 349, 0, 0, false, 13596 },  // Rotgrip
    { 349, 0, 0, false, 12201 },  // Princess Theradras

    // Sunken Temple (map 109)
    { 109, 0, 0, false,  5712 },  // Zolo
    { 109, 0, 0, false,  5713 },  // Gasher
    { 109, 0, 0, false,  5714 },  // Loro
    { 109, 0, 0, false,  5715 },  // Hukku
    { 109, 0, 0, false,  5716 },  // Zul'Lor
    { 109, 0, 0, false,  5717 },  // Mijan
    { 109, 0, 0, false,  5710 },  // Jammal'an the Prophet
    { 109, 0, 0, false,  5711 },  // Ogom the Wretched
    { 109, 0, 0, false,  5721 },  // Dreamscythe
    { 109, 0, 0, false,  5720 },  // Weaver
    { 109, 0, 0, false,  5719 },  // Morphaz
    { 109, 0, 0, false,  5722 },  // Hazzas
    { 109, 0, 0, false,  5709 },  // Shade of Eranikus
    { 109, 0, 0, true,  8580 },  // Atal'alarion (optional)

    // Blackrock Depths (map 230)
    { 230, 0, 0, false,  9018 },  // High Interrogator Gerstahn
    { 230, 0, 0, false,  9319 },  // Houndmaster Grebmar
    { 230, 0, 0, false,  9025 },  // Lord Roccor
    { 230, 0, 1, false,  9027 },  // Ring of Law champion
    { 230, 0, 1, false,  9028 },  // Ring of Law champion
    { 230, 0, 1, false,  9029 },  // Ring of Law champion
    { 230, 0, 1, false,  9030 },  // Ring of Law champion
    { 230, 0, 1, false,  9031 },  // Ring of Law champion
    { 230, 0, 1, false,  9032 },  // Ring of Law champion
    { 230, 0, 1, false, 16059 },  // Ring of Law champion
    { 230, 0, 0, false,  9024 },  // Pyromancer Loregrain
    { 230, 0, 0, false,  9017 },  // Lord Incendius
    { 230, 0, 0, false,  9041 },  // Warder Stilgiss
    { 230, 0, 0, false,  9042 },  // Verek
    { 230, 0, 0, false,  9056 },  // Fineous Darkvire
    { 230, 0, 0, false,  9016 },  // Bael'Gar
    { 230, 0, 0, false,  9033 },  // General Angerforge
    { 230, 0, 0, false,  8983 },  // Golem Lord Argelmach
    { 230, 0, 0, false,  9156 },  // Ambassador Flamelash
    { 230, 0, 0, false,  9938 },  // Magmus
    { 230, 0, 0, false,  9019 },  // Emperor Dagran Thaurissan
    { 230, 0, 0, true,  9502 },  // Phalanx (optional)
    { 230, 0, 0, true,  9499 },  // Plugger Spazzring (optional)
    { 230, 0, 0, true,  9537 },  // Hurley Blackbreath (optional)
    { 230, 0, 0, true,  9543 },  // Ribbly Screwspigot (optional)
    { 230, 0, 0, true,  8929 },  // Princess Moira Bronzebeard (optional)
    { 230, 0, 0, true,  9034 },  // Hate'rel (optional)
    { 230, 0, 0, true,  9035 },  // Anger'rel (optional)
    { 230, 0, 0, true,  9036 },  // Vile'rel (optional)
    { 230, 0, 0, true,  9037 },  // Gloom'rel (optional)
    { 230, 0, 0, true,  9038 },  // Seeth'rel (optional)
    { 230, 0, 0, true,  9039 },  // Doom'rel (optional)
    { 230, 0, 0, true,  9040 },  // Dope'rel (optional)

    // Blackrock Spire (map 229)
    // Lower
    { 229, 1, 0, false,  9196 },  // Highlord Omokk
    { 229, 1, 0, false,  9236 },  // Shadow Hunter Vosh'gajin
    { 229, 1, 0, false,  9237 },  // War Master Voone
    { 229, 1, 0, false, 10596 },  // Mother Smolderweb
    { 229, 1, 0, false,  9736 },  // Quartermaster Zigris
    { 229, 1, 0, false, 10220 },  // Halycon
    { 229, 1, 0, false, 10268 },  // Gizrul the Slavener
    { 229, 1, 0, false,  9568 },  // Overlord Wyrmthalak
    // Upper
    { 229, 2, 0, false,  9816 },  // Pyroguard Emberseer
    { 229, 2, 0, false, 10899 },  // Goraluk Anvilcrack
    { 229, 2, 0, false, 10339 },  // Gyth
    { 229, 2, 0, false, 10429 },  // Warchief Rend Blackhand
    { 229, 2, 0, false, 10430 },  // The Beast
    { 229, 2, 0, false, 10363 },  // General Drakkisath
    { 229, 2, 0, true, 10264 },  // Solakar Flamewreath (optional)

    // Scholomance (map 289)
    { 289, 0, 0, false, 11622 },  // Rattlegore
    { 289, 0, 0, false, 10503 },  // Jandice Barov
    { 289, 0, 0, false, 10508 },  // Ras Frostwhisper
    { 289, 0, 0, false, 10505 },  // Instructor Malicia
    { 289, 0, 0, false, 11261 },  // Doctor Theolen Krastinov
    { 289, 0, 0, false, 10901 },  // Lorekeeper Polkelt
    { 289, 0, 0, false, 10507 },  // The Ravenian
    { 289, 0, 0, false, 10504 },  // Lord Alexei Barov
    { 289, 0, 0, false, 10502 },  // Lady Illucia Barov
    { 289, 0, 0, false,  1853 },  // Darkmaster Gandling
    { 289, 0, 0, true, 10432 },  // Vectus (optional)
    { 289, 0, 0, true, 10433 },  // Marduk Blackpool (optional)

    // Stratholme (map 329)
    // Live
    { 329, 1, 0, false, 10808 },  // Timmy the Cruel
    { 329, 1, 0, false, 11032 },  // Malor the Zealous
    { 329, 1, 0, false, 10997 },  // Cannon Master Willey
    { 329, 1, 0, false, 10811 },  // Archivist Galford
    { 329, 1, 1, false, 10813 },  // Balnazzar
    { 329, 1, 1, false, 10812 },  // Grand Crusader Dathrohan
    { 329, 1, 0, true, 10558 },  // Hearthsinger Forresten (optional)
    { 329, 1, 0, true, 10516 },  // The Unforgiven (optional)
    // Undead
    { 329, 2, 0, false, 10435 },  // Magistrate Barthilas
    { 329, 2, 0, false, 10436 },  // Baroness Anastari
    { 329, 2, 0, false, 10437 },  // Nerub'enkan
    { 329, 2, 0, false, 10438 },  // Maleki the Pallid
    { 329, 2, 0, false, 10439 },  // Ramstein the Gorger
    { 329, 2, 0, false, 10440 },  // Baron Rivendare

    // Dire Maul (map 429)
    // East
    { 429, 1, 0, false, 11490 },  // Zevrim Thornhoof
    { 429, 1, 0, false, 13280 },  // Hydrospawn
    { 429, 1, 0, false, 14327 },  // Lethtendris
    { 429, 1, 0, false, 11492 },  // Alzzin the Wildshaper
    { 429, 1, 0, true, 14354 },  // Pusillin (optional)
    // West
    { 429, 2, 0, false, 11489 },  // Tendris Warpwood
    { 429, 2, 0, false, 11488 },  // Illyanna Ravenoak
    { 429, 2, 0, false, 11487 },  // Magister Kalendris
    { 429, 2, 0, false, 11496 },  // Immol'thar
    { 429, 2, 0, false, 11486 },  // Prince Tortheldrin
    // North
    { 429, 3, 0, false, 11501 },  // King Gordok
    { 429, 3, 0, true, 14326 },  // Guard Mol'dar (optional)
    { 429, 3, 0, true, 14322 },  // Stomper Kreeg (optional)
    { 429, 3, 0, true, 14321 },  // Guard Fengus (optional)
    { 429, 3, 0, true, 14323 },  // Guard Slip'kik (optional)
    { 429, 3, 0, true, 14325 },  // Captain Kromcrush (optional)

    // Crescent Grove (map 802)
    { 802, 0, 0, false, 92107 },  // Grovetender Engryss
    { 802, 0, 0, false, 92109 },  // Keeper Ranathos
    { 802, 0, 0, false, 92108 },  // High Priestess A'lathea
    { 802, 0, 0, false, 92111 },  // Fenektis the Deceiver
    { 802, 0, 0, false, 92110 },  // Master Raxxieth

    // Dragonmaw Retreat (map 816)
    { 816, 0, 0, false, 62057 },  // Gowlfang
    { 816, 0, 0, false, 62066 },  // Cavernweb Broodmother
    { 816, 0, 0, false, 62067 },  // Web Master Torkon
    { 816, 0, 0, false, 62071 },  // Garlok Flamekeeper
    { 816, 0, 0, false, 62069 },  // Halgan Redbrand
    { 816, 0, 0, false, 62068 },  // Slagfist Destroyer
    { 816, 0, 0, false, 62070 },  // Overlord Blackheart
    { 816, 0, 0, false, 62038 },  // Elder Hollowblood
    { 816, 0, 0, false, 62072 },  // Searistrasz
    { 816, 0, 0, false, 62037 },  // Zuluhed the Whacked

    // Stormwrought Ruins (map 818)
    { 818, 0, 0, false, 62547 },  // Dagar the Glutton
    { 818, 0, 0, false, 62548 },  // Oronok Torn-Heart
    { 818, 0, 0, false, 62549 },  // Duke Balor the IV
    { 818, 0, 0, false, 62552 },  // Librarian Theodorus
    { 818, 0, 0, false, 62551 },  // Chieftain Stormsong
    { 818, 0, 0, false, 62550 },  // Deathlord Tidebane
    { 818, 0, 0, false, 62652 },  // Subjugator Halthas Shadecrest
    { 818, 0, 0, false, 62664 },  // Mycellakos
    { 818, 0, 0, false, 62661 },  // Eldermaw the Primordial
    { 818, 0, 0, false, 62665 },  // Lady Drazare
    { 818, 0, 0, false, 62671 },  // Ighal'for
    { 818, 0, 0, false, 62673 },  // Mergothid
    { 818, 0, 0, true, 62670 },  // Remains of the Innocent (optional)

    // Windhorn Canyon (map 820)
    { 820, 0, 0, false, 62778 },  // Ahgk'tos the Pure
    { 820, 0, 0, false, 62779 },  // Pathun Duskhide
    { 820, 0, 0, false, 62783 },  // Ambassador Vortalus
    { 820, 0, 0, false, 62784 },  // Walgan Bloodcaller
    { 820, 0, 0, false, 62780 },  // Bonespeaker Narlgom
    { 820, 0, 0, false, 62781 },  // Prophet Stormhoof
    { 820, 0, 0, false, 62782 },  // Chieftain Shalk Blackwind

    // Frostmane Hollow (map 822)
    { 822, 0, 0, false, 63129 },  // Kan'za the Seer
    { 822, 0, 0, false, 63131 },  // Battlemaster Ubukaz
    { 822, 0, 0, false, 63133 },  // Tan'sha the Sleek
    { 822, 0, 0, false, 63130 },  // Hailar the Frigid
    { 822, 0, 0, true, 63132 },  // Handler Oboka (optional)

    // Gilneas City (map 815)
    { 815, 0, 0, false, 61419 },  // Matthias Holtz
    { 815, 0, 0, false, 61420 },  // Packmaster Ragetooth
    { 815, 0, 0, false, 61421 },  // Judge Sutherland
    { 815, 0, 0, false, 61422 },  // Dustivan Blackcowl
    { 815, 0, 0, false, 61423 },  // Marshal Magnus Greystone
    { 815, 0, 0, false, 61605 },  // Horsemaster Levvin
    { 815, 0, 0, false, 61263 },  // Regent-Lady Celia Harlow
    { 815, 0, 0, false, 61264 },  // Regent-Lord Mortimer Harlow
    { 815, 0, 0, false, 61418 },  // Genn Greymane

    // Hateforge Quarry (map 808)
    { 808, 0, 0, false, 60735 },  // High Foreman Bargul Blackhammer
    { 808, 0, 0, false, 60736 },  // Engineer Figgles
    { 808, 0, 0, false, 60829 },  // Corrosis
    { 808, 0, 0, false, 60734 },  // Hatereaver Annihilator
    { 808, 0, 0, false, 60737 },  // Har'gesh Doomcaller

    // Karazhan Crypt (map 800)
    { 800, 0, 0, false, 91920 },  // Marrowspike
    { 800, 0, 0, false, 91929 },  // Hivaxxis
    { 800, 0, 0, false, 91917 },  // Corpsemuncher
    { 800, 0, 0, false, 92935 },  // Guard Captain Gort
    { 800, 0, 0, false, 91916 },  // Archlich Enkhraz
    { 800, 0, 0, false, 91919 },  // Commander Andreon
    { 800, 0, 0, false, 91928 },  // Alarus

    // The Black Morass (map 269)
    { 269, 0, 0, false, 65113 },  // Chronar
    { 269, 0, 0, false, 61575 },  // Epidamu
    { 269, 0, 0, false, 61316 },  // Drifting Avatar of Sand
    { 269, 0, 0, false, 65116 },  // Time-Lord Epochronos
    { 269, 0, 0, false, 65124 },  // Mossheart
    { 269, 0, 0, false, 65122 },  // Rotmaw
    { 269, 0, 0, false, 65125 },  // Antnormi

    // Stormwind Vault (map 35)
    {  35, 0, 0, false, 80853 },  // Aszosh Grimflame
    {  35, 0, 0, false, 80852 },  // Tham'Grarr
    {  35, 0, 0, false, 80850 },  // Black Bride
    {  35, 0, 0, false, 80851 },  // Volkan Cruelblade
    {  35, 0, 0, false, 80854 },  // Damian
    {  35, 0, 0, false, 93107 },  // Arc'tiras
};
