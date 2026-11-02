
#include "playerbot/playerbot.h"
#include "playerbot/ServerFacade.h"
#include "WarlockActions.h"

using namespace ai;

uint32 CastSummonDemonAction::MissingReagent()
{
    uint32 spellId = GetSpellID();
    if (!spellId)
        spellId = AI_VALUE2(uint32, "spell id", GetSpellName());

    const SpellEntry* spellInfo = sServerFacade.LookupSpellInfo(spellId);
    if (!spellInfo)
        return 0;

    for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (spellInfo->Reagent[i] <= 0)
            continue;

        const uint32 itemId = uint32(spellInfo->Reagent[i]);
        if (!bot->HasItemCount(itemId, spellInfo->ReagentCount[i]))
            return itemId;
    }

    return 0;
}

// A line goes out when it changes, or after 30 s of the same line, so a
// summon that waits on mana does not fill the log every check.
void CastSummonDemonAction::TellPetDebug(const std::string& text)
{
    Player* master = ai->GetMaster();
    if (!master || !ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
        return;

    const time_t now = time(0);
    if (text == lastDebugText && now - lastDebugTime < 30)
        return;

    lastDebugText = text;
    lastDebugTime = now;
    ai->TellDebug(master, "pet: " + text);
}

bool CastSummonDemonAction::isUseful()
{
    Unit* pet = AI_VALUE(Unit*, "pet target");
    if (pet && pet->GetEntry() == demonEntry)
        return false;

    // The engine waits out a summon's 10 s cast. This covers a tick that comes
    // in during it from somewhere else.
    if (bot->IsNonMeleeSpellCasted(false, false, true))
        return false;

    if (const uint32 itemId = MissingReagent())
    {
        const ItemPrototype* proto = sObjectMgr.GetItemPrototype(itemId);
        TellPetDebug(GetSpellName() + " needs " + (proto ? proto->Name1 : std::to_string(itemId)) + ", none in bags");
        return false;
    }

    if (!lastResort && !pet && lastCastStart && time(0) - lastCastStart < RetrySeconds)
    {
        TellPetDebug(GetSpellName() + " was cast " + std::to_string(time(0) - lastCastStart) +
            " s ago and no demon came, the next summon goes first");
        return false;
    }

    return true;
}

bool CastSummonDemonAction::isPossible()
{
    if (CastSpellAction::isPossible())
        return true;

    // `debug action` already writes "why: ... cast result N" from
    // CastSpellAction::isPossible. This is the same fact for `debug` alone.
    if (!ai->HasStrategy("debug action", BotState::BOT_STATE_NON_COMBAT) &&
        ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
    {
        if (notKnown)
        {
            TellPetDebug(GetSpellName() + " not known");
        }
        else
        {
            SpellCastResult result = SPELL_CAST_OK;
            ai->CanCastSpell(GetSpellName(), bot, 0, nullptr, true, false, false, &result);
            TellPetDebug(GetSpellName() + " refused, cast result " + std::to_string((uint32)result));
        }
    }

    return false;
}

bool CastSummonDemonAction::Execute(Event& event)
{
    if (!CastSpellAction::Execute(event))
    {
        TellPetDebug(GetSpellName() + " failed to start");
        return false;
    }

    lastCastStart = time(0);
    TellPetDebug("casting " + GetSpellName());
    return true;
}
