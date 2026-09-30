# Real Scenario Test -- Costume Slot Loader

## What is in this directory

| File | Role | Source |
|------|------|--------|
| `re_chunk_000.pak.patch_001.pak` | Vagrant mod (simulates Fluffy install) | `--make-fluffy-pak` from vagrant directory |
| `re_chunk_000.pak.patch_002.pak` | Aloha C1 mod (simulates Fluffy install) | `--make-fluffy-pak` from aloha directory |
| `re_chunk_000.pak.patch_003.pak` | Costume slot loader output (our pak) | Production mode scan of patch_001 + patch_002 |

## What patch_003 contains

- **Vagrant** relocated to `esf001/006/` as costume v05 ("Outfit I", record 149)
- **Aloha C1** relocated to `esf001/007/` as costume v06 ("Outfit II", record 150)
- Patched `esf.scn.20` with 2 new `via.Folder` entries (esf001v05, esf001v06)
- Patched `fightercostumeuserdata.user.2` with 2 new costume records
- Patched `fightercostumemessage.msg.21` with 2 new message entries
- Patched `fightercostumecoloruserdata.user.2` with 20 color entries (10 per slot)
- 18 vanilla file restorations for `esf001/001/` (Ryu Outfit 1 restored)
- 1 mod-only shared texture addition (`000/01/ryubod_nrrc.tex`)
- Marker `natives/stm/sf6_costume_slots.marker`
- 166 total entries, 255 MB

## Install procedure (game must be closed)

1. **Back up** the existing test pak at the game root:
   ```
   cd "C:\Program Files (x86)\Steam\steamapps\common\Street Fighter 6"
   move re_chunk_000.pak.patch_001.pak ..\reframework\agent\tmp\costume_slot_test\manual_patch_001.pak.bak
   ```

2. **Copy** the 3 paks from this directory to the game root:
   ```
   copy out\real\re_chunk_000.pak.patch_001.pak .
   copy out\real\re_chunk_000.pak.patch_002.pak .
   copy out\real\re_chunk_000.pak.patch_003.pak .
   ```

3. **Launch** the game.

## What to verify in-game

1. **Ryu Outfit 1** (vanilla) is restored -- no Vagrant/Aloha visual
2. **Ryu costume selector** shows 2 new entries after the last vanilla outfit:
   - "Outfit I" = Vagrant (relocated to folder 006)
   - "Outfit II" = Aloha C1 (relocated to folder 007)
3. Each new outfit has **10 color slots** in the color picker
4. All 3 model parts (head, body, hair) load without missing textures
5. The character select screen shows the correct preview for each outfit

## Uninstall

Delete patch_001, patch_002, patch_003 from the game root.
Restore the manual test pak if desired:
```
move ..\reframework\agent\tmp\costume_slot_test\manual_patch_001.pak.bak re_chunk_000.pak.patch_001.pak
```

## Known limitations (production mode)

6 pak entries from Vagrant could not be attributed to a path (hash-only paks
do not store paths). Breakdown:
- 2 `light/*.pfb` files (character select lighting -- non-model, not relevant to costume slots)
- 1 `weather/*.chain` file (wind physics -- non-model, not relevant)
- 3 `000/02/halbd.tex` etc. (short-named textures the mod ships but no mdf2 references by that name)

These are all non-functional for the costume slot system. The first 3 are
character-global files that only matter if Fluffy is used (the mod pak
serves them directly). The last 3 appear to be unreferenced bonus files.

In `--mod-dir` mode (where paths are known from disk), all files are handled.
