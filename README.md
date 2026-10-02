<p align="center"><img src="logo.png" width="160" alt="LonelyIce"></p>

# LonelyIce

Your own World of Warcraft 3.3.5a (Wrath of the Lich King) world, for playing alone or with a few friends,
in one application. LonelyIce sets everything up from the game client you already have, runs the server in
the background and starts the game with one button.

## Getting started

1. Download a release and unpack it anywhere, for example next to your game folder.
2. Run `LonelyIce.exe`. The setup wizard finds your game, prepares the server data from it and creates your
   account. The first setup takes from twenty minutes to an hour, depending on your computer.
3. Press **Play**.

You need a World of Warcraft 3.3.5a client (build 12340). LonelyIce does not include or download any game
data.

For setup over SSH, use `LonelyIce -nw`: the terminal interface configures the server, plugins and storage.
Start the configured server separately with `LonelyIce --headless`.
See [Terminal setup and headless servers](docs/terminal.md).

## What you get

- **One button to play**: LonelyIce starts the server, points the game at it and launches the game.
- **A world that feels alive**: server features come as plugins, which you can add, remove and configure.
- **Settings in the interface or YAML**: experience and drop rates, difficulty, world options and plugin settings
  in plain words. Share the plugin list and settings as `server.yaml`, with machine-specific overrides in `local.yaml`.
- **Game master tools**: common server commands as simple forms, account management, and a console for the
  rest.
- **Runs quietly in the tray**: closing the window keeps your world running; start, stop and restart from the
  tray menu.
- **Automatic backups** of your characters and your world, as often as every hour: only what changed takes
  space, old backups thin out by themselves within the disk space you allow, and any of them can be restored
  with two clicks.

## Building from source

See [docs/building.md](docs/building.md). Plugin authors: [docs/plugin-format.md](docs/plugin-format.md).

## Support

LonelyIce is free, with no ads and no paid features. If it is useful to you, you can
[buy me a coffee](https://buymeacoffee.com/darthgelum): it pays for the server, code signing and development time.

<a href="https://buymeacoffee.com/darthgelum"><img src=".github/buy-me-a-coffee.png" alt="Buy me a coffee" width="303"></a>

## License

GNU General Public License v2.0 or later, see [LICENSE](LICENSE).

### Blizzard Entertainment

World of Warcraft®, Warcraft®, Wrath of the Lich King® and Blizzard Entertainment® are trademarks or registered
trademarks of Blizzard Entertainment, Inc. in the U.S. and/or other countries.

The game and everything in it belong to Blizzard Entertainment, Inc.: the game client and its program files, data
files and archives, maps and terrain, models, textures, art, animations, interface, music, sounds, voices, texts,
names, lore, characters, creatures, spells, items, quests and every other part of the game. All of it remains
Blizzard's property wherever it appears, including the data a server extracts from your client on your own computer
(game tables, maps, collision and navigation data).

LonelyIce is an unofficial, non-commercial fan project. It is not affiliated with, endorsed, sponsored, approved or
supported by Blizzard Entertainment, Inc. Blizzard's names are used only to say which game client the project works
with.

This repository contains no files from the game client, and LonelyIce neither distributes nor downloads any. It works
only with a copy of the game you already own. Keep the data extracted from your client to yourself: it is Blizzard's
property and is not ours or yours to share.

The license above covers only the code and files of this project and the works it is based on. It grants no rights
to anything that belongs to Blizzard Entertainment, Inc. All other trademarks belong to their respective owners.
