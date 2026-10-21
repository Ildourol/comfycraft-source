#ifndef MANGOS_MOUNT_RENTAL_H
#define MANGOS_MOUNT_RENTAL_H

#include "Common.h"

class Player;

/*
 * PUTS A PLAYER ON A MOUNT, FOR A TIME AND A PRICE. The stables
 * (`rented_mount`, random_scripts_1.cpp) and the dialog editor's "rents a
 * mount" action (SCRIPT_COMMAND_RENT_MOUNT, 102) both come through here, so a
 * fix to one is a fix to both. docs/ai/content/rented-mounts.md has the long
 * form of every rule below.
 *
 * `spellId` is a real mount spell, cast on the player by the player. It brings
 * the model, the name and the icon, so no client file is needed.
 *
 * `durationMs` 0 leaves the spell as it is: a real mount spell is permanent and
 * runs at the speed from riding skill. Anything else puts a timer on it, and a
 * mount with a timer gets the rental speed by level (SpellAuras.cpp).
 *
 * The money comes out only after the mount lands. A refusal tells the player
 * why, in the red error line, and costs nothing.
 */
enum MountRentalResult
{
    MOUNT_RENTAL_OK,
    MOUNT_RENTAL_ALREADY_MOUNTED,
    MOUNT_RENTAL_NO_MONEY,
    MOUNT_RENTAL_NOT_A_MOUNT,
    MOUNT_RENTAL_REFUSED            // the cast failed: indoors, in combat, shapeshifted
};

// The longest timer SCRIPT_COMMAND_RENT_MOUNT accepts, one day. The website's
// MAX_MOUNT_MINUTES in lib/dialogs.js is the same number.
uint32 const MAX_RENT_MOUNT_MINUTES = 1440;

MountRentalResult RentMount(Player* player, uint32 spellId, uint32 durationMs, uint32 cost);

#endif
