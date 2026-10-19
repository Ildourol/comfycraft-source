#pragma once

#include <unordered_set>
#include "Config/Config.h"
#include "Talentspec.h"
#include "SharedDefines.h"
#include "SystemConfig.h"

class Player;
class PlayerbotMgr;
class ChatHandler;

#if PLATFORM == PLATFORM_WINDOWS
inline std::string _D_AIPLAYERBOT_CONFIG = "aiplayerbot.conf";
#else
inline std::string _D_AIPLAYERBOT_CONFIG = SYSCONFDIR "aiplayerbot.conf";
#endif

enum class BotCheatMask : uint32
{
    none = 0,
    taxi = 1 << 0,
    gold = 1 << 1,
    health = 1 << 2,
    mana = 1 << 3,
    power = 1 << 4,
    item = 1 << 5,
    cooldown = 1 << 6,
    repair = 1 << 7,
    movespeed = 1 << 8,
    attackspeed = 1 << 9,
    breath = 1 << 10,
    glyph = 1 << 11,
    quest = 1 << 12,    
    maxMask = 1 << 13
};

enum class BotAutoLogin : uint32
{
    DISABLED = 0,
    LOGIN_ALL_WITH_MASTER = 1,
    LOGIN_ONLY_ALWAYS_ACTIVE = 2
};

enum class BotSelfBotLevel : uint32
{
    DISABLED = 0,
    GM_ONLY = 1,
    ACTIVE_BY_COMMAND = 2,
    ALWAYS_ALLOWED = 3,
    ACTIVE_BY_LOGIN = 4, 
    ALWAYS_ACTIVE = 5
};

enum class BotAlwaysOnline : uint32
{
    DISABLED = 0,
    ACTIVE = 1,
    DISABLED_BY_COMMAND = 2
};

enum class BotLoginCriteriaType : uint8
{
    RACECLASS = 0,
    LEVEL = 1,
    RANGE_TO_PLAYER = 2,
    MAX_LOGIN_CRITERIA = 3
};

#define MAX_GEAR_PROGRESSION_LEVEL 6

class ConfigAccess
{
private:
    std::string m_filename;
    std::string m_envVarPrefix;
    std::unordered_map<std::string, std::string> m_entries; // keys are converted to lower case.  values cannot be.

public:
    std::vector<std::string> GetValues(const std::string& name) const;
    std::mutex m_configLock;
};

struct ParsedUrl {
    std::string hostname;
    std::string path;
    int port;
    bool https;
};

//GlyphPrioritySpecMap[specId][level] = {{glyphItemId, prereqTalentSpell}};
using GlyphPriority = std::pair<uint32, uint32>;
using GlyphPriorityList = std::vector<GlyphPriority>;
using GlyphPriorityLevelMap = std::unordered_map<uint32, GlyphPriorityList>;
using GlyphPrioritySpecMap = std::unordered_map<uint32, GlyphPriorityLevelMap>;

class PlayerbotAIConfig
{
public:
    PlayerbotAIConfig();
    static PlayerbotAIConfig& instance()
    {
        static PlayerbotAIConfig instance;
        return instance;
    }

public:
    bool Initialize();
    bool IsInRandomAccountList(uint32 id);
    bool IsFreeAltBot(uint32 guid);
    bool IsFreeAltBot(Player* player) {return IsFreeAltBot(player->GetGUIDLow());}
    bool IsInRandomQuestItemList(uint32 id);
	bool IsInPvpProhibitedZone(uint32 id);

    bool enabled;
    // mod-playerbots caps how many bots one account may .add; the dungeon-clear
    // test runner reads it to size its parties. Zero keeps that gate open, the
    // same default mod-playerbots ships.
    uint32 maxAddedBots = 0;
    // mod-playerbots rest threshold: mana percent above which a bot counts as
    // rested. Their shipped default.
    uint32 highMana = 65;
    // mod-playerbots gear ceiling for auto-equip; 0 is their shipped default
    // and means unlimited, which is also the only behaviour this tree has.
    uint32 autoGearScoreLimit = 0;
    uint32 autoGearQualityLimit = 0;  // like autoGearScoreLimit: 0 = no cap (mod-dungeon-clear reads it for its test sidecar)
    bool allowGuildBots;
    bool allowMultiAccountAltBots;
    // How long a companion waits for an owner who logged out before it is
    // deleted instead of brought back. See CompanionOwnership::ParkAll.
    uint32 companionParkMinutes;
    uint32 globalCoolDown, reactDelay, maxWaitForMove, expireActionTime, dispelAuraDuration, passiveDelay, repeatDelay,
        errorDelay, rpgDelay, sitDelay, returnDelay, lootDelay;
    float sightDistance, spellDistance, reactDistance, grindDistance, lootDistance, groupMemberLootDistance, groupMemberLootDistanceWithActiveMaster,
        gatheringDistance, groupMemberGatheringDistance, groupMemberGatheringDistanceWithActiveMaster, shootDistance,
        fleeDistance, tooCloseDistance, meleeDistance, followDistance, raidFollowDistance, wanderMinDistance, wanderMaxDistance, whisperDistance, contactDistance,
        aoeRadius, rpgDistance, targetPosRecalcDistance, farDistance, healDistance, healDistanceBg, aggroDistance, proximityDistance, maxFreeMoveDistance, freeMoveDelay, walkDistance;
    uint32 criticalHealth, lowHealth, mediumHealth, almostFullHealth;
    uint32 lowMana, mediumMana;

    // When a bot sits down to eat or drink, as a percentage of its maximum.
    // NOT lowMana/lowHealth: those gate spell choices - when to stop casting
    // the expensive rank - and moving them changes rotations rather than
    // mealtimes. These are near full on purpose. Eating and drinking are
    // out-of-combat only, so the seconds are free, and a healer that starts a
    // pull two thirds full runs dry before it ends.
    uint32 drinkAtMana, eatAtHealth;

    // How fast a bot eating or drinking on the ITEM CHEAT gets it back, as a
    // percentage of its maximum per second. Spell.dbc ships 2 for both Food
    // (24005) and Drink (24355), so a full top up takes 50 seconds and the
    // auras' own 25s/30s durations cap one sit at half a bar. The number is
    // only ever read as m_modifier.m_amount on the applied aura, once per tick
    // (SpellAuras.cpp, the OBS_MOD_HEALTH and OBS_MOD_MANA cases), and
    // Aura::GetModifier() is non-const - so this is overridden on the aura
    // after it lands rather than by editing the DBC, and needs no core change.
    //
    // Only affects bots the server feeds: companions and random bots hold
    // BotCheatMask::item, the owner's own alts run with .bot add do not and
    // eat real food at the real rate.
    //
    // Raising it shortens the sit rather than lengthening the meal - the sit
    // duration is derived from this same number in DrinkDurationValue and
    // EatDurationValue, so at 8 a full top up is 12.5 seconds, not 50.
    uint32 botEatDrinkPercentPerSecond;

    // Seconds a companion's party must be clear of combat before the companion
    // gets back up where its owner is standing. Counted from the later of the
    // death and the end of the fight, so dying at the start of a long pull does
    // not start the clock until the pull is over. 0 disables the whole
    // behaviour and leaves companions on the upstream corpse-run path.
    uint32 companionReviveDelay;

    // Seconds between two `summon` orders to the same companion. Summon is the
    // way out for a stuck companion, not a teleport to use in play.
    uint32 companionSummonCooldown;
    float looseFollowDistance;

    uint32 openGoSpell;
    bool randomBotAutologin;
    BotAutoLogin botAutologin;
    std::string randomBotMapsAsString;
    std::vector<uint32> randomBotMaps;
    std::list<uint32> randomBotQuestItems;
    std::list<uint32> randomBotAccounts;
    std::unordered_set<uint32> nonRandomBotAccounts;
    std::list<uint32> randomBotSpellIds;
    std::list<uint32> randomBotQuestIds;
    std::list<uint32> immuneSpellIds;
    std::list<std::pair<uint32, uint32>> freeAltBots;
    std::list<std::string> toggleAlwaysOnlineAccounts;
    std::list<std::string> toggleAlwaysOnlineChars;
    bool enableRandomTeleports;
    bool enableMinimalMove;
    uint32 randomBotTeleportDistance;
    bool randomBotTeleportNearPlayer;
    uint32 transportTeleportType;
    uint32 randomBotTeleportNearPlayerMaxAmount;
    float randomBotTeleportNearPlayerMaxAmountRadius;
    uint32 randomBotTeleportMinInterval, randomBotTeleportMaxInterval;
    uint32 randomGearMaxLevel;
    uint32 randomGearMaxDiff;
    bool randomGearUpgradeEnabled;
    bool randomGearTabards;
    bool randomGearTabardsReplaceGuild;
    bool randomGearTabardsUnobtainable;
    float randomGearTabardsChance;
    std::list<uint32> randomGearBlacklist;
    std::list<uint32> randomGearWhitelist;
    bool randomGearProgression;
    float randomGearLoweringChance;
    bool rollBadItemsWithPlayer;
    float randomBotMaxLevelChance;
    float randomBotRpgChance;
    float usePotionChance;
    float attackEmoteChance;
    bool randomBotAutoCreate;
    uint32 minRandomBots, maxRandomBots;
    uint32 randomBotUpdateInterval, randomBotCountChangeMinInterval, randomBotCountChangeMaxInterval;
    bool randomBotTimedLogout, randomBotTimedOffline;
    uint32 minRandomBotInWorldTime, maxRandomBotInWorldTime;
    uint32 minRandomBotRandomizeTime, maxRandomBotRandomizeTime;
    uint32 minRandomBotChangeStrategyTime, maxRandomBotChangeStrategyTime;
    uint32 minRandomBotReviveTime, maxRandomBotReviveTime;
    uint32 minRandomBotPvpTime, maxRandomBotPvpTime;
    uint32 randomBotsMaxLoginsPerInterval;
    uint32 randomBotsPerInterval;
    uint32 minRandomBotsPriceChangeInterval, maxRandomBotsPriceChangeInterval;
    //Auction house settings
    bool shouldQueryAHListingsOutsideOfAH;
    std::list<uint32> ahOverVendorItemIds;
    std::list<uint32> vendorOverAHItemIds;
    bool botCheckAllAuctionListings;
    bool botsSaveEpics;
    //
    bool randomBotJoinLfg;
    bool logRandomBotJoinLfg;
    bool randomBotJoinBG;
    bool randomBotAutoJoinBG;
    uint32 randomBotBracketCount;
    bool randomBotLoginAtStartup;
    uint32 randomBotTeleLevel;
    bool logInGroupOnly, logValuesPerTick;
    bool fleeingEnabled;
    bool summonAtInnkeepersEnabled;
    std::string combatStrategies, nonCombatStrategies, reactStrategies, deadStrategies;
    std::string randomBotCombatStrategies, randomBotNonCombatStrategies, randomBotReactStrategies, randomBotDeadStrategies;
    // Applied to companions after the strings above, so a hire can be made
    // quiet (+silent) without touching the owner's own alts or the random
    // population. See AiFactory::AddDefault*Strategies.
    std::string companionCombatStrategies, companionNonCombatStrategies;
    // 0: a companion opens corpses so group rolls start and the money splits,
    // but picks up no item and passes every roll. See StoreLootAction and
    // RollAction::CalculateRollVote.
    bool companionTakesLoot;
    // Yards from the owner a companion lands at on arrival, at a random
    // bearing. See RandomPlayerbotMgr::LoginFreeBots.
    float companionArrivalDistance;
    uint32 randomBotMinLevel, randomBotMaxLevel;
    float randomChangeMultiplier;
    std::string premadeLevelSpec[MAX_CLASSES][10][91]; //lvl 10 - 100
    uint32 classRaceProbabilityTotal;
    uint32 classRaceProbability[MAX_CLASSES][MAX_RACES];
    bool useFixedClassRaceCounts;
    using ClassRacePair = std::pair<uint8, uint8>;
    std::map<ClassRacePair, uint32> fixedClassRaceCounts;
    uint32 levelProbability[PLAYER_STRONG_MAX_LEVEL + 1];   // see levelBucket in PlayerbotLoginMgr.h
    ClassSpecs classSpecs[MAX_CLASSES];
    GlyphPrioritySpecMap glyphPriorityMap[MAX_CLASSES];
    bool gearProgressionSystemEnabled;
    uint32 gearProgressionSystemItemLevels[MAX_GEAR_PROGRESSION_LEVEL][2];
    int32 gearProgressionSystemItems[MAX_GEAR_PROGRESSION_LEVEL][MAX_CLASSES][4][SLOT_EMPTY];
    std::string commandPrefix, commandSeparator;
    std::string randomBotAccountPrefix;
    // Character names that stay online and are never teleported away.
    // Resolved to guids by RandomPlayerbotMgr, which has the database.
    std::list<std::string> pinnedBotNames;

    // Bots per team a battleground may fill while no real player is queuing for
    // that bracket, keyed by BattleGroundTypeId. Deliberately below the
    // template maximum so a player who queues later drops into the running
    // match rather than starting a second one. Empty means no cap.
    std::map<uint32, uint32> bgBotTeamCap;

    // -1 when the type is not listed at all, otherwise the configured number.
    // Zero is meaningful: it switches the battleground off for bots entirely,
    // which is what a map disabled by the client patch needs.
    // How many battleground instances of one type and bracket bots may keep
    // running at once while nobody real is waiting. Was hardcoded to 1, which
    // took Warsong from 400 matches a day down to 12.
    uint32 bgMaxInstancesPerBracket;
    int32 GetBgBotTeamCap(uint32 bgTypeId) const
    {
        auto it = bgBotTeamCap.find(bgTypeId);
        return it == bgBotTeamCap.end() ? -1 : (int32)it->second;
    }
    uint32 randomBotAccountCount;
    bool deleteRandomBotAccounts;
    uint32 randomBotGuildCount;
    bool deleteRandomBotGuilds;
    uint32 randomBotArenaTeamCount;
    bool deleteRandomBotArenaTeams;
    std::list<uint32> randomBotArenaTeams;
	bool RandombotsWalkingRPG;
	bool RandombotsWalkingRPGInDoors;
    bool boostFollow;
    bool turnInRpg;
    bool globalSoundEffects;
    bool shareTargets;
    std::list<uint32> randomBotGuilds;
	std::list<uint32> pvpProhibitedZoneIds;
    bool enableGreet;
    bool randomBotShowHelmet;
    bool randomBotShowCloak;
    bool disableRandomLevels;
    bool instantRandomize;
    bool gearscorecheck;
    int32 levelCheck;
	bool randomBotPreQuests;
    float playerbotsXPrate;
    bool disableBotOptimizations;
    bool disableActivityPriorities;
    bool forceActiveWhenNearPlayer;
    bool limitCombatActivity;
    bool guildOrderAlwaysActive;
    uint32 botActiveAlone;
    uint32 diffWithPlayer;
    uint32 diffEmpty;
    uint32 minEnchantingBotLevel;
    uint32 randombotStartingLevel;
    bool randomBotSayWithoutMaster;
    bool randomBotInvitePlayer;
    bool randomBotGroupNearby;
    bool randomBotRaidNearby;
    bool randomBotGuildNearby;
    bool randomBotFormGuild;
    bool randomBotRandomPassword;
    bool inviteChat;
    // Whisper a real person who invites a bot one line telling them
    // "help" exists. Off makes the join silent again.
    bool groupJoinHelpHint;
    bool botsSilent;
    // Opt-in diagnostic logging: when true, [BOT] log lines and per-bot action
    // log files (logs/bots/<name>_acc<id>_<timestamp>.log) are emitted. Default
    // off so production servers don't pay disk I/O / branch overhead.
    bool enableActionLog;
    // Filename (relative to LogsDir) for the bot subsystem log. When set,
    // all sLog calls from bot .cpp files are redirected there instead of
    // writing to the main server log. Default: "bots.log". Empty = disabled.
    std::string botLogFile;
    bool enableOffSpecStrategies;
    bool useWanderAsDefaultFollowStrategy;
    std::string defaultFormation;

    uint32 guildMaxBotLimit;

    bool enableBroadcasts;
    uint32 broadcastChanceMaxValue;

    uint32 broadcastToGuildGlobalChance;
    uint32 broadcastToWorldGlobalChance;
    uint32 broadcastToGeneralGlobalChance;
    uint32 broadcastToTradeGlobalChance;
    uint32 broadcastToLFGGlobalChance;
    uint32 broadcastToLocalDefenseGlobalChance;
    uint32 broadcastToWorldDefenseGlobalChance;
    uint32 broadcastToGuildRecruitmentGlobalChance;
    uint32 broadcastToSayGlobalChance;
    uint32 broadcastToYellGlobalChance;

    uint32 broadcastChanceLootingItemPoor;
    uint32 broadcastChanceLootingItemNormal;
    uint32 broadcastChanceLootingItemUncommon;
    uint32 broadcastChanceLootingItemRare;
    uint32 broadcastChanceLootingItemEpic;
    uint32 broadcastChanceLootingItemLegendary;
    uint32 broadcastChanceLootingItemArtifact;

    uint32 broadcastChanceQuestAccepted;
    uint32 broadcastChanceQuestUpdateObjectiveCompleted;
    uint32 broadcastChanceQuestUpdateObjectiveProgress;
    uint32 broadcastChanceQuestUpdateFailedTimer;
    uint32 broadcastChanceQuestUpdateComplete;
    uint32 broadcastChanceQuestTurnedIn;

    uint32 broadcastChanceKillNormal;
    uint32 broadcastChanceKillElite;
    uint32 broadcastChanceKillRareelite;
    uint32 broadcastChanceKillWorldboss;
    uint32 broadcastChanceKillRare;
    uint32 broadcastChanceKillUnknown;
    uint32 broadcastChanceKillPet;
    uint32 broadcastChanceKillPlayer;

    uint32 broadcastChanceLevelupGeneric;
    uint32 broadcastChanceLevelupTenX;
    uint32 broadcastChanceLevelupMaxLevel;

    uint32 broadcastChanceSuggestInstance;
    uint32 broadcastChanceSuggestQuest;
    uint32 broadcastChanceSuggestGrindMaterials;
    uint32 broadcastChanceSuggestGrindReputation;
    uint32 broadcastChanceSuggestSell;
    uint32 broadcastChanceSuggestSomething;

    uint32 broadcastChanceSuggestSomethingToxic;

    uint32 broadcastChanceSuggestToxicLinks;
    std::string toxicLinksPrefix;
    uint32 toxicLinksRepliesChance;

    uint32 broadcastChanceSuggestThunderfury;
    uint32 thunderfuryRepliesChance;

    uint32 broadcastChanceGuildManagement;

    uint32 guildRepliesRate;

    uint32 botAcceptDuelMinimumLevel;

    bool talentsInPublicNote;
    bool nonGmFreeSummon;

    BotSelfBotLevel selfBotLevel;
    uint32 iterationsPerTick;

    std::string autoPickReward;
    bool autoEquipUpgradeLoot;
    bool syncQuestWithPlayer;
    bool syncQuestForPlayer;
    std::string autoTrainSpells;
    std::string autoPickTalents;
    bool autoLearnTrainerSpells;
    bool autoLearnQuestSpells;
    bool autoLearnDroppedSpells;
    bool autoDoQuests;
    bool deferItemCache;
    bool syncLevelWithPlayers;
    uint32 syncLevelMaxAbove, syncLevelNoPlayer;
    bool syncAltLevelToMaster;
    uint32 tweakValue; //Debugging config
    float respawnModNeutral, respawnModHostile;
    uint32 respawnModThreshold, respawnModMax;
    bool respawnModForPlayerBots, respawnModForInstances;

    bool randomBotLoginWithPlayer;
    bool asyncBotLogin, preloadHolders;
    uint32 freeRoomForNonSpareBots;
    uint32 loginBotsNearPlayerRange;
    std::vector<std::string> defaultLoginCriteria;
    std::vector<std::vector<std::string>> loginCriteria;

    bool jumpInBg;
    bool jumpWithPlayer;
    bool jumpFollow;
    bool jumpChase;
    bool useKnockback;
    float jumpNoCombatChance;
    float jumpMeleeInCombatChance;
    float jumpRandomChance;
    float jumpInPlaceChance;
    float jumpBackwardChance;
    float jumpHeightLimit;
    float jumpVSpeed;
    float jumpHSpeed;

    std::mutex m_logMtx;

    std::list<std::string> allowedLogFiles;
    std::list<std::string> debugFilter;

    std::unordered_map <std::string, std::pair<FILE*, bool>> logFiles;

    uint32 botCheatMask = 0;
    uint32 rndBotCheatMask = 0;

    std::vector<std::string> BotCheatMaskName = { "taxi", "gold", "health", "mana", "power", "item", "cooldown", "repair", "movespeed", "attackspeed", "breath", "glyph", "quest", "maxMask" };

    struct worldBuff{
        uint32 spellId;
        uint32 factionId = 0;
        uint32 classId = 0;
        uint32 specId = 0;
        uint32 minLevel = 0;
        uint32 maxLevel = 0;
        uint32 eventId = 0;
    };

    std::vector<worldBuff> worldBuffs;

    int commandServerPort;
    bool perfMonEnabled;
    bool bExplicitDbStoreSave = false;

    //LM BEGIN
    std::string llmApiEndpoint, llmApiKey, llmApiJson, llmPrePrompt, llmPreRpgPrompt, llmPrompt, llmPostPrompt, llmResponseStartPattern, llmResponseEndPattern, llmResponseDeletePattern, llmResponseSplitPattern;
    uint32 llmEnabled, llmContextLength, llmBotToBotChatChance, llmGenerationTimeout, llmMaxSimultaniousGenerations, llmRpgAIChatChance;
    bool llmGlobalContext;
    ParsedUrl llmEndPointUrl;
    std::set<uint32> llmBlockedReplyChannels;
    //LM END

    // How far a bot will let a MOVING master get before it gives up on eating
    // or drinking and follows instead. The leash is this plus the follow
    // distance, so the default is a little over 20 yards: far enough that the
    // master shuffling about does not stand the bot up mid-meal, close enough
    // that walking away still collects it. Read from the config since
    // 2026-09-06 - the two conf lines documenting them had never been wired to
    // anything and editing them did nothing.
    float EatDrinkMinDistance = 20.0f;
    float EatDrinkMaxDistance = 1000.0f;

    std::string GetValue(std::string name);
    void SetValue(std::string name, std::string value);

    void loadFreeAltBotAccounts();
    // Bot NPCs stand themselves up from tw_char.bot_npc. See the note on the
    // implementation for why this does not use the "always online" event.
    void loadBotNpcs();

    std::string GetTimestampStr();

    bool hasLog(std::string fileName) { return std::find(allowedLogFiles.begin(), allowedLogFiles.end(), fileName) != allowedLogFiles.end(); };
    bool openLog(std::string fileName, char const* mode = "a", bool haslog = false);
    bool isLogOpen(std::string fileName) { auto it = logFiles.find(fileName); return it != logFiles.end() && it->second.second;}
    // Writes the line verbatim. Nearly every caller here hands over text it has
    // already assembled, and a percent sign anywhere in it - a bot name, a mob
    // name, an item name - used to be read as a conversion specifier. glibc
    // printed nonsense; the Microsoft runtime aborted the server.
    void log(std::string fileName, const char* line);

    // The formatting variant, for the callers that actually pass arguments.
#if defined(__GNUC__) || defined(__clang__)
    void logf(std::string fileName, const char* format, ...)
        __attribute__((format(printf, 3, 4)));
#else
    void logf(std::string fileName, const char* format, ...);
#endif

    void logEvent(PlayerbotAI* ai, std::string eventName, std::string info1 = "", std::string info2 = "");
    void logEvent(PlayerbotAI* ai, std::string eventName, ObjectGuid guid, std::string info2);

    bool CanLogAction(PlayerbotAI* ai, std::string actionName, bool isExecute, std::string lastActionName);

private:
    void LoadTalentSpecs();
    void LoadLLMDefaultPrompts(const std::string& fileName);

    Config config;
};

#define sPlayerbotAIConfig MaNGOS::Singleton<PlayerbotAIConfig>::Instance()

