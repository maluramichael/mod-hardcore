# mod-hardcore

An [AzerothCore](https://www.azerothcore.org/) module (WotLK 3.3.5a) that adds an opt-in
**Hardcore** ruleset: one death, and it's over.

## What it does

A character opts in itself with a chat command:

```
.hardcore on
.hardcore status
```

There is no `.hardcore off` - opting in is a one-way commitment. Once flagged, the next death
is final. What "final" means is configurable (`Hardcore.DeathPolicy`):

- **`ghost`** *(default, non-destructive)* - the character becomes a permanently locked ghost.
  Resurrection and graveyard release are both blocked forever; the character stays visible,
  stuck exactly where it fell.
- **`archive`** *(non-destructive)* - the character is kicked and, once safely logged out,
  renamed so the original name frees up. It can never be logged into again. All of its data is
  kept.
- **`delete`** *(destructive, must be explicitly configured)* - the character is kicked and,
  once safely logged out, permanently removed from the database. This cannot be undone.

Every death is logged permanently (character, level, map, zone, timestamp), regardless of
policy, and can optionally be broadcast server-wide (`Hardcore.AnnounceDeaths`).

## Configuration

`conf/mod_hardcore.conf.dist`:

| Key                        | Default | Description                                   |
|-----------------------------|---------|------------------------------------------------|
| `Hardcore.Enable`           | `1`     | Master on/off switch                          |
| `Hardcore.DeathPolicy`      | `ghost` | `ghost` \| `archive` \| `delete`              |
| `Hardcore.MinLevelToFlag`   | `1`     | Minimum level required to opt in              |
| `Hardcore.AnnounceDeaths`   | `1`     | Broadcast a server-wide message on death      |

The module creates its own storage tables automatically on first start - no SQL file to
import, no client patch needed.

## Limitations

- Group and playerbot loot/rescue behavior on a Hardcore death is left at engine defaults in
  this version - no special group-wipe or bot-takeover handling yet.
- The death policy in effect for a given character is whatever `Hardcore.DeathPolicy` is set
  to *at the moment it dies*. Changing the setting later does not retroactively change how an
  already-fallen character is handled.

## Pairing with Self-Found

Hardcore is conceptually self-found - a permadeath character should usually also not be able
to lean on trading, the Auction House or mail from other players. This module does not enforce
that itself; install [mod-self-found](https://github.com/maluramichael/mod-self-found)
alongside it and have players opt into both for the full ruleset.

## Installation

Clone into your AzerothCore `modules/` directory and rebuild the worldserver.

## License

Released under the GNU GPL v2 (or later).
