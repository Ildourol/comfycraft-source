#include "MountRental.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "WorldSession.h"
#include "Log.h"

MountRentalResult RentMount(Player* player, uint32 spellId, uint32 durationMs, uint32 cost)
{
    if (player->IsMounted())
    {
        player->GetSession()->SendNotification("You are already mounted.");
        return MOUNT_RENTAL_ALREADY_MOUNTED;
    }

    SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
    if (!spellInfo || !spellInfo->HasAura(SPELL_AURA_MOUNTED))
    {
        sLog.outError("RentMount: spell %u is not a mount spell, %s is not mounted.", spellId, player->GetName());
        return MOUNT_RENTAL_NOT_A_MOUNT;
    }

    if (player->GetMoney() < cost)
    {
        player->GetSession()->SendNotification("You don't have enough money!");
        return MOUNT_RENTAL_NO_MONEY;
    }

    // INSTANT, or the clamp below never runs. 82 of the stable rows use a real
    // mount spell with a 3 s cast time, and a triggered spell with a cast time
    // waits for a later tick (Spell::prepare). GetSpellAuraHolder then found
    // nothing, so the player got the mount 3 s late, free, permanent and at the
    // skill speed. The no-cast-time flag makes Spell::prepare cast it now.
    bool const hadNoCastTime = player->HasOption(PLAYER_CHEAT_NO_CAST_TIME);
    player->EnableOption(PLAYER_CHEAT_NO_CAST_TIME);
    SpellCastResult const result = player->CastSpell(player, spellInfo, true);
    if (!hadNoCastTime)
        player->RemoveOption(PLAYER_CHEAT_NO_CAST_TIME);

    // A triggered cast does not tell the client why it failed, so say it here.
    // A mount spell has conditions of its own: indoors, in combat, shapeshifted.
    SpellAuraHolder* holder = player->GetSpellAuraHolder(spellId);
    if (!holder)
    {
        if (result != SPELL_CAST_OK)
            Spell::SendCastResult(player, spellInfo, result);
        return MOUNT_RENTAL_REFUSED;
    }

    if (durationMs)
    {
        // SetAuraMaxDuration clears the permanent flag for us, which is what
        // turns a real mount spell (duration index 21, -1) into a rental.
        holder->SetAuraMaxDuration(durationMs);
        holder->SetAuraDuration(durationMs);
        holder->UpdateAuraDuration();

        // The speed aura's handler gives a mount with a timer the rental speed
        // by level. It ran during the cast, while a real mount spell was still
        // permanent, so run it again now that the holder has a timer. The
        // handler also calls UpdateSpeed. Which effect index carries the speed
        // is not fixed across mount spells, so look rather than assume.
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (Aura* aura = holder->GetAuraByEffectIndex(SpellEffectIndex(i)))
                if (aura->GetModifier()->m_auraname == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)
                    aura->HandleAuraModIncreaseMountedSpeed(true, true);
    }

    if (cost)
        player->ModifyMoney(-int32(cost));
    return MOUNT_RENTAL_OK;
}
