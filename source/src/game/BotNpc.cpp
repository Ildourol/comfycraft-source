#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Map.h"
#include "Policies/SingletonImp.h"
#include "Util.h"

#include "Guild/Guild.h"
#include "Guild/GuildMgr.h"
#include "BotNpc.h"
#include "BotNpcIdle.h"

#include <cmath>

INSTANTIATE_SINGLETON_1(BotNpcMgr);

void BotNpcMgr::Load()
{
    m_entries.clear();
    m_spawns.clear();
    m_idle.clear();
    m_emote.clear();

    // Bumped on every load, which is what restarts a `custom` scene. BotNpc.h
    // has the reasoning.
    ++m_generation;

    std::unique_ptr<QueryResult> result(CharacterDatabase.Query(
        "SELECT id, name, race, gender, subname, look, gossip_menu_id, "
        "vendor_template_id, quest_giver_id, npc_flags "
        "FROM bot_npc"));

    if (!result)
    {
        sLog.outString(">> Bot NPCs: none.");
        sLog.outString();
        return;
    }

    do
    {
        Field* fields = result->Fetch();

        BotNpcEntry entry;
        entry.id            = fields[0].GetUInt32();
        entry.name          = fields[1].GetCppString();
        entry.race          = fields[2].GetUInt32();
        entry.gender        = fields[3].GetUInt32();
        entry.subname       = fields[4].GetCppString();
        entry.look          = fields[5].GetCppString();
        entry.gossipMenuId  = fields[6].GetUInt32();
        entry.vendorTemplateId = fields[7].GetUInt32();
        entry.questGiverId  = fields[8].GetUInt32();
        entry.npcFlags      = fields[9].GetUInt32();

        m_entries[entry.id] = entry;
    }
    while (result->NextRow());

    LoadSpawns();
    LoadIdleRules();

    sLog.outString(">> Bot NPCs: %u design(s), %u standing, %u with something to do, %u answering an emote.",
                   Count(), uint32(m_spawns.size()), uint32(m_idle.size()), uint32(m_emote.size()));
    sLog.outString();

    /*
     * DELIBERATELY NOT ApplyToLoggedIn() HERE. Load() runs inside
     * World::SetInitialWorldSettings, where nobody is logged in yet and
     * sObjectAccessor's player map is empty -- so the call would be a no-op
     * that looks like it is doing the work, and the real application would go
     * unnoticed as missing. Flags are put on in Player::LoadFromDB instead, on
     * the way in, which is the only path a bot NPC can arrive by.
     *
     * Reload() is the one that has live players to fix up, and says so.
     */
}

/*
 * THE BODIES. sql/custom/113; BotNpc.h has why they are rows of their own.
 *
 * A ROW FOR A DESIGN THAT IS GONE IS DROPPED AND SAID SO. The website deletes
 * spawns with their design, so this is hand-written SQL again; the character
 * logs in as an ordinary bot, which is what a body with no design is.
 */
void BotNpcMgr::LoadSpawns()
{
    std::unique_ptr<QueryResult> result(CharacterDatabase.Query(
        "SELECT character_guid, bot_npc_id, map, position_x, position_y, position_z, orientation, "
        "look_applied FROM bot_npc_spawn"));

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();

        BotNpcSpawn spawn;
        spawn.characterGuid = fields[0].GetUInt32();
        spawn.designId      = fields[1].GetUInt32();
        spawn.mapId         = fields[2].GetUInt32();
        spawn.x             = fields[3].GetFloat();
        spawn.y             = fields[4].GetFloat();
        spawn.z             = fields[5].GetFloat();
        spawn.o             = fields[6].GetFloat();
        spawn.lookApplied   = fields[7].GetCppString();

        if (!spawn.characterGuid)
            continue;

        if (m_entries.find(spawn.designId) == m_entries.end())
        {
            sLog.outErrorDb("Table `bot_npc_spawn` has character %u standing for bot NPC #%u, which does not exist. Skipped.",
                            spawn.characterGuid, spawn.designId);
            continue;
        }

        m_spawns[spawn.characterGuid] = spawn;
    }
    while (result->NextRow());
}

/*
 * WHAT EACH DESIGN DOES UNPROMPTED. Read here rather than in a manager of its
 * own so that `reload bot_npc` covers it: a rule is part of the design in every
 * sense a person cares about, and a second command to remember would be a
 * second command to forget.
 *
 * ONLY RULES THAT RUN ARE KEPT. A row that is switched off, or points at no
 * script, is the same as no row at all to everything downstream, so it is
 * dropped here rather than being tested on every tick of every map. That is
 * also what makes AnyIdleRules() true only on a server that is actually using
 * this.
 *
 * A ROW FOR A DESIGN THAT IS GONE IS DROPPED AND SAID SO. The website deletes
 * rules with their design, so this only happens to hand-written SQL -- but a
 * rule nothing can reach is exactly the kind of thing somebody spends an
 * evening on.
 */
void BotNpcMgr::LoadIdleRules()
{
    std::unique_ptr<QueryResult> result(CharacterDatabase.Query(
        "SELECT bot_npc_id, trigger_kind, enabled, period, radius, script_id, scene_id, emote_id "
        "FROM bot_npc_idle ORDER BY bot_npc_id, emote_id"));

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();

        uint32 const designId = fields[0].GetUInt32();
        std::string const kind = fields[1].GetCppString();

        /*
         * THE EMOTE KIND LEAVES EARLY, because it is a list rather than one of
         * the four indexed slots and shares almost nothing below this point:
         * no radius, no scene, and a cooldown that means the same thing on
         * every row instead of three different things. sql/custom/108.
         */
        if (kind == "emote")
        {
            BotNpcEmoteRule er;
            er.emoteId  = fields[7].GetUInt32();
            er.period   = fields[3].GetUInt32();
            er.scriptId = fields[5].GetUInt32();

            if (fields[2].GetUInt32() == 0 || !er.scriptId)
                continue;                                   // switched off, or points at nothing

            if (m_entries.find(designId) == m_entries.end())
            {
                sLog.outErrorDb("Table `bot_npc_idle` has an emote rule for bot NPC %u, which is not a design. Ignoring.", designId);
                continue;
            }

            // A rule with no emote answers nothing, and would otherwise sit in
            // the list being compared against every emote that ever arrives.
            if (!er.emoteId)
            {
                sLog.outErrorDb("Table `bot_npc_idle` has an emote rule for bot NPC %u with emote_id 0, which is not an emote. Ignoring.", designId);
                continue;
            }

            // Nothing throttles emotes in this core, so a zero here is a script
            // restarted several times a second by a held-down macro. Clamped
            // rather than refused, the same call the period below makes.
            if (er.period < 1)
                er.period = 1;

            m_emote[designId].push_back(er);
            continue;
        }

        BotNpcIdleTrigger trigger;
        if (kind == "loop")             trigger = BOT_NPC_IDLE_LOOP;
        else if (kind == "near")        trigger = BOT_NPC_IDLE_NEAR;
        else if (kind == "after_talk")  trigger = BOT_NPC_IDLE_AFTER_TALK;
        else if (kind == "custom")      trigger = BOT_NPC_IDLE_CUSTOM;
        else
        {
            sLog.outErrorDb("Table `bot_npc_idle` has trigger_kind '%s' for bot NPC %u, which is not one of loop, near, after_talk, custom, emote. Ignoring.",
                            kind.c_str(), designId);
            continue;
        }

        BotNpcIdleRule rule;
        rule.enabled  = fields[2].GetUInt32() != 0;
        rule.period   = fields[3].GetUInt32();
        rule.radius   = fields[4].GetFloat();

        /*
         * ONLY THE FIELD ITS TRIGGER MEANS, so exactly one of the two is ever
         * set and `Runs()` can ask about either without knowing which trigger
         * it belongs to. A `custom` row carrying a script id is a row somebody
         * wrote by hand for a different trigger and then changed; reading it
         * would start a timeline as though it were a scene.
         */
        if (trigger == BOT_NPC_IDLE_CUSTOM)
            rule.sceneId = fields[6].GetUInt32();
        else
            rule.scriptId = fields[5].GetUInt32();

        if (!rule.Runs())
            continue;

        if (m_entries.find(designId) == m_entries.end())
        {
            sLog.outErrorDb("Table `bot_npc_idle` has a rule for bot NPC %u, which is not a design. Ignoring.", designId);
            continue;
        }

        // A zero period would start the script again every single second, on
        // top of the copy still running. Clamped rather than refused: the
        // number is a person's estimate of how often, not a promise.
        if (rule.period < 1)
            rule.period = 1;

        if (trigger == BOT_NPC_IDLE_NEAR && rule.radius <= 0.0f)
        {
            sLog.outErrorDb("Table `bot_npc_idle` has a `near` rule for bot NPC %u with radius %g, which nobody can ever cross. Ignoring.",
                            designId, rule.radius);
            continue;
        }

        m_idle[designId][trigger] = rule;
    }
    while (result->NextRow());
}

bool BotNpc_Post(Unit const* unit, float& px, float& py, float& pz, float& po)
{
    if (!unit || !unit->GetMap())
        return false;

    px = unit->GetPositionX();
    py = unit->GetPositionY();
    pz = unit->GetPositionZ();
    po = unit->GetOrientation();

    if (unit->GetTypeId() == TYPEID_PLAYER)
    {
        Player const* pl = static_cast<Player const*>(unit);
        if (BotNpcSpawn const* post = sBotNpcMgr.GetSpawn(pl->GetGUIDLow()))
        {
            // Its row is for another map: there is no post here to measure
            // from, and a point on this map computed from those numbers would
            // be somewhere arbitrary.
            if (post->mapId != unit->GetMapId())
                return false;
            px = post->x; py = post->y; pz = post->z; po = post->o;
        }
    }
    else if (unit->GetTypeId() == TYPEID_UNIT)
    {
        static_cast<Creature const*>(unit)->GetRespawnCoord(px, py, pz, &po);
    }
    return true;
}

bool BotNpc_PointFromPost(Unit const* unit, float forward, float right,
                          float& x, float& y, float& z, float& o)
{
    float px, py, pz, po;
    if (!BotNpc_Post(unit, px, py, pz, po))
        return false;

    if (forward > BOT_NPC_POST_MAX_OFFSET)  forward = BOT_NPC_POST_MAX_OFFSET;
    if (forward < -BOT_NPC_POST_MAX_OFFSET) forward = -BOT_NPC_POST_MAX_OFFSET;
    if (right > BOT_NPC_POST_MAX_OFFSET)    right = BOT_NPC_POST_MAX_OFFSET;
    if (right < -BOT_NPC_POST_MAX_OFFSET)   right = -BOT_NPC_POST_MAX_OFFSET;

    x = px + forward * std::cos(po) + right * std::sin(po);
    y = py + forward * std::sin(po) - right * std::cos(po);
    o = po;

    float const ground = unit->GetMap()->GetHeight(x, y, pz + BOT_NPC_POST_GROUND_HEADROOM, true);
    z = ground > INVALID_HEIGHT ? ground : pz;
    return true;
}

bool BotNpc_OffsetFromPost(Unit const* unit, float wx, float wy, float& forward, float& right)
{
    float px, py, pz, po;
    if (!BotNpc_Post(unit, px, py, pz, po))
        return false;

    /*
     * THE SAME ROTATION RUN BACKWARDS. PointFromPost does
     *     x = px + f cos(o) + r sin(o)
     *     y = py + f sin(o) - r cos(o)
     * and projecting the difference back onto those two axes undoes it exactly,
     * so numbers read off here and typed into the editor land where you stood.
     */
    float const dx = wx - px;
    float const dy = wy - py;
    forward = dx * std::cos(po) + dy * std::sin(po);
    right   = dx * std::sin(po) - dy * std::cos(po);
    return true;
}

BotNpcIdleRules const* BotNpcMgr::IdleRules(uint32 designId) const
{
    std::map<uint32, BotNpcIdleRules>::const_iterator itr = m_idle.find(designId);
    return itr != m_idle.end() ? &itr->second : nullptr;
}

BotNpcEmoteRules const* BotNpcMgr::EmoteRules(uint32 designId) const
{
    std::map<uint32, BotNpcEmoteRules>::const_iterator itr = m_emote.find(designId);
    return itr != m_emote.end() ? &itr->second : nullptr;
}

/*
 * NAMES EVERY RANK BOT_NPC_GUILD_RANK, so a client addon can tell a bot NPC
 * from a player. BotNpc.h has the reasoning; the short of it is that rank names
 * are the one piece of arbitrary text the server can hand an addon about a unit
 * it does not know, using packets the client already asks for.
 *
 * COMPARES BEFORE WRITING because every path through BotNpc_ApplySubname calls
 * this, including the one taken on every pass of the bot manager. SetRankName
 * is an UPDATE per call and would otherwise be an UPDATE per rank per pass.
 *
 * A client that has ALREADY cached the guild query this session keeps the old
 * rank names until it next asks, which is a relog. That only bites a guild that
 * existed before this change, once.
 */
static void BotNpc_MarkGuildRanks(Guild* guild)
{
    if (!guild)
        return;

    for (uint32 rank = 0; rank < guild->GetRanksSize(); ++rank)
        if (guild->GetRankName(rank) != BOT_NPC_GUILD_RANK)
            guild->SetRankName(rank, BOT_NPC_GUILD_RANK);
}

/*
 * THE <BRACKET> LINE, WHICH IS A GUILD.
 *
 * A creature's subname arrives in SMSG_CREATURE_QUERY_RESPONSE and a player
 * guid gets SMSG_NAME_QUERY_RESPONSE, which has no such field -- so a bot NPC
 * cannot have one the ordinary way. A guild name lands in exactly that slot,
 * and nothing gates reading it: HandleGuildQueryOpcode answers any player's
 * query for any guild id with no membership check.
 *
 * ONE GUILD PER SUBNAME, SHARED by every NPC wearing it, the first one placed
 * being its leader.
 *
 * THE MEMBERSHIP IS REAL and has to be. A guild with no members is disbanded at
 * startup: Guild::CheckGuildStructure hands the leadership on while it can and
 * gives up when the last member goes, and GuildMgr::LoadGuilds deletes what it
 * gives up on. So the tempting shortcut -- label-only guilds with
 * PLAYER_GUILDID faked in the update block -- does not survive a restart, and
 * would have looked like it worked all afternoon.
 *
 * The names bypass the charter filter, which is deliberate rather than
 * overlooked: Guild::Create checks only GetGuildByName for uniqueness, because
 * IsValidCharterName and IsReservedName live in PetitionsHandler, the
 * PLAYER-facing path. "Innkeeper" is a reserved word to a player and exactly
 * what is wanted here.
 */
void BotNpc_ApplySubname(Player* bot, std::string const& subname)
{
    if (!bot)
        return;

    Guild* current = bot->GetGuildId() ? sGuildMgr.GetGuildById(bot->GetGuildId()) : nullptr;

    // Already wearing it. The common case on every reload, and it must cost
    // nothing and change nothing -- re-adding a member rewrites guild_member
    // rows and broadcasts joins to anybody in the guild.
    if (current && current->GetName() == subname)
    {
        // Still mark it. This is the path taken once everything has settled, so
        // a guild founded before the marker existed would never get one.
        BotNpc_MarkGuildRanks(current);
        return;
    }

    if (current)
    {
        /*
         * LEAVING IS NOT JUST DelMember, and getting that wrong is why a
         * subname could be set once and never changed.
         *
         * Its contract is "true means the guild is finished and THE CALLER
         * disbands it" -- and in the sole-leader case it returns true having
         * removed NOTHING: no member erased, no guild_member row deleted, no
         * SetInGuild(0). So the bot stayed in its old guild while this function
         * happily went on to make the new one, Guild::Create then failed with
         * ALREADY_IN_GUILD, and because Create writes its `guild` row BEFORE it
         * adds the leader, every attempt left an orphan guild behind. Two edits
         * produced two rows called "arch nemisis" and a bot still wearing
         * <Innkeeper>.
         *
         * HandleGuildLeaveOpcode is the shape to copy. Its extra refusal for a
         * leader with members is a PLAYER-facing rule and deliberately not
         * copied: here the leadership should simply pass to whoever else wears
         * the subname, which is what DelMember does by itself.
         */
        if (current->DelMember(bot->GetObjectGuid()))
        {
            current->Disband();
            delete current;
        }
    }

    if (subname.empty())
        return;

    if (Guild* existing = sGuildMgr.GetGuildByName(subname))
    {
        BotNpc_MarkGuildRanks(existing);
        existing->AddMember(bot->GetObjectGuid(), existing->GetLowestRank());
        return;
    }

    // Nobody wears this one yet, so this NPC founds it. Guild::Create needs a
    // leader with a session, which a logged-in bot has.
    Guild* made = new Guild;
    if (!made->Create(bot, subname))
    {
        // Create writes the `guild` row before it adds the leader, so a failure
        // leaves one behind -- upstream's own caller just deletes the object and
        // lets it rot. Clean it up rather than accumulating a guild per failed
        // attempt, which is exactly what happened the first time this ran.
        CharacterDatabase.PExecute("DELETE FROM guild WHERE guildid = '%u'", made->GetId());
        CharacterDatabase.PExecute("DELETE FROM guild_rank WHERE guildid = '%u'", made->GetId());

        delete made;
        sLog.outError("Bot NPC %s: could not make a guild called '%s' for its subname.",
                      bot->GetName(), subname.c_str());
        return;
    }

    sGuildMgr.AddGuild(made);

    // After Create, which is what makes the default ranks there are to rename.
    BotNpc_MarkGuildRanks(made);
}

/*
 * THE STOCK A BOT NPC SELLS. BotNpc.h has the reasoning; the short version is
 * that a bot NPC has no creature entry, so npc_vendor -- keyed by one -- can
 * never hold its stock, and a npc_vendor_template row set is the only kind of
 * list it can point at.
 *
 * NOT GUARDED ON UNIT_NPC_FLAG_VENDOR. The flag says whether the client may ask;
 * this says what the answer is. Keeping them separate is what lets the caller
 * report "has the flag and an empty list" as the distinct mistake it is, the
 * same way PrepareGossipMenu does for a creature.
 */
VendorItemData const* BotNpc_VendorItems(Unit const* unit)
{
    if (!unit || unit->GetTypeId() != TYPEID_PLAYER)
        return nullptr;

    BotNpcEntry const* entry = sBotNpcMgr.Get(unit->GetGUIDLow());
    if (!entry || !entry->vendorTemplateId)
        return nullptr;

    return sObjectMgr.GetNpcVendorTemplateItemList(entry->vendorTemplateId);
}

void BotNpc_ApplyInvulnerability(Player* bot, bool on)
{
    if (!bot)
        return;

    uint32 const flags = UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC;

    // SetFlag and RemoveFlag both compare before writing, so calling this on
    // every pass of the bot manager costs nothing once it has settled.
    if (on)
        bot->SetFlag(UNIT_FIELD_FLAGS, flags);
    else
        bot->RemoveFlag(UNIT_FIELD_FLAGS, flags);
}

/*
 * SOMEBODY EMOTED AT A BOT NPC. sql/custom/108.
 *
 * Called from HandleTextEmoteOpcode, beside housing's critter pat, and the two
 * are the same shape deliberately: a free function reaching into a subsystem,
 * because the seam that handler offers two lines further down --
 * `Creature::AI()->ReceiveEmote` -- needs an AI class bound through
 * creature_template.script_name, and a bot NPC is a Player and fails
 * `unit->IsCreature()` before that ever comes up.
 *
 * THE CHEAP CHECKS FIRST, IN THIS ORDER, because this runs on every text emote
 * every player on the server sends. AnyEmoteRules() is a bool and is false on a
 * server that has never written one; the guid test is two comparisons; only
 * then does anything look anything up.
 */
void BotNpc_ReceiveEmote(Player* who, uint32 textEmote, ObjectGuid target)
{
    if (!who || !sBotNpcMgr.AnyEmoteRules())
        return;

    // Targeted only, which is the design: /bow at her, not /bow near her. The
    // client puts the player's current selection in the packet, and a bot NPC
    // is a player, so this is where everything that is not one leaves.
    if (!target.IsPlayer() || !sBotNpcMgr.IsBotNpc(target.GetCounter()))
        return;

    Map* map = who->GetMap();
    if (!map)
        return;

    Player* bot = map->GetPlayer(target);
    if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        return;

    map->GetBotNpcIdle()->ReceiveEmote(map, bot, who, textEmote);
}

/*
 * ONE VALUE OUT OF `a=1&b=2`, or empty for a key that is not in there.
 *
 * No unescaping, and none needed: the dressing room is the only thing that
 * writes a look, and every value in one is a number or one of three words.
 */
static std::string BotNpc_LookValue(std::string const& query, std::string const& key)
{
    size_t at = 0;

    while (at <= query.size())
    {
        size_t amp = query.find('&', at);
        std::string const pair = query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);

        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, key) == 0)
            return pair.substr(eq + 1);

        if (amp == std::string::npos)
            break;

        at = amp + 1;
    }

    return "";
}

SheathState BotNpc_SheathFor(std::string const& look)
{
    std::string const hold = BotNpc_LookValue(look, "hold");

    if (hold == "away")
        return SHEATH_STATE_UNARMED;

    if (hold == "ranged")
        return SHEATH_STATE_RANGED;

    return SHEATH_STATE_MELEE;
}

BotNpcEntry const* BotNpcMgr::Get(uint32 characterGuid) const
{
    BotNpcSpawn const* spawn = GetSpawn(characterGuid);
    if (!spawn)
        return nullptr;

    std::map<uint32, BotNpcEntry>::const_iterator itr = m_entries.find(spawn->designId);
    return itr == m_entries.end() ? nullptr : &itr->second;
}

BotNpcSpawn const* BotNpcMgr::GetSpawn(uint32 characterGuid) const
{
    if (!characterGuid)
        return nullptr;

    std::map<uint32, BotNpcSpawn>::const_iterator itr = m_spawns.find(characterGuid);
    return itr == m_spawns.end() ? nullptr : &itr->second;
}

std::vector<BotNpcSpawn const*> BotNpcMgr::SpawnsOf(uint32 designId) const
{
    std::vector<BotNpcSpawn const*> found;

    for (const auto& itr : m_spawns)
        if (itr.second.designId == designId)
            found.push_back(&itr.second);

    return found;
}

/*
 * Case-insensitive, and NOT through normalizePlayerName, which was the first
 * version and quietly broke the moment names got longer than a player's.
 *
 * normalizePlayerName caps at MAX_INTERNAL_PLAYER_NAME (15) and returns FALSE
 * above it -- so "Innkeeper Allison" normalised to nothing, FindByName answered
 * nullptr, and `.npcbot place "Innkeeper Allison"` reported no such design
 * while the design sat there in plain sight. It also lowercases everything
 * after the first letter, which mangles the second word of a real NPC name.
 */
static bool BotNpcNameEquals(std::string const& a, std::string const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;

    return true;
}

std::vector<BotNpcEntry const*> BotNpcMgr::FindByName(std::string const& name) const
{
    std::vector<BotNpcEntry const*> found;

    for (const auto& itr : m_entries)
        if (BotNpcNameEquals(itr.second.name, name))
            found.push_back(&itr.second);

    return found;
}

BotNpcEntry const* BotNpcMgr::FindById(uint32 id) const
{
    std::map<uint32, BotNpcEntry>::const_iterator itr = m_entries.find(id);
    return itr == m_entries.end() ? nullptr : &itr->second;
}

void BotNpcMgr::ApplyToLoggedIn()
{
    HashMapHolder<Player>::MapType const& players = sObjectAccessor.GetPlayers();

    for (const auto& itr : players)
    {
        Player* player = itr.second;
        if (!player || !player->IsInWorld())
            continue;

        BotNpcEntry const* entry = Get(player->GetGUIDLow());

        // Both directions matter, and the second is the easy half to forget: a
        // character whose design was DELETED has to have its flags taken off,
        // or it stays an NPC until it next logs out.
        uint32 want = entry ? entry->npcFlags : 0;

        if (player->GetUInt32Value(UNIT_NPC_FLAGS) != want)
            player->SetUInt32Value(UNIT_NPC_FLAGS, want);

        if (entry)
            player->SetBotNpcGossipMenuId(entry->gossipMenuId);
        else
            player->SetBotNpcGossipMenuId(0);

        /*
         * WEAPONS DRAWN OR PUT AWAY -- and only where there IS a design. This
         * loop walks every player in world, so an `else` here would sheathe
         * the whole server every time somebody typed `reload bot_npc`.
         *
         * Deliberately NOT inside the re-dress below, which is gated on the
         * look having changed: the first reload after this shipped has
         * look_applied already equal to look for every NPC standing, and that
         * is exactly the reload that has to correct them.
         */
        if (entry)
            player->SetSheath(BotNpc_SheathFor(entry->look));

        /*
         * RENAMING, which the website has offered all along and which silently
         * did nothing.
         *
         * A character's name lives in memory once it is logged in, and
         * Player::SaveToDB writes the whole row from there -- so editing
         * `characters.name` under a live character is not a rename, it is a
         * value waiting to be overwritten on the next save. That cost an
         * afternoon of "the rename reverted" before it was understood.
         *
         * The cache matters as much as the field: GetPlayerGuidByName answers
         * from sObjectMgr's player cache, and a name changed without it is a
         * character that cannot be found by the name it is wearing -- which is
         * how `.npcbot place` came to create a second body instead of adopting
         * the one already standing there.
         */
        if (entry && !entry->name.empty() && entry->name != player->GetName())
        {
            std::string const oldName = player->GetName();

            player->SetName(entry->name);
            sObjectMgr.ChangePlayerNameInCache(player->GetGUIDLow(), oldName, entry->name);
            player->SaveToDB();

            sLog.outString("Bot NPC %s is now called %s.", oldName.c_str(), entry->name.c_str());
        }

        // Scenery does not die. Both directions, so a character that stops
        // being an NPC stops being invulnerable with it -- otherwise removing a
        // design would leave an immortal player standing in the world.
        BotNpc_ApplyInvulnerability(player, entry != nullptr);

        // The bracket line. Cheap when unchanged -- it checks the guild the bot
        // is already in before touching anything.
        if (entry)
            BotNpc_ApplySubname(player, entry->subname);

        /*
         * AND RE-WEAR THE OUTFIT, which is the half that was missing and the
         * reason `reload bot_npc` felt like it had not worked. Dressing an NPC
         * up on the website writes `look` on the DESIGN; nothing told the
         * standing character about it, so the NPC kept whatever it had on.
         *
         * ONLY WHEN THE DESIGN DIFFERS FROM WHAT WAS LAST PUT ON. The character
         * itself cannot be asked -- its inventory is not populated at every
         * moment this runs, so "all slots empty" and "not loaded yet" look
         * identical, and guessing wrong re-equips the outfit and orphans the
         * old one. sql/custom/087.
         */
        BotNpcSpawn const* spawn = GetSpawn(player->GetGUIDLow());
        if (entry && spawn && !entry->look.empty() && entry->look != spawn->lookApplied)
        {
            std::string error;
            if (BotNpc_Dress(player, entry->look, error))
                MarkLookApplied(player->GetGUIDLow(), entry->look);
            else
                sLog.outError("Bot NPC %s could not be dressed on reload: %s",
                              player->GetName(), error.c_str());
        }
    }
}

void BotNpcMgr::Reload()
{
    Load();
    ApplyToLoggedIn();

    // Said out loud because the interesting failure is silent: a design whose
    // character is not logged in reloads perfectly and changes nothing anybody
    // can see.
    sLog.outString("Bot NPCs reloaded: %u design(s), applied to everyone in world.", Count());
}

void BotNpcMgr::Save(BotNpcEntry const& entry)
{
    if (!entry.id)
    {
        sLog.outError("BotNpcMgr::Save called with no id -- the row has to exist first.");
        return;
    }

    m_entries[entry.id] = entry;

    std::string name = entry.name;
    std::string look = entry.look;
    std::string subname = entry.subname;
    CharacterDatabase.escape_string(name);
    CharacterDatabase.escape_string(look);
    CharacterDatabase.escape_string(subname);

    CharacterDatabase.PExecute(
        "UPDATE bot_npc SET name = '%s', race = %u, gender = %u, subname = '%s', look = '%s', "
        "gossip_menu_id = %u, vendor_template_id = %u, quest_giver_id = %u, npc_flags = %u "
        "WHERE id = %u",
        name.c_str(), entry.race, entry.gender, subname.c_str(), look.c_str(),
        entry.gossipMenuId, entry.vendorTemplateId, entry.questGiverId, entry.npcFlags, entry.id);
}

void BotNpcMgr::SaveSpawn(BotNpcSpawn const& spawn)
{
    if (!spawn.characterGuid || !spawn.designId)
    {
        sLog.outError("BotNpcMgr::SaveSpawn called without a character or a design.");
        return;
    }

    m_spawns[spawn.characterGuid] = spawn;

    std::string look = spawn.lookApplied;
    CharacterDatabase.escape_string(look);

    CharacterDatabase.DirectPExecute(
        "REPLACE INTO bot_npc_spawn (character_guid, bot_npc_id, map, position_x, position_y, "
        "position_z, orientation, look_applied) VALUES (%u, %u, %u, %f, %f, %f, %f, '%s')",
        spawn.characterGuid, spawn.designId, spawn.mapId, spawn.x, spawn.y, spawn.z, spawn.o,
        look.c_str());
}

void BotNpcMgr::RemoveSpawn(uint32 characterGuid)
{
    m_spawns.erase(characterGuid);
    CharacterDatabase.DirectPExecute("DELETE FROM bot_npc_spawn WHERE character_guid = %u", characterGuid);
}

void BotNpcMgr::MarkLookApplied(uint32 characterGuid, std::string const& look)
{
    std::map<uint32, BotNpcSpawn>::iterator itr = m_spawns.find(characterGuid);
    if (itr == m_spawns.end())
        return;

    itr->second.lookApplied = look;

    std::string safe = look;
    CharacterDatabase.escape_string(safe);
    CharacterDatabase.DirectPExecute("UPDATE bot_npc_spawn SET look_applied = '%s' WHERE character_guid = %u",
                                     safe.c_str(), characterGuid);
}
