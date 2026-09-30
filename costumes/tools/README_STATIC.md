# Static Structural Files -- Costume Slot Loader

## Purpose

These pre-built binary files contain ALL the RSZ/MSG data the costume loader
needs.  The C++ port ships them as-is in the output pak -- no RSZ parser or
MSG handler required at runtime.

## Files

| File | Size | Content |
|------|------|---------|
| `esf.scn.20` | 31120 B | Vanilla + 108 `via.Folder` entries (18 base fighters x 6 slots v05..v10) |
| `fightercostumeuserdata.user.2` | 21248 B | Vanilla 134 records + 186 new (31 fighters x 6 slots) |
| `fightercostumemessage.msg.21` | 9420 B | Vanilla 5 messages + 6 new ("Outfit I" .. "Outfit VI") |
| `static_meta.json` | ~12 KB | ID lookup table for the C++ loader |

## ID allocation

| Range | Values | Purpose |
|-------|--------|---------|
| Costume record id | 5000..5185 | One per (fighter, costumeNo 5..10). Formula: `5000 + fighter_index*6 + (costumeNo-5)` |
| Costume ManageId | 5000..5185 | Tracks 1:1 with record id |
| FighterCostumeMessage id | 5000..5005 | 6 shared messages, one per slot offset |
| sortNo | 101..286 | Ascending, one per record |
| Message GUIDs | `a1b2c3d4-NNNN-4000-8000-0000000500NN` | Fixed, deterministic |

**Reserved (do not collide):**
- Vanilla record ids: 1..148
- Vanilla color ids: 1..1872, ManageId 1..1864
- Dynamic color ids (if added by Lua): start at 1873+

## `isDefault = false` hypothesis

All 186 new records have `isDefault = false`.  The hypothesis (to test in game):

1. **Boot with 108 folders whose scenes don't exist**: the game resolves each
   `via.Folder`'s `ScenePath` at load time.  If the scene file is not in any
   pak, the folder is silently ignored (confirmed for the 82 vanilla folders).
   With 108 new folders whose scenes are only created when a mod is installed,
   the game should boot without issue.

2. **Non-owned slots are invisible**: `isDefault = false` means the costume is
   not "owned" by default.  In vanilla, only DLC costumes use `isDefault = false`
   until purchased.  The costume selector should hide these slots unless
   possession is granted at runtime.

3. **Hot possession via Lua**: the Lua script reads `registry.json`, finds which
   slots are active (have a mod installed), and sets `isDefault = true` on
   those records at runtime via the managed object API.  The costume selector
   updates on the next menu refresh.

## fighter_index mapping

```
Index  FighterID  Name
  0        1      Ryu
  1        2      Luke
  2        3      Kimberly
  3        4      Chun-Li
  4        5      Manon
  5        6      Zangief
  6        7      JP
  7        8      Dhalsim
  8        9      Cammy
  9       10      Ken
 10       11      DeeJay
 11       12      Lily
 12       13      AKI
 13       14      Rashid
 14       15      Blanka
 15       16      Juri
 16       17      Marisa
 17       18      Guile
 18       19      Ed
 19       20      E.Honda
 20       21      Jamie
 21       22      Akuma
 22       25      Sagat
 23       26      Bison
 24       27      Terry
 25       28      Mai
 26       29      Elena
 27       30      C.Viper
 28       31      Alex
 29       32      Ingrid
 30       33      Yasmine
```

## Regeneration

If the game adds new fighters or changes the costume table format:
```
python make_static_structural.py
```
This re-reads the vanilla pak and regenerates all 3 files + metadata.
