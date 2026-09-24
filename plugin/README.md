# SF6_CostumeSlotsNative

Native REFramework plugin that ports the "in-match" part of `SF6_CostumeSlots.lua`
into a DLL loaded by the plugin loader (not cut off during online matches).

## Role

When the player has chosen a costume slot (5+), the Lua writes DriveTech (costume 4)
into the save data for the network. This plugin, during the match:

1. Reads `state.json` (slot + colour per character) and `registry.json` (ownership).
2. Finds the v04 folder (DriveTech) in the scene tree by suffix.
3. Waits for the game to mount v04 (FighterVisualHolder holder present).
4. Activates the slot's folder (`via.Folder.activate`).
5. Copies the slot's visual manifest (`SettingData.overwrite`) onto v04.
6. If the colour is not one of DriveTech's base colours, swaps the
   CCVD data (`costumeColorVariation.Colors[ColorId].Data`) base <-> desired.
7. Restores the colours when the v04 holder disappears.

## Files exchanged

| File | Read/Write | Description |
|---------|-----------------|-------------|
| `data/SF6_CostumeSlots_data/state.json` | R | Per-character intent (slot, color) |
| `data/SF6_Costumes_Data/registry.json` | R | Owned slots (ownership array) |
| `data/SF6_CostumeSlots_data/native_present.json` | W (every 2s) | Plugin presence (`{ts, pid}`) |
| `data/SF6_CostumeSlots_data/native_log.txt` | W (append) | Timestamped log (max 1 MB) |

## Coexistence with the Lua

The plugin writes `native_present.json` every 2 seconds. The Lua checks this
file (at most 1x/s): if `ts` is less than 10 s old, it skips `keep_mounted`,
`swap_manifest` and the colour swap (logged as "alias delegated to the native plugin").

The Lua keeps: Battle Settings menus, save data writing, DLC ownership,
colours in TableDataManager, SelectFighterUIData badges.

## Build

```
"C:\...\reframework\plugins\native\costumeslots\build.bat"
```

Output: `out\SF6_CostumeSlotsNative.dll`. Copy into `reframework\plugins\`
**with the game closed**.

## Compatibility

- API headers: v1.5.8 (`REFRAMEWORK_PLUGIN_VERSION_MINOR = 10`)
- REFramework: 1.5.8, 1.5.9, nightly (loads on any runtime >= minor 10)
- Compiled with `/EHa` (project rule 17)

## Known limitations

- **Mirroring**: if the opponent also has DriveTech for the same character, their v04
  holder could be the one found first. The plugin takes the first v04 (same as the Lua).
- **Replay / spectator**: untested. The plugin does nothing if `state.json`
  has no active slot.
- The plugin does not create DLC ownership or colour records; that's the
  Lua's (or the loader's) job at startup.
