/*
 * item_invasion_sigil -- the Invader's Sigil. Right-click it and the server
 * goes looking for somebody flagged for PvP near your level, warns them, and
 * a few seconds later drops you in behind them. Item 100035, bound by
 * item_template.script_name; see sql/custom/075_invasion_sigil.sql and
 * docs/features/invasions.md.
 *
 * IT REALLY CASTS, AND THE SEARCH WAITS FOR THE BAR. pItemUse runs from
 * WorldSession::HandleUseItemOpcode BEFORE CastItemUseSpell, so returning FALSE
 * is the whole mechanism: the caller goes on to cast, the client draws its bar,
 * and spell_invasion_search::OnSuccessfulFinish does the hunting. Divining for
 * a victim should take a moment and look like it is taking a moment.
 *
 * IT DID NOT ALWAYS. The first version returned true on every path, so nothing
 * cast at all -- and a 1.12 client that has begun animating an item use it
 * never gets an answer to stands there with its hands glowing indefinitely. In
 * game that read as "nothing happens", even with the refusal message sitting in
 * the chat frame. A real cast ends by itself and the whole class of problem
 * goes away; the refusal path below still needs SendEquipError, because that
 * path genuinely does not cast.
 *
 * A REFUSAL COSTS NOTHING, because it is decided BEFORE the cast starts.
 * CanBeginInvasion asks whether invading could work at all -- enabled, high
 * enough level, not already hunting, not in combat, not hardcore, off cooldown
 * -- so a no is instant, with no bar, no cooldown and no charge spent. That is
 * the same split HouseFurnitureShouldCast uses next door in housing, and it is
 * why the sigil is never consumed on any path: it carries spellcharges_1 = 0,
 * so TakeCastItem finds nothing expendable and no code of ours has to protect
 * it.
 *
 * WHY OnSuccessfulFinish AND NOT pItemUseSpell. The name says "after the cast"
 * and it is not: Player::CastItemUseSpell calls OnItemUseSpell straight after
 * spell->prepare(), which for a timed spell is the moment the bar STARTS.
 * OnSuccessfulFinish is the last statement of Spell::finish(true), so walking
 * away mid-cast hunts nobody, with nothing written here to make that true.
 *
 * SO WHY DOES IT CARRY THIS PARTICULAR SPELL. Because the 1.12 client decides
 * whether an item is right-clickable from its spell list, not from anything the
 * server says: an item with no ON_USE spell never sends CMSG_USE_ITEM and this
 * script would never run. The sigil carries 51942 "Shards of Hellfury"
 * (sql/custom/077), and it had to clear a bar:
 *
 *   - effectImplicitTargetA1 = 1, THE CASTER. Cast from an item there is no
 *     target, so a spell wanting one fails and the hunt never starts. This is
 *     what disqualified the better-looking runner-up -- see below.
 *   - AN EMPTY DESCRIPTION, so no green "Use:" line of somebody else's flavour
 *     text lands on the tooltip. The furniture crates carry 33453, whose
 *     description is about engineering goggles, and pay for it with an addon
 *     that strips the line; there was no reason to inherit that here.
 *   - targets = 0, so no ground reticle is raised. The chalk deliberately picks
 *     a spell that DOES raise one; this deliberately picks one that does not.
 *   - A CAST TIME AND NOTHING ELSE -- effect 3 is DUMMY, so the cast fills a bar
 *     and plays its visual and does nothing whatever. 6000ms here.
 *   - No script_name, no stance, no spell focus, no mana, and referenced by no
 *     item or creature, so binding a script to it reaches nothing but the sigil.
 *     The guard below checks the cast item regardless.
 *
 * AND IT LOOKS LIKE SOMETHING. Precast kit 60 and cast kit 61 are byte for byte
 * the pair Immolate uses -- red fire -- plus a ground impact. The first choice,
 * 5017 "Divining Trance", was picked on properties alone and turned out to have
 * precast kit 99: a white-gold holy glow, which is a strange way to go hunting.
 * The kits live in SpellVisual.dbc; the names in this range describe the quest a
 * spell was cut for, not what it looks like, so they are worth nothing here.
 *
 * THE RUNNER-UP IS A LESSON. 18666 "Corrupt Redpath" carries Shadow Bolt's exact
 * kits and looked better in game, and it cannot be used: implicit target 38 is
 * TARGET_UNIT_SCRIPT_NEAR_CASTER and it has no spell_script_target rows, so from
 * an item it fails SPELL_FAILED_BAD_TARGETS. `.cast self 18666` looks perfect,
 * because that command supplies the target the item never will. Test a spell the
 * way the feature will actually cast it.
 *
 * THE REFUSALS ARE NOT THIS FILE'S BUSINESS. BeginInvasion messages the player
 * on every path it declines -- disabled, too low, already invading, in combat,
 * in a sanctuary, hardcore, on cooldown, or simply nobody worth hunting -- so
 * there is nothing to add and no branch worth writing on the result.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Item.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "InvasionMgr.h"

bool ItemUse_item_invasion_sigil(Player* player, Item* item, SpellCastTargets& /*targets*/)
{
    if (!player || !item)
        return true;

    // FALSE hands it to the spell, which is the cast the client is about to
    // draw a bar for.
    if (sInvasionMgr.CanBeginInvasion(player))
        return false;

    // A refusal has already been explained in the chat frame, so all that is
    // left is to release the client -- nothing is casting on this path. BOTH
    // signals HandleUseItemOpcode sends on a failed item cast, one branch above
    // where this script is called from: the equip error frees the greyed item,
    // and the cast result stops the cast. With the first alone the client sat
    // in the cast with its hands glowing (a GM in GM mode, 2026-10-10).
    // DONT_REPORT, so no red error joins the chat line that already said why.
    player->SendEquipError(EQUIP_ERR_NONE, item, nullptr);
    for (auto const& use : item->GetProto()->Spells)
    {
        if (use.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE || !use.SpellId)
            continue;
        if (SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(use.SpellId))
            Spell::SendCastResult(player, spellInfo, SPELL_FAILED_DONT_REPORT);
        break;
    }
    return true;
}

// The other end of the cast route, bound by spell_template.script_name on the
// one spell the sigil carries; see sql/custom/076.
struct spell_invasion_search : public SpellScript
{
    void OnSuccessfulFinish(Spell* spell) const override
    {
        if (!spell)
            return;

        Player* player = spell->m_casterUnit ? spell->m_casterUnit->ToPlayer() : nullptr;
        Item* item = spell->GetCastItem();

        // NO CAST ITEM MEANS THIS WAS NOT US. The script is bound to the spell
        // rather than to the item, so anything else that ever casts Divining
        // Trance falls out here instead of starting a hunt.
        if (!player || !item || item->GetEntry() != 100035)
            return;

        sInvasionMgr.BeginInvasion(player);
    }
};

template <class T>
static SpellScript* GetInvasionSpellScript(SpellEntry const*)
{
    return new T();
}

void AddSC_item_invasion_sigil()
{
    Script* newscript = new Script;
    newscript->Name = "item_invasion_sigil";
    newscript->pItemUse = &ItemUse_item_invasion_sigil;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "spell_invasion_search";
    newscript->GetSpellScript = &GetInvasionSpellScript<spell_invasion_search>;
    newscript->RegisterSelf();
}
