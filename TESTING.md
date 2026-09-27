# What still needs testing

Everything below was either fixed without being seen in game yet, or never exercised on a real
installation. Each item says what to do and what to look for. `SF6_CostumeLoader.log` sits in the game
folder; the loader rewrites it at every launch where something changed.

## Costumes fixed offline, to look at in game

- **Cammy, DOA4 Christie**: every colour (01 to 10) must show its colours, not a white outfit.
- **A.K.I., TFD Racer**: same check.
- **Ken, Alpha Ken (Ripped Gi)**: loads and looks like the mod's screenshot.
- **Lily, Nico bundle, Outfits I and II**: body not see-through (Outfits III and IV are confirmed).
- **Ryu, Aloha bundle**: each of the 12 variants, including the no-gloves and no-headband ones.

## Loader paths never triggered by a real mod

- **A slot left out**: no installed mod has needed it so far. When one does, the log shows
  `WARN: slot ... left out, the game could not load it: <reason>` and that outfit is simply absent.
  Report the mod and the log.
- **The pak in use**: launch the game right after closing it, while the previous process is still
  exiting. If it does not let go within about ten seconds, the log shows
  `ERROR: ... in use by another Street Fighter 6 process` and the costumes stay as they were; the next
  launch must regenerate them. Tested on the bench only.
- **Removing a mod**: its outfit must disappear on the next launch, without an empty entry left behind.

## Installation and first launch

- **Fluffy Mod Manager**: install the release archive through Fluffy on a clean game, then add costume
  mods both through Fluffy and as archives in `reframework\costume_mods\<Character>\`.
- **First launch with many archives**: the archives are unpacked once into `costume_mods\.cache`.
  Note how long the first launch takes; antivirus scanning can make it much longer.
- **Characters owned or not**: a character you do not own (trial) must list only the installed mods'
  outfits, and none of them must stall.

## Online

- **Casual match with a modded outfit**, on official REFramework 1.5.8: the match must start and the
  opponent must see a Drive Tech outfit. Last confirmed before the changes of 2026-09-27.
- **Your opponent's modded outfit**, mirrored as Drive Tech on your side: not tested.
- **Replays and spectator mode** with modded outfits: not tested.

## More mods

Any costume mod not in [COMPATIBILITY.md](COMPATIBILITY.md) is untested. Mods from 2023 are the most
useful to try, since they are the likeliest to be affected by game updates.
