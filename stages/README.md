# SF6 Stage Slots

Street Fighter 6 stage mods replace a stage: install a Training Room mod and the original Training Room is
gone until you uninstall it, and two mods for the same stage cannot live together. Stage Slots keeps the
original and every mod side by side: on the stage select screen, **UP / DOWN** cycles the focused stage
through its original look and each mod installed for it. The name and the preview image follow, the VS
screen shows the choice, and the battle loads it.

Nothing is installed over the game's files: each mod's files are copied into a patch pak under names of
their own, and served in place of the original files only while that mod is selected.

## Install and use

Stage Slots ships inside the costume loader's `amd_ags_x64.dll` (one DLL for both, see *How it works*),
plus a Lua script. You need REFramework in `dinput8.dll`.

| File | Where |
|---|---|
| `amd_ags_x64.dll` (costume loader + stage slots) | game folder |
| `script/SF6_StageSlots.lua` | `reframework/autorun/` |
| `data/loader/stage_paths.txt` | `reframework/data/SF6_StageSlots_Data/loader/` |

Put stage mods **as downloaded** in `reframework/stage_mods/`: the `.zip`, `.7z` or `.rar` file itself, a
folder with a `natives` tree, or a `.pak`. Launch the game.

- Each option of a mod becomes one variant of the stage it changes. A pack covering many stages (Stage
  Lighting Overhaul: 24 options) gives one variant per stage and option.
- On the stage select screen, a stage that has variants shows its name between the UP / DOWN symbols of
  the device in use (keys on a keyboard, directions on a pad), the way the game's BGM selector frames its
  value. UP / DOWN changes the variant; the choice is kept per stage
  (`reframework/data/SF6_StageSlots_Data/state.json`).
- Names come from the mod's `modinfo.ini` (`name=`). A variant named like the stage itself (a lighting pack
  names its options "Bather's Beach") shows the pack name (`nameAsBundle=`) instead.
- Previews are made from the mod's screenshot (`screenshot=` in `modinfo.ini`, or the first image of the
  folder): a 4:1 band at 60% of the height, 2048x512, BC1 like the game's own. A mod without an image keeps
  the stage's original preview.
- Variants can change from one battle to the next without restarting the game.

## Limits

- **The choice is local.** The game only knows the vanilla stage id, so another player sees their own version
  of the stage. The redirection is native and stays active online; online play has not been tested.
- **Stage mods installed through Fluffy Mod Manager are not variants yet**: their pak replaces the stage for
  everyone, the loader does not restore the original under it.
- **Adding or removing a stage mod makes the costume loader rebuild its pak once** (about 50 s with many
  costume mods), because the stage pak sits below it in the patch order.
- A mod that hangs the game on its own hangs it here too (seen: *Aokigahara - no NPC* stays on the VS screen,
  also when installed as a plain Fluffy pak).

## How it works

The game imports `amd_ags_x64.dll` from its own folder, before any engine code runs. The DLL is the costume
loader's proxy, built from its sources unchanged, with the stage slots around it:

1. **Stage pass** (`loader/stage_loader.cpp`, DllMain). Reads `reframework/stage_mods`, unpacks archives once
   into `stage_mods/.cache`, and recognises the stage of every file with `stage_paths.txt` (every game path
   holding a stage code `essNNNN_NN`, hashed at startup). Each file of a variant is stored in our patch pak
   under `pak_path_hash("natives/stm/_stageslots/<variant>/<vanilla hash>")`, next to its preview texture.
   The stage pak goes right above the mod paks and below the costume pak (marker
   `natives/stm/sf6_stage_slots.marker`). Nothing changed since last launch: the saved table is reused
   (a few ms).
2. **Costume pass**, unchanged.
3. **Redirection** (`loader/stage_redirect.cpp`). The engine turns every file path into a pak hash through one
   function, `path_to_hash`. A small block of memory is reserved near the executable at startup; once a
   variant is selected (and never in the first 20 s), the entry of `path_to_hash` gets a 5-byte jump to our
   detour, which swaps the hash of each vanilla file the selected variant replaces for its copy's hash. A
   tool that hooked the function first (HARD READ) stays in the chain. The selection is re-read from
   `state.json` when it changes.
4. **Lua script** (`script/SF6_StageSlots.lua`). Counts UP / DOWN on the stage select agent, writes
   `state.json`, and shows the variant: name and preview on the stage select screen, name and image on the
   VS screen. Game thread only (`LateUpdateBehavior`), no text written twice.

Outputs, in `reframework/data/SF6_StageSlots_Data`: `registry.json` (variants per stage, for the script),
`loader/variants.tsv` (the same with every redirection), `state.json` (selection). Log:
`SF6_StageSlots.log` in the game folder.

## Building

Visual Studio 2022 or Build Tools with the C++ workload. The costume loader's sources and its
`archive_deps.lib` are needed (`COSTUME_SRC`, by default the development copy in
`reframework/SF6_CostumeLoader`).

```
loader\build.bat        build\amd_ags_x64.dll
tools\build_tools.bat   build\stagepak.exe, textest.exe, stagetest.exe
```

- `stagetest <folder>` runs the stage pass on a folder laid out like the game's (no game needed).
- `textest <image> <out.tex>` makes a preview; `textest --dump <in.tex> <out.png>` decodes one.
- `tools/make_stage_index.py` rebuilds `data/loader/stage_paths.txt` from REasy-parser's `SF6_STM.list`.

Development notes (engine facts, test status, open items): [docs/DEV_NOTES.md](docs/DEV_NOTES.md).

## Third party

`loader/third_party`: stb_image, stb_image_write, stb_dxt (public domain), bcdec (MIT, test tool only). The
costume loader's pak, archive and decompression code is compiled from its own repository.
