# ComfyCraft source

**Bugs, questions and screenshots: [join our Discord](https://discord.gg/uhefX2efB7).**

[![Discord](https://img.shields.io/badge/Discord-ComfyCraft-5865F2?logo=discord&logoColor=white&style=for-the-badge)](https://discord.gg/uhefX2efB7)

The server source for ComfyCraft, a small World of Warcraft 1.18.1 server
(build 7272) built around companions and player housing.

This repository is a published copy, updated each time the server is deployed.
Every commit is the source of the server as it ran from that deploy onwards.
Development happens elsewhere, so pull requests here will not be merged.

## What is here

- **`source/`**: the world and login server. A fork of
  [Tortoise WoW](https://github.com/tortoise-wow/tortoise-wow), by way of
  [Shyalya's playerbots branch](https://github.com/Shyalya/tortoise-wow/tree/playerbots-integration-gh),
  with ComfyCraft's changes on top: housing, companions, bot NPCs, custom shops
  and quests, and many fixes.
- **`sql/custom/`**: ComfyCraft's world and character database changes. Apply
  them in filename order after the base database. Each file says in its header
  which database it targets; a file that says nothing targets `tw_world`.

## Building

See `source/INSTALL-LINUX.md` and `source/INSTALL-WINDOWS.md`. ComfyCraft is
built with `-DBUILD_PLAYERBOTS=ON`.

## Licence

`source/` is licensed under the GNU Affero General Public License v3, the
licence of the project it forks (`LICENSE`). Third party code under
`source/dep/` carries its own notices.

## Credits

ComfyCraft stands on the work of the Turtle WoW team, the Tortoise WoW
contributors, and the MaNGOS, vMaNGOS, Elysium, Nostalrius, AzerothCore and
playerbots projects. `source/AUTHORS.md` sets out the lineage.

World of Warcraft is a trademark of Blizzard Entertainment. ComfyCraft is not
affiliated with or endorsed by Blizzard Entertainment.
