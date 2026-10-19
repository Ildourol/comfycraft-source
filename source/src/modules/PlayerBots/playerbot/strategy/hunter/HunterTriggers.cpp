
#include "playerbot/playerbot.h"
#include "HunterTriggers.h"
#include "HunterActions.h"
#include "Database/CharacterDatabaseCache.h"

using namespace ai;

bool HunterNoStingsActiveTrigger::IsActive()
{
	Unit* target = AI_VALUE(Unit*, "current target");
    return target && AI_VALUE2(uint8, "health", "current target") > 40 &&
        !ai->HasAura("serpent sting", target) &&
        !ai->HasAura("scorpid sting", target) &&
        !ai->HasAura("viper sting", target);
}

// Call Pet when a pet is stored but not out. The core puts the pet away when
// the hunter dies, and a dead pet that decays goes to the database, so Revive
// Pet has no corpse to target. Call Pet brings back a dead pet as a corpse,
// and "hunters pet dead" then revives it. This was inverted (on only when
// Call Pet could NOT be cast), so a hunter companion never called its pet
// after a death (2026-10-17).
bool HunterNoPet::IsActive()
{
    if (AI_VALUE2(bool, "mounted", "self target"))
        return false;

    if (bot->GetPetGuid())
        return false;

    // Call Pet passes its cast check with nothing stored. Look for the pet
    // it would load, or the bot casts it every tick.
    if (!sCharacterDatabaseCache.GetCharacterPetByOwner(bot->GetGUIDLow()))
        return false;

    return ai->CanCastSpell("call pet", bot, 0);
}

bool HuntersPetDeadTrigger::IsActive()
{
    return AI_VALUE(bool, "pet dead") && !AI_VALUE2(bool, "mounted", "self target");
}

bool HuntersPetLowHealthTrigger::IsActive()
{
    Unit* pet = AI_VALUE(Unit*, "pet target");
    return pet && AI_VALUE2(uint8, "health", "pet target") < 40 &&
        !AI_VALUE2(bool, "dead", "pet target") && !AI_VALUE2(bool, "mounted", "self target");
}

bool HunterPetNotHappy::IsActive()
{
    return !AI_VALUE(bool, "pet happy") && !AI_VALUE2(bool, "mounted", "self target");
}

bool ViperStingOnAttackerTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (target)
    {
        const bool noStings = !ai->HasAura("serpent sting", target) &&
                              !ai->HasAura("scorpid sting", target) &&
                              !ai->HasAura("viper sting", target);
        if (noStings)
        {
            if (target->GetPower(POWER_MANA) >= 10)
            {
                return DebuffOnAttackerTrigger::IsActive();
            }
        }
    }

    return false;
}

bool SerpentStingOnAttackerTrigger::IsActive()
{
    Unit* target = GetTarget();
    if (target)
    {
        const bool noStings = !ai->HasAura("serpent sting", target) &&
                              !ai->HasAura("scorpid sting", target) &&
                              !ai->HasAura("viper sting", target);
        if (noStings)
        {
            if (target->GetPower(POWER_MANA) < 10)
            {
                return DebuffOnAttackerTrigger::IsActive();
            }
        }
    }

    return false;
}
