# SF6_CostumeSlotsNative (native REFramework plugin)

The in-game part of the costume slots and of the stage slots, in C++. Since 1.9.0 it replaces the two Lua
scripts (`SF6_CostumeSlots.lua`, `SF6_StageSlots.lua`): no Lua state, no garbage collector, and it keeps
running during online matches, where official REFramework stops Lua scripts. It renames the old scripts
to `.lua.old` when it finds them in `reframework/autorun/` (an update made by hand).

The file keeps its old name so that an update overwrites it.

## Driven by events

Nothing is watched from frame to frame. Hooks on the game's own methods only record what happened (hooks
can run on any thread); the work is done on the next `LateUpdateBehavior`, on the game thread, and stops
when it is done:

| Event (hooked method) | Work |
|---|---|
| boot (first frames, once) | colour records of the slots, their select screen data and colour squares, DriveTech's folders and colours, ownership |
| title screen destroyed (`app.menu.UIFlowTitle.onDestroy`) | ownership given back (the game clears DLC ownership at login) |
| character select started (`app.battle.bBattleFighterSelectFlow.start`, `app.UIFlowUI10501.Start`) | ownership, save safety net |
| Battle Settings shown (`app.UIFlowMatchingSetting.Param.CreatedObject` / `ShowedObject`) | the save shows the slot chosen there |
| Battle Settings closed (`HidObject`, `OnEnd`; only after an open) | the slot becomes the intent, the save gets DriveTech (what other players see) |
| an outfit's visual manifest created (`app.battle.assets.FighterVisualHolder..ctor`: the game mounted its folder), outside the character select screen | for two seconds (the loading screen), for each character with an intent: once DriveTech's manifest is there, the slot's folder is mounted, its manifest copied over DriveTech's, the colour placed under a colour DriveTech has |
| Battle Settings shown, character select started (between matches) | the colours of the last alias put back |
| stage select shown / hidden, focus or preview changed (`app.menu.UIFlowStageSelect.Param`) | the variant's name between UP / DOWN hints, its preview |
| UP / DOWN on the stage select agent (`app.UIAgent.InputUp` / `InputDown`) | next / previous variant, `state.json` for the loader |
| VS screen activated or rewritten (`app.esports.VSInfoOffline`) | the variant's name and image |

Without stage mods, the stage hooks are not installed; without costume slots, the costume ones are not. A
fight triggers none of these events: during a fight the plugin reads nothing but its own memory.

## Files

- `reframework/data/SF6_Costumes_Data/registry.json` (written by the loader): slots, record ids, colour squares.
- `reframework/data/SF6_CostumeSlots_data/state.json`: the slot and colour chosen in Battle Settings, per character.
- `reframework/data/SF6_StageSlots_Data/registry.json` (written by the loader) and `state.json` (the chosen variants,
  watched by the loader).
- Log: `reframework/data/SF6_CostumeSlots_data/native_log.txt` (the previous one is kept as `native_log.old.txt`).

## Build

`build.bat` at the root of the repository builds it with the rest, into `build\SF6_CostumeSlotsNative.dll`.
Copy it into `reframework\plugins\` **with the game closed**. Headers: REFramework plugin API v1.5.8
(`include/reframework`), `/std:c++20 /EHa /W4`.

## Known limits

- Mirror match online: when the opponent has DriveTech of the same character, the first DriveTech found is used.
- `via.Folder.activate` / `deactivate` are not usable as events: the engine mounts folders without going through
  them (a hook on them sees only the plugin's own calls). The manifest's constructor is.
