# SF6 Costume Slots Loader

Street Fighter 6 costume mods replace an existing outfit: install a Ryu mod and Outfit 1 is gone until you
uninstall it. This loader turns every installed costume mod into an **extra** outfit slot and puts the original
outfit back, so Outfit 1 and the mod live side by side in the costume spinner.

Install it once. After that nothing changes in how you use the game: install costume mods the way you already
do, start the game, and the new outfits are there.

## Install

Grab the archive from [Releases](https://github.com/Wael3rd/SF6-CostumeSlotsLoader/releases), install it in
Fluffy Mod Manager or copy it over your Street Fighter 6 folder, then install costume mods as usual. You need
REFramework in `dinput8.dll`, official build 1.5.8 or newer.

Costume mods can also go straight into `reframework\costume_mods\<Character>\`, **as downloaded**: the
`.zip`, `.7z` or `.rar` file itself, or an unpacked folder with a `natives` tree or a `.pak`. Nothing to
unpack, nothing to convert.

## Checking what is installed

Double-click `reframework\costume_mods\SF6_CostumeAudit.bat`. It changes nothing: it checks that the loader,
REFramework, the Lua script and the native plugin are in place, says what the last launch did, lists the
costume mods it finds (Fluffy Mod Manager paks in the game folder, archives and folders in `costume_mods`)
with the outfits each one gave, then the extra outfits by character. The report is saved as
`SF6_CostumeAudit.txt` next to it: send it with `SF6_CostumeLoader.log` when reporting a problem.

## Archives, bundles and add-ons

- **Archives** are recognised by their content, not their name, and unpacked once into the hidden
  `costume_mods\.cache` folder, then reused until the archive changes. Only what the game needs is
  kept. The first launch after adding a large archive takes a few extra seconds; the next ones don't.
- **Bundles**: an archive or folder holding several options gives one outfit slot per option.
- **Add-ons**: an option that only changes a part (no gloves, other hair, a weapon) is combined with the
  outfit it completes, the way Fluffy Mod Manager installs them together. The loader reads Fluffy's
  `addonfor` and bundle names; without them it matches options of the same archive that touch the same
  outfit. Each outfit gets one variant per add-on, plus one with all of them.
- **Combining two downloads**: a costume and an add-on published separately (a hair, a weapon) are combined
  when both archives sit in one sub-folder of the character, `costume_mods\<Character>\<any name>\`. The
  add-on must change the same original outfit as the costume.
- **Modular mods** (main files, body options, skin options, optional extras) give one outfit per body
  option with the default skin, as Fluffy installs them by default.
- A mod that only changes a part of an original outfit (glasses, earrings) gives that outfit with the
  change, in a slot of its own.
- Variants share most of their files, and the generated pak stores identical data only once.
- Broken, password-protected or unrelated archives are skipped and reported in `SF6_CostumeLoader.log`.

## How it works

The game imports `amd_ags_x64.dll` from its own folder, so a proxy with that name is mapped into the process
before any engine code runs. That is the moment the loader does its work:

1. It scans the installed costume mods, both the Fluffy Mod Manager patch paks and the folders under
   `reframework/costume_mods/`.
2. It rebuilds a patch pak that relocates each modded costume to a free slot, from 5 to 104, which is
   **100 extra slots per character**, and restores the outfit the mod had overwritten from the base pak.
3. Colours, physics chains, shared body parts, weapons and the `streaming/` high resolution textures follow the
   costume to its new slot. Files made before a game update are brought up to date on the way (see below).
4. It reads every slot back the way the game will load it, repairs what cannot load, and leaves out a slot it
   cannot repair instead of letting it stall the game. Only the slots in use are declared to the game.
5. It writes a registry that the Lua script and the native plugin read to make the slots selectable and, online,
   to show the other players a legal outfit.

A fingerprint of the installed mods is kept, so a launch with nothing changed costs about 20 ms. A launch after
installing or removing a mod takes a few seconds, up to about fifteen with many large archives.

## Compatibility

Mods published since 2023 were made for older versions of the game. The loader recognises the known
differences (old texture suffixes, colour files in older layouts, broken texture headers, parts whose
materials no longer match) and fixes them in its own pak, without touching the mod files.
[COMPATIBILITY.md](COMPATIBILITY.md) lists what it fixes and the mods tested; [TESTING.md](TESTING.md)
lists what still needs testing.

Online, the extra slot is aliased to a DriveTech outfit for the other players, so matchmaking stays valid. The
menus are handled by the Lua script and the match itself by the native plugin, because official REFramework stops
Lua scripts during online matches but not native plugins.

## Repository layout

```
loader/       the loader itself: KPKA pak reader and writer, scene and material patching, AGS proxy
plugin/       native REFramework plugin, used during online matches
script/       Lua script, used in menus (slot selection, colours, ownership)
tools/        generators for the data files, run against your own game installation
audit/        SF6_CostumeAudit.bat / .ps1, shipped in costume_mods: what is installed, for users
packaging/    builds the distributable zip
```

The loader DLL has no hooks, no user interface and no scripting. It imports `kernel32`, `bcrypt` (to hash the
mod fingerprint), and `advapi32` and `user32` for the archive decoders; the game has all four loaded already.
Every AGS export is forwarded to `amd_ags_x64_real.dll`, the genuine AMD library that ships with the game.

## Building

Visual Studio 2022 with the C++ toolchain, then:

```
loader\build.bat     produces amd_ags_x64.dll, costume_loader.exe, paktool.exe, loadtest.exe
plugin\build.bat     produces SF6_CostumeSlotsNative.dll
```

Both are compiled with `/MT` and `/EHa`. Structured exception handling must not be disabled: the loader runs
inside `DllMain` and swallows any fault rather than taking the game down with it.

## Data files

The loader needs two data files that are **derived from your own copy of the game** and are deliberately not
distributed here:

- `vanilla_costume_index.tsv`, an index of the original costume files, built by `tools/build_vanilla_inventory.py`.
- the static structural tables that declare the extra slots, built by `tools/make_static_structural.py`.

Both scripts read the game paks through [REasy-parser](https://github.com/seifhassine/REasy). Point the
`REASY_PARSER` environment variable at your checkout before running them.

## Installing another proxy alongside

Only one `amd_ags_x64.dll` can exist in the game folder. If another tool already uses that entry point, rename
its DLL to `amd_ags_x64_chain.dll` and keep the genuine AMD library as `amd_ags_x64_real.dll`. The loader then
loads the chained module from a separate thread and logs it in `SF6_CostumeLoader.log`. Without that file, the
loader loads nothing else.

## Requirements

REFramework in `dinput8.dll`, official build 1.5.8 or newer.

## Credits and licence

Written by Wael. Released under the MIT licence, see `LICENSE`.

Third party code is vendored under `loader/third_party/` and `plugin/include/`: the zstd decompressor, miniz,
the LZMA SDK 7z decoder, UnRAR and the REFramework plugin API. See `THIRD_PARTY.md` for their licences.

This repository ships no game data: the tables the loader needs are generated from your own installation,
and the release archive carries a prebuilt copy of them. Street Fighter 6 is a trademark of Capcom.
