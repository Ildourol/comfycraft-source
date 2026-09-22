/*
 * BotNpcIdle -- what a bot NPC does when nobody has told it to do anything.
 *
 * A bot NPC stands there, deliberately. PlayerbotAI's update returns before
 * DoNextAction for anything IsBotNpc(), because the first one placed in the
 * world re-rolled its gear on a loop and then wandered off to attack something,
 * and a character conscripted as scenery needs the AI switched OFF rather than
 * tuned down. This is the other half: what it should do INSTEAD, written down
 * by a person on `/admin/npcbots` rather than decided by an engine.
 *
 * Four rules, one per trigger (BotNpc.h):
 *
 *      loop         every `period` seconds, while nobody is talking to it
 *      near         a real player crosses into `radius` yards, once per visit
 *      after_talk   a conversation with it ends
 *      custom       a Lua scene holding its own loop, started once
 *
 * The first three run an ordinary `generic_scripts` timeline with the NPC as
 * source and whoever set it off as target, so there is no second executor and
 * no new script command: say, emote, walk, cast, play an effect, run a Lua
 * scene.
 *
 * `custom` is the one that is not on a clock at all. It is a scene, started the
 * first time the NPC is seen standing in the world and never put on a timer --
 * every wait in it is one the author wrote. What starts it again is a bump of
 * BotNpcMgr::Generation(), which every `reload bot_npc` does, so saving on the
 * website lands the way it does for a timeline.
 *
 * ------------------------------------------------------------------ THREADING
 *
 * ONE DRIVER PER MAP, HELD BY VALUE, TICKED FROM Map::Update. That is the same
 * thread the scripts execute on, the same thread LuaScene resumes on, and the
 * only thread on which a grid search is safe while maps update in parallel.
 *
 * It is also the thread CMSG_GOSSIP_HELLO is handled on -- that opcode is
 * PACKET_PROCESS_MAP, drained by Map::Update itself -- which is why NoteTalk
 * needs no lock.
 *
 * Player::Update would have been the obvious alternative and is wrong:
 * Map::UpdatePlayers skips inactive players on continents, and a bot NPC
 * standing alone in a field is the definition of one.
 *
 * ------------------------------------------------------------ WHAT IT COSTS
 *
 * One pass over the map's player list per second, and only when at least one
 * design anywhere has a rule (sBotNpcMgr.AnyIdleRules()). On a server that has
 * never written one, that is a bool.
 */

#ifndef COMFY_BOTNPCIDLE_H
#define COMFY_BOTNPCIDLE_H

#include "Common.h"
#include "ObjectGuid.h"
#include "BotNpc.h"

#include <set>
#include <unordered_map>

class Map;
class Player;

class BotNpcIdle
{
    public:
        // Once a second, from Map::Update. `diff` is the map's own tick.
        void Update(Map* map, uint32 diff);

        /*
         * SOMEBODY IS TALKING TO THIS NPC, as of now. Called from the bot NPC
         * arms of the two gossip handlers -- hello and select -- and from
         * nowhere else.
         *
         * THERE IS NO MATCHING NoteTalkEnded, AND THAT IS NOT AN OVERSIGHT.
         * The 1.12 client sends nothing at all when the player closes the
         * window with Escape: SMSG_GOSSIP_COMPLETE goes the other way, and
         * PlayerMenu keeps no record of whose menu it is. So there is no event
         * to wait for, and hooking CloseGossip would catch only the half of the
         * cases where a button did it.
         *
         * A conversation is therefore over when the clicking has stopped and
         * they are no longer standing there, which is also what it looks like
         * from the NPC's side.
         */
        void NoteTalk(ObjectGuid npc, ObjectGuid player);

        /*
         * SOMEBODY EMOTED AT THIS NPC. Called from BotNpc_ReceiveEmote, which
         * HandleTextEmoteOpcode calls, and from nowhere else.
         *
         * NOT ON THE TICK, because this is an event and has one. The other four
         * triggers are moments the server has to go looking for; this one
         * arrives in a packet with the target guid already in it.
         *
         * Still the map's thread: CMSG_TEXT_EMOTE is PACKET_PROCESS_MAP, drained
         * by Map::Update itself, which is the same argument NoteTalk makes.
         */
        void ReceiveEmote(Map* map, Player* bot, Player* who, uint32 textEmote);

    private:
        struct State
        {
            // When each trigger may next fire. Indexed by BotNpcIdleTrigger.
            time_t next[BOT_NPC_IDLE_TRIGGERS] = {};

            /*
             * WHILE SOMETHING IS ALREADY RUNNING, NOTHING ELSE STARTS.
             *
             * Two timelines on one NPC at once is an NPC saying two things over
             * each other and walking to two places, and it is easy to reach --
             * a player walking up during a loop run does it. Set to the end of
             * whatever was started, which is known exactly: a script's last row
             * carries its delay.
             */
            time_t busyUntil = 0;

            // Who is talking to it, and when they last clicked. See NoteTalk.
            ObjectGuid talker;
            time_t talkedAt = 0;

            /*
             * PLAYERS STANDING INSIDE THE `near` RADIUS.
             *
             * The trigger is crossing IN, not being inside, so somebody who
             * walks up and stays gets one greeting rather than one a second.
             * They are forgotten when they leave, which is what makes coming
             * back a second visit.
             */
            std::set<ObjectGuid> inside;

            // The pass that last saw this NPC. Anything older has left the map,
            // and its state goes with it -- including `inside`, which would
            // otherwise claim everyone was already there when it came back.
            time_t seenAt = 0;

            /*
             * THE GENERATION THE `custom` SCENE WAS STARTED UNDER, or 0 for
             * never.
             *
             * A custom behaviour is started ONCE and never restarted on a
             * clock, so this is the whole of its bookkeeping: start when the
             * two disagree, and they disagree exactly twice -- the first time
             * this NPC is seen, and after a `reload bot_npc`. Losing the state
             * when the NPC leaves the map is what makes a relog a fresh start.
             */
            uint32 customGen = 0;

            // Which scene that was. Kept so stopping it is exact: a gossip
            // button's scene on the same NPC must not be cut off because the
            // idle loop restarted.
            uint32 customScene = 0;

            /*
             * WHEN EACH EMOTE MAY BE ANSWERED AGAIN. sql/custom/108.
             *
             * PER EMOTE RATHER THAN ONE CLOCK FOR ALL OF THEM, because the
             * rules are independent: answering /bow should not make /wave go
             * quiet. `busyUntil` is the one that stops two answers overlapping,
             * and it is shared with the other four the way it always was.
             *
             * A map rather than an array, since the key is an EmotesText id and
             * there are a few hundred of those. It is empty on an NPC nobody
             * has emoted at, which is nearly all of them.
             */
            std::unordered_map<uint32, time_t> emoteNext;
        };

        void Tick(Map* map, Player* bot, BotNpcIdleRules const& rules, State& st, time_t now);
        void Fire(Map* map, Player* bot, State& st, BotNpcIdleTrigger trigger,
                  BotNpcIdleRule const& rule, ObjectGuid who, time_t now);
        void StartCustom(Map* map, Player* bot, State& st, BotNpcIdleRule const& rule);
        void StopCustom(Map* map, Player* bot, State& st);

        uint32 m_timer = 0;
        std::unordered_map<ObjectGuid, State> m_state;
};

#endif
