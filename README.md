# SF6 Costume Slots Loader

Street Fighter 6 costume mods replace an existing outfit: install a Ryu mod and Outfit 1 is gone until you
uninstall it. This loader turns every installed costume mod into an **extra** outfit slot and puts the original
outfit back, so Outfit 1 and the mod live side by side in the costume spinner.

Install it once. After that nothing changes in how you use the game: install costume mods the way you already
do, start the game, and the new outfits are there.

## Install

Grab the archive from [Releases](https://github.com/Wael3rd/SF6-CostumeSlotsLoader/releases), install it in
Fluffy Mod Manager or copy it over your Street Fighter 6 folder, then install costume mods as usual. You need
REFramework in `dinput8.dll`, official build 1.5.8 or newer. Costume mods can also be dropped straight into
`reframework\costume_mods\<Character>\<Costume name>\`, as a `natives` tree or a `.pak`.

## How it works

The game imports `amd_ags_x64.dll` from its own folder, so a proxy with that name is mapped into the process
before any engine code runs. That is the moment the loader does its work:

1. It scans the installed costume mods, both the Fluffy Mod Manager patch paks and the folders under
   `reframework/costume_mods/`.
2. It rebuilds a patch pak that relocates each modded costume to a free slot, from 5 to 104, which is
   **100 extra slots per character**, and restores the outfit the mod had overwritten from the base pak.
3. Colours, physics chains, shared body parts and the `streaming/` high resolution textures follow the costume
   to its new slot.
4. It writes a registry that the Lua script and the native plugin read to make the slots selectable and, online,
   to show the other players a legal outfit.

A fingerprint of the installed mods is kept, so a launch with nothing changed costs about 20 ms. A launch after
installing a mod costs about a second.

Online, the extra slot is aliased to a DriveTech outfit for the other players, so matchmaking stays valid. The
menus are handled by the Lua script and the match itself by the native plugin, because official REFramework stops
Lua scripts during online matches but not native plugins.

## Repository layout

```
loader/       the loader itself: KPKA pak reader and writer, scene and material patching, AGS proxy
plugin/       native REFramework plugin, used during online matches
script/       Lua script, used in menus (slot selection, colours, ownership)
tools/        generators for the data files, run against your own game installation
packaging/    builds the distributable zip
```

The loader DLL has no hooks, no user interface and no scripting. Its only imports are `kernel32` and `bcrypt`,
the latter to hash the mod fingerprint. Every AGS export is forwarded to `amd_ags_x64_real.dll`, the genuine AMD
library that ships with the game.

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

Third party code is vendored under `loader/third_party/` and `plugin/include/`: the zstd decompressor, miniz and
the REFramework plugin API. See `THIRD_PARTY.md` for their licences.

This project ships no game data of any kind. Street Fighter 6 is a trademark of Capcom.
