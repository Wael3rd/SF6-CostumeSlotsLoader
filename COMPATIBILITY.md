# Compatibility

Costume mods are made for the game as it was on the day they were published. Capcom has updated the
game many times since June 2023, and a mod that loaded then can fail now. Most of these failures are
not visible as errors: the game waits forever for a resource it cannot load, the preview stays on the
previous character, and every costume shown afterwards stays empty.

The loader therefore does two things at every launch where something changed:

1. **It repairs what is known to go wrong**, in the files it writes to its own pak. The mod files on
   disk are never modified.
2. **It checks every slot the way the game will load it** before writing anything. A part that cannot
   load is rebuilt from the mod's shared version of that part, or from the original outfit; a slot
   that still cannot load is left out rather than shipped, and `SF6_CostumeLoader.log` says which mod
   and why.

## What the loader fixes on its own

| Problem found in a mod | Symptom in game | What the loader does |
|---|---|---|
| Material files point at textures by their original name, the textures move with the slot | Costume white or untextured | Rewrites those references to the relocated names |
| No material file shipped for a part | Wrong textures, see-through body | Takes the original material of **that part** and binds it to the mod's textures |
| Files only the mod has, stored in another outfit's folder | Loading never completes | Keeps them at their original path as well |
| Weapons and props listed outside the costume scene (`model_parts`) | Original weapons on a modded outfit | Gives the slot its own copy, pointing at the mod's meshes |
| Textures with the pre-2024 file suffix (`.tex.143230113`) | Textures ignored | Reads them as the current version |
| Colour files whose type signature predates a game update | Whole costume white, hair included | Brings the signature up to date (the layout did not change) |
| Colour files in the 2023 layout (no fur block per garment) | Whole costume white | Converts them to the current layout, with the game's own disabled fur block |
| Texture whose mip table declares a wrong row pitch | Loading never completes | Rewrites the pitch (the image data is left as is) |
| Mesh whose materials its material file does not define | Loading never completes | Rebuilds the part from the mod's shared `000/` part, else from the original outfit |
| Model folder shared by two outfits of the character (Dhalsim) | Another outfit shown instead of the mod | Maps the mod to the outfit that owns the folder (the lowest costume number) |
| Character played on trial, or DLC unlocked by a script | 100 empty outfits, loading stalls on the first one | Only the slots in use are declared to the game |
| A Street Fighter 6 still closing holds the old pak | Old costumes kept for good | Waits for it, and if it does not let go, changes nothing and retries at the next launch |
| A mod is removed while the game still remembers its outfit (last choice, replay) | Loading stalls on the removed outfit | Every unused slot number shows the character's Outfit 1 instead |
| Slots keep their number after a mod is removed | Gaps in the names (Outfit I, III, IV) | Names follow each other per character (Outfit I, II, III); the numbers behind them stay |

Every check is measured against the game's own files: the mesh and material rule holds for all 362
original costume parts, the texture rule for all 7,084 original costume textures, and the colour-file
reader rewrites all 1,530 original colour files byte for byte.

## Mods tested

Tested on 2026-09-27 with official REFramework 1.5.8 and the game up to date. "Works" means seen in game,
offline (training, character select, replays).

| Character | Mod | Result | Notes |
|---|---|---|---|
| Ryu | [Aloha Ryu bundle](https://www.nexusmods.com/streetfighter6/mods/3214) | Works | 12 variants (outfit 2 and 3, gloves and headband add-ons) |
| Ryu | Vagrant, installed with Fluffy Mod Manager | Works | Patch paks are read like folders |
| Ken | [Alpha Ken](https://www.nexusmods.com/streetfighter6/mods/2822) | Works | Read straight from the `.rar` |
| Ken | [Alpha Ken (Ripped Gi)](https://www.nexusmods.com/streetfighter6/mods/2822) | Works | |
| Ken | [Ken SFV style](https://www.nexusmods.com/streetfighter6/mods/1145) | Works | October 2023: old textures and 2023 colour files, both converted |
| Ken | [Vegeta Majin](https://www.nexusmods.com/streetfighter6/mods/126) | Works offline | June 2023: head part rebuilt, two textures repaired. Online: network error, under investigation |
| Cammy | [DOA4 Christie](https://www.nexusmods.com/streetfighter6/mods/3166) | Works | Colour files upgraded; the costume was white before, all 10 colours now show |
| Lily | [Nico bundle](https://www.nexusmods.com/streetfighter6/mods/78) | Works | Outfits with hair and weapon add-ons, weapons follow the slot |
| Zangief | [Specter Zangief](https://www.nexusmods.com/streetfighter6/mods/3816) | Works | |
| Dhalsim | [Mummy Dhalsim](https://www.nexusmods.com/streetfighter6/mods/3645) | Works | |
| Juri | [Oni Juri](https://www.nexusmods.com/streetfighter6/mods/3900) | Works | |
| Juri | [Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3781) | Works | Together with Oni Juri, in either order |
| A.K.I. | [TFD Racer](https://www.nexusmods.com/streetfighter6/mods/3084) | Works | Same colour-file problem as Christie, fixed the same way |
| Akuma | [Specter Akuma](https://www.nexusmods.com/streetfighter6/mods/3805) | Works | |
| C. Viper | [Coat no mesh, Outfit 1](https://www.nexusmods.com/streetfighter6/mods/2966) | Works | Keeps the coat, removes the mesh fabric |
| C. Viper | An Outfit 3 body mod shared on Discord | Works | |

## Not supported yet

- Mods that combine parts of two outfits (for example a head taken from Drive Tech Wear).
- Hair physics driven by the stage wind, when the mod ships its own wind settings: untested.
- Characters released after the loader's tables were built (they cover the 31 characters up to Yasmine).

## Reporting a mod

Send the mod link and `SF6_CostumeLoader.log` from the game folder. Lines starting with `WARN` or
`ERROR`, and the `check:` lines, tell what the loader found and repaired.
