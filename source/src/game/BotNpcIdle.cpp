#include "BotNpcIdle.h"

#include "Log.h"
#include "ModuleSlots.h"
#include "ObjectGuid.h"
#include "LuaScene.h"
#include "ScriptMgr.h"
#include "World.h"

#include "Maps/Map.h"
#include "Objects/Player.h"

#include <iterator>
#include <list>
#include <set>

namespace
{
    /*
     * HOW CLOSE COUNTS AS STILL TALKING TO IT.
     *
     * INTERACTION_DISTANCE is 5 yards and is what the gossip handler enforces
     * on the click. This is deliberately wider: a step backwards while reading
     * a menu is not walking away, and ending the conversation on it would have
     * an NPC turning back to its chores mid-sentence.
     */
    float const TALK_RANGE = 12.0f;

    /*
     * AND HOW LONG AFTER THE LAST CLICK.
     *
     * The backstop for somebody who reads a menu, presses Escape and stands
     * exactly where they were -- which the server never hears about at all, so
     * without this the NPC would be gated on a conversation that ended minutes
     * ago. Long enough to read a page of dialog, short enough that an NPC does
     * not look broken.
     */
    uint32 const TALK_IDLE_SECONDS = 20;

    // The last row of a script carries the whole thing's length, because delay
    // is absolute seconds from the start. An id with nothing behind it is 0,
    // which is the right answer for a script that will not run.
    uint32 ScriptLength(uint32 scriptId)
    {
        ScriptMapMap::const_iterator itr = sGenericScripts.find(scriptId);
        if (itr == sGenericScripts.end() || itr->second.empty())
            return 0;
        return itr->second.rbegin()->first;
    }
}

void BotNpcIdle::Update(Map* map, uint32 diff)
{
    /*
     * A SECOND IS THE RESOLUTION EVERYTHING HERE WORKS AT. Script delays are
     * whole seconds, Map::ScriptsStart schedules against GetGameTime(), and the
     * `near` radius is a thing a person walks across rather than teleports. A
     * faster tick would buy nothing and pay for a grid search with it.
     */
    if (m_timer > diff)
    {
        m_timer -= diff;
        return;
    }
    m_timer = 1000;

    if (!sBotNpcMgr.AnyIdleRules() && !sBotNpcMgr.AnyEmoteRules())
    {
        // Somebody deleted the last rule on the server. Let the state go rather
        // than keeping it for ever against the chance one comes back -- a stale
        // `inside` set would greet nobody on their next arrival.
        m_state.clear();
        return;
    }

    time_t const now = sWorld.GetGameTime();

    Map::PlayerList const& players = map->GetPlayers();
    for (const auto& ref : players)
    {
        Player* bot = ref.getSource();
        if (!bot || !bot->IsInWorld() || !bot->IsBotNpc())
            continue;

        BotNpcEntry const* design = sBotNpcMgr.Get(bot->GetGUIDLow());
        if (!design)
            continue;

        if (BotNpcIdleRules const* rules = sBotNpcMgr.IdleRules(design->id))
        {
            Tick(map, bot, *rules, m_state[bot->GetObjectGuid()], now);
            continue;
        }

        /*
         * AN NPC WITH ONLY EMOTE RULES STILL HAS TO BE STAMPED, and forgetting
         * this is what would make its cooldowns useless: the sweep below drops
         * anything it did not see this pass, so a row created by an emote would
         * be erased one second later and the next /bow would fire again.
         *
         * `find`, not operator[]. An NPC nobody has ever emoted at has no row,
         * and the point of this branch is not to give it one.
         */
        if (sBotNpcMgr.EmoteRules(design->id))
        {
            auto itr = m_state.find(bot->GetObjectGuid());
            if (itr != m_state.end())
                itr->second.seenAt = now;
        }
    }

    /*
     * ANYTHING THIS PASS DID NOT SEE HAS LEFT THE MAP, or stopped being an NPC,
     * or had its last rule deleted. Its state goes with it -- and the part that
     * matters is `inside`, which would otherwise insist everybody standing
     * there had already been greeted when it came back.
     *
     * A stamp rather than a set of what was seen, so a quiet map allocates
     * nothing at all on the way through.
     */
    for (auto itr = m_state.begin(); itr != m_state.end(); )
    {
        if (itr->second.seenAt == now)
        {
            ++itr;
            continue;
        }

        /*
         * AND ITS CUSTOM SCENE GOES WITH IT. A parked scene holds a guid, not a
         * pointer, so one whose NPC has gone is not dangerous -- every call in
         * it simply does nothing. It is also immortal: `while true do ... end`
         * has no reason to stop, and it would sit on the map resuming for ever
         * over a character that logged out.
         *
         * An author who writes `while not npc:IsGone()` does not need this. The
         * ones who do not are the reason it is here.
         */
        if (itr->second.customScene)
            if (LuaScene* scene = map->GetLuaScene())
                scene->StopFor(itr->first, itr->second.customScene);

        itr = m_state.erase(itr);
    }
}

void BotNpcIdle::NoteTalk(ObjectGuid npc, ObjectGuid player)
{
    // NOT operator[]. A conversation with an NPC that has no idle rule is not
    // worth a row, and this is called for every gossip click on every bot NPC
    // on the server.
    auto itr = m_state.find(npc);
    if (itr == m_state.end())
        return;

    itr->second.talker = player;
    itr->second.talkedAt = sWorld.GetGameTime();
}

/*
 * SOMEBODY EMOTED AT THIS NPC. sql/custom/108.
 *
 * The fifth trigger, and the only one that is an EVENT rather than a moment the
 * server went looking for -- CMSG_TEXT_EMOTE arrives carrying the guid of
 * whatever the player had targeted, so there is nothing to search for and no
 * reason to be on the tick.
 *
 * WHAT IT REFUSES, AND WHY EACH ONE IS HERE:
 *
 *  - A BOT EMOTER. The playerbots module emits text emotes by calling
 *    HandleTextEmoteOpcode directly, so without this a companion standing in a
 *    party could set an NPC off with nobody watching. Same rule `near` makes.
 *  - AN INVISIBLE GM. Also `near`'s rule: somebody who cannot be seen testing
 *    something should not be starting scenes for an empty room.
 *  - ANYBODY TOO FAR AWAY. A target stays selected across a zone, so
 *    targeted-only is not on its own a proximity test. The text-emote listen
 *    range is the right number because it is exactly the set of people who saw
 *    the emote happen; anything else would have the NPC answering something
 *    nobody in the room witnessed.
 *  - A BUSY NPC. `busyUntil` is shared with the other four, so an emote cannot
 *    talk over a greeting and a greeting cannot talk over an emote.
 *  - AN EMOTE INSIDE ITS COOLDOWN, silently. Nothing in this core throttles
 *    emotes at all, so a held-down /bow macro arrives several times a second;
 *    housing's critter pat refuses the same way and for the same reason, that
 *    a refusal message on a held macro is its own spam.
 */
void BotNpcIdle::ReceiveEmote(Map* map, Player* bot, Player* who, uint32 textEmote)
{
    BotNpcEntry const* design = sBotNpcMgr.Get(bot->GetGUIDLow());
    if (!design)
        return;

    BotNpcEmoteRules const* rules = sBotNpcMgr.EmoteRules(design->id);
    if (!rules)
        return;

    BotNpcEmoteRule const* match = nullptr;
    for (BotNpcEmoteRule const& r : *rules)
    {
        if (r.emoteId == textEmote)
        {
            match = &r;
            break;
        }
    }

    if (!match)
        return;

    if (who->GetModuleSlot(MODULE_SLOT_BOT_AI))
        return;

    if (who->IsGameMaster() && !who->isGMVisible())
        return;

    if (!bot->IsWithinDistInMap(who, sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_TEXTEMOTE)))
        return;

    // operator[] here, unlike NoteTalk's find: a matching rule is the whole
    // reason this NPC needs a row, and without one there is nowhere to keep the
    // cooldown that stops the next line of a macro.
    State& st = m_state[bot->GetObjectGuid()];

    time_t const now = sWorld.GetGameTime();

    if (now < st.busyUntil)
        return;

    time_t& next = st.emoteNext[textEmote];
    if (now < next)
        return;

    uint32 const length = ScriptLength(match->scriptId);

    if (!length && sGenericScripts.find(match->scriptId) == sGenericScripts.end())
    {
        // The reload order again: a rule names a script id, so
        // `reload generic_scripts` has to come before `reload bot_npc`. Said
        // once per cooldown, because the clock moves on below either way.
        sLog.outError("Bot NPC %s has an emote rule pointing at generic_scripts %u, which is not loaded.",
                      bot->GetName(), match->scriptId);
    }

    map->ScriptsStart(sGenericScripts, match->scriptId, bot->GetObjectGuid(), who->GetObjectGuid());

    next = now + match->period;
    st.busyUntil = now + length;

    // Belt and braces. Update's pass already stamps an emote-only NPC before
    // the sweep runs, so this is not what keeps the row alive -- it is here so
    // that a later change to that branch cannot quietly turn every cooldown on
    // the server into no cooldown at all.
    st.seenAt = now;
}

void BotNpcIdle::Tick(Map* map, Player* bot, BotNpcIdleRules const& rules, State& st, time_t now)
{
    st.seenAt = now;

    /*
     * DEAD OR FIGHTING IS NOT A MOMENT TO START A PERFORMANCE. A bot NPC is
     * made invulnerable and has its AI switched off, so neither should happen
     * -- but "should not happen" is how an NPC ends up dancing over its own
     * corpse, and the check is two words.
     */
    if (!bot->IsAlive() || bot->IsInCombat())
        return;

    /*
     * THE CUSTOM SCENE, WHICH IS NOT ON A CLOCK AT ALL.
     *
     * Started when the generation it was started under stops matching the
     * manager's, which happens exactly twice: the first time this NPC is seen
     * standing in the world, and after a `reload bot_npc`. Everything else
     * about its timing is a `wait()` the author wrote.
     *
     * IT DOES NOT TOUCH `busyUntil` AND IS NOT GATED BY IT. The other three are
     * moments, and two moments landing on one NPC is one of them talking over
     * the other; a custom scene is the NPC's whole day, and there is no end to
     * wait for. An author using both gets both, which is theirs to arrange.
     */
    BotNpcIdleRule const& custom = rules[BOT_NPC_IDLE_CUSTOM];
    if (custom.Runs())
    {
        if (st.customGen != sBotNpcMgr.Generation())
            StartCustom(map, bot, st, custom);
    }
    else if (st.customScene)
    {
        // Switched off on the website. Stopped rather than left to finish: a
        // `while true` loop has no end of its own, so "it will stop eventually"
        // is not true of the shape this is written in.
        StopCustom(map, bot, st);
    }

    /*
     * IS ANYBODY TALKING TO IT.
     *
     * There is no close event to wait for -- see NoteTalk -- so a conversation
     * is over when the clicking stopped and they are no longer standing there.
     * Whichever of those comes first.
     */
    bool talking = false;
    if (!st.talker.IsEmpty())
    {
        Player* who = map->GetPlayer(st.talker);
        bool const stillHere = who && who->IsInWorld() &&
                               who->IsWithinDistInMap(bot, TALK_RANGE);

        if (stillHere && uint32(now - st.talkedAt) < TALK_IDLE_SECONDS)
        {
            talking = true;
        }
        else
        {
            ObjectGuid const was = st.talker;
            st.talker.Clear();

            /*
             * THE PERSON IS PASSED ON ONLY IF THEY ARE STILL THERE. A script
             * whose target has walked off has a target of nothing, which every
             * command handler reads as "no player" rather than as an error --
             * and a line addressed to somebody who has left is worse than no
             * line at all.
             */
            BotNpcIdleRule const& rule = rules[BOT_NPC_IDLE_AFTER_TALK];
            if (rule.Runs() && now >= st.next[BOT_NPC_IDLE_AFTER_TALK] && now >= st.busyUntil)
                Fire(map, bot, st, BOT_NPC_IDLE_AFTER_TALK, rule, stillHere ? was : ObjectGuid(), now);
        }
    }

    /*
     * WHO IS STANDING INSIDE THE RADIUS, kept up to date whether the trigger is
     * off cooldown or not. The set is what "crossed IN" is measured against, so
     * letting it go stale during a cooldown would fire the greeting again at
     * somebody who never moved.
     */
    BotNpcIdleRule const& greet = rules[BOT_NPC_IDLE_NEAR];
    if (greet.Runs())
    {
        std::list<Player*> found;
        bot->GetAlivePlayerListInRange(bot, found, greet.radius);

        std::set<ObjectGuid> inside;
        ObjectGuid arrived;

        for (Player* p : found)
        {
            if (!p || p == bot || !p->IsInWorld())
                continue;

            /*
             * REAL PLAYERS ONLY. Anything carrying the bot module's AI slot is
             * a companion, an alt bot or another bot NPC, and two guards
             * standing near each other would otherwise greet one another for
             * ever with nobody watching.
             */
            if (p->GetModuleSlot(MODULE_SLOT_BOT_AI))
                continue;

            // A GM who has made themselves invisible is not somebody who walked
            // up, and an NPC reacting to one gives the game away.
            if (p->IsGameMaster() && !p->isGMVisible())
                continue;

            ObjectGuid const guid = p->GetObjectGuid();
            inside.insert(guid);

            // The FIRST new arrival, not all of them. A script runs for one
            // person, and a crowd arriving together is one greeting.
            if (arrived.IsEmpty() && !st.inside.count(guid))
                arrived = guid;
        }

        st.inside.swap(inside);

        if (!arrived.IsEmpty() && !talking && now >= st.next[BOT_NPC_IDLE_NEAR] && now >= st.busyUntil)
            Fire(map, bot, st, BOT_NPC_IDLE_NEAR, greet, arrived, now);
    }
    else if (!st.inside.empty())
    {
        st.inside.clear();
    }

    /*
     * AND THE LOOP, LAST OF THE THREE. It is the one that yields: a greeting
     * and a parting line are about somebody who is standing there, and the
     * chores can wait a minute.
     */
    BotNpcIdleRule const& loop = rules[BOT_NPC_IDLE_LOOP];
    if (loop.Runs() && !talking && now >= st.next[BOT_NPC_IDLE_LOOP] && now >= st.busyUntil)
        Fire(map, bot, st, BOT_NPC_IDLE_LOOP, loop, ObjectGuid(), now);
}

/*
 * START THE CUSTOM SCENE, HAVING STOPPED WHATEVER WAS THERE.
 *
 * The stop is not defensive tidying, it is the whole reason this is one
 * function: nothing else in LuaScene cancels a scene, so a second copy of a
 * looping one would run beside the first for ever, saying everything twice and
 * walking the NPC to two places. It is narrowed to the scene id so that a
 * conversation somebody is having with this NPC through a gossip button is not
 * cut off because the idle loop was reloaded.
 */
void BotNpcIdle::StartCustom(Map* map, Player* bot, State& st, BotNpcIdleRule const& rule)
{
    LuaScene* scene = map->GetLuaScene();
    if (!scene)
        return;

    if (st.customScene)
        scene->StopFor(bot->GetObjectGuid(), st.customScene);

    auto data = LuaScene::Data();
    auto it = data->scripts.find(rule.sceneId);
    if (it == data->scripts.end())
    {
        /*
         * The usual cause is the reload order: a rule names a scene, so
         * `reload dialog_scenes` has to come before `reload bot_npc`. Said once
         * per start rather than once per second, because the generation is
         * taken below either way.
         */
        sLog.outError("Bot NPC %s has a custom behaviour pointing at dialog_lua %u, which is not loaded.",
                      bot->GetName(), rule.sceneId);
        st.customGen = sBotNpcMgr.Generation();
        st.customScene = 0;
        return;
    }

    char name[32];
    snprintf(name, sizeof(name), "scene %u", rule.sceneId);

    /*
     * NO PLAYER, AND THAT IS THE INTERESTING HALF.
     *
     * Nobody started this, so there is nobody for `player` to be. A scene that
     * wants to react to somebody asks for them itself, with
     * `npc:PlayersNear(yards)` -- which is the binding that exists because of
     * this case. `print` has nobody to whisper to either, so it only logs.
     */
    scene->Run(rule.sceneId, it->second, name, bot->GetObjectGuid(), ObjectGuid());

    st.customGen = sBotNpcMgr.Generation();
    st.customScene = rule.sceneId;
}

void BotNpcIdle::StopCustom(Map* map, Player* bot, State& st)
{
    if (LuaScene* scene = map->GetLuaScene())
        scene->StopFor(bot->GetObjectGuid(), st.customScene);

    st.customScene = 0;
    st.customGen = 0;
}

void BotNpcIdle::Fire(Map* map, Player* bot, State& st, BotNpcIdleTrigger trigger,
                      BotNpcIdleRule const& rule, ObjectGuid who, time_t now)
{
    uint32 const length = ScriptLength(rule.scriptId);

    if (!length && sGenericScripts.find(rule.scriptId) == sGenericScripts.end())
    {
        /*
         * A RULE POINTING AT A SCRIPT THAT IS NOT LOADED. Map::ScriptsStart
         * would return in silence, so this says it instead -- and only once per
         * period, because `next` is moved on below whether it ran or not.
         *
         * The usual cause is the reload order: a rule names a script id, so
         * `reload generic_scripts` has to come before `reload bot_npc`.
         */
        sLog.outError("Bot NPC %s has an idle rule pointing at generic_scripts %u, which is not loaded.",
                      bot->GetName(), rule.scriptId);
    }

    map->ScriptsStart(sGenericScripts, rule.scriptId, bot->GetObjectGuid(), who);

    /*
     * BOTH CLOCKS MOVE, AND THEY MEAN DIFFERENT THINGS.
     *
     * `next` is this trigger's own cooldown, measured from the start, which is
     * what a person setting "every 40 seconds" means.
     *
     * `busyUntil` is shared by all three and is how long the NPC is actually
     * occupied, which the script itself says: its last row's delay. Without it,
     * a player walking up during a loop run would have the NPC greeting them
     * over the top of whatever it was already saying.
     */
    st.next[trigger] = now + rule.period;
    st.busyUntil = now + length;
}
