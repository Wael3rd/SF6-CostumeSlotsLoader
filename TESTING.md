# What still needs testing

Everything below was either fixed without being seen in game yet, or never exercised on a real
installation. Each item says what to do and what to look for. `SF6_CostumeLoader.log` sits in the game
folder; the loader rewrites it at every launch where something changed.

## Confirmed in game on 2026-09-27

- Colours of Cammy DOA4 Christie and A.K.I. TFD Racer, all 10 colours.
- Alpha Ken (Ripped Gi), Lily Nico Outfits I and II, the 12 Aloha Ryu variants one by one.
- Replays with modded outfits.
- Online casual match with Ryu Outfit I (a mod), on official REFramework 1.5.8.

## Fixed, to confirm in game

- **Removing a mod.** Before the fix, removing Alpha Ken (Ripped Gi) left Ken's character select on a
  missing outfit and navigation stalled. Now every unused slot number shows the character's Outfit 1,
  and names stay contiguous (after removing one of Ken's four mods: Outfit I, II, III). Remove a mod
  whose outfit was the last one selected, launch, open the character select: no stall, and the names
  follow each other.

## Open problems

- **Online with Ken's Vegeta Majin slot: network error**, while Ryu Outfit I works. The native plugin
  never saw the game load Ken's Drive Tech outfit, so the error comes before the match loads. To narrow
  it down: the moment of the error (search, opponent found, versus screen), and whether another Ken
  mod (Alpha Ken) fails the same way.

## Loader paths never triggered by a real mod

- **A slot left out**: no installed mod has needed it so far. When one does, the log shows
  `WARN: slot ... left out, the game could not load it: <reason>` and that outfit is simply absent.
  Report the mod and the log.
- **The pak in use**: launch the game right after closing it, while the previous process is still
  exiting. If it does not let go within about ten seconds, the log shows
  `ERROR: ... in use by another Street Fighter 6 process` and the costumes stay as they were; the next
  launch must regenerate them. Tested on the bench only.

## Installation and first launch

- **Fluffy Mod Manager**: install the release archive through Fluffy on a clean game, then add costume
  mods both through Fluffy and as archives in `reframework\costume_mods\<Character>\`.
- **First launch with many archives**: the archives are unpacked once into `costume_mods\.cache`.
  Note how long the first launch takes; antivirus scanning can make it much longer.
- **Characters owned or not**: a character you do not own (trial) must list only the installed mods'
  outfits, and none of them must stall.

## Online, not tested yet

- **Your opponent's modded outfit**, mirrored as Drive Tech on your side.
- **Spectator mode** with modded outfits.

## More mods

Any costume mod not in [COMPATIBILITY.md](COMPATIBILITY.md) is untested. Mods from 2023 are the most
useful to try, since they are the likeliest to be affected by game updates.
