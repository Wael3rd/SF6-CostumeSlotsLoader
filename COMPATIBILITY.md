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
| Texture whose small levels were sized with fractional blocks (6144x6144 BC7: the 6x6 level declared 36 bytes, 64 written) | The outfit's own part replaced by the original one (Feixue for Mai lost its body, and with it its face) | Rewrites the whole mip table when the table computed from the format fills the file exactly |
| Mesh whose materials its material file does not define | Loading never completes | Pairs the mesh with another material file of the outfit that defines its materials (a part hidden behind a stripped mesh of another part); else rebuilds the part from the mod's shared `000/` part, else from the original outfit |
| Mod whose own costume scene points a part at another folder (Mummy Dhalsim's head in `0L/`) | The original part shown (Dhalsim's own head) | Takes over the parts the mod's scene moved, when the file exists and the scene matches the game's |
| Physics files next to the parts (`esf032_001_02_chain.chain`), where the game keeps them, rather than in a part folder | The new hair or cloth moves with the original outfit's physics, or not at all | Reads the part from the file name and points the slot's physics settings at the mod's files |
| Model folder shared by two outfits of the character (Dhalsim) | Another outfit shown instead of the mod | Maps the mod to the outfit that owns the folder (the lowest costume number) |
| Character played on trial, or DLC unlocked by a script | 100 empty outfits, loading stalls on the first one | Only the slots in use are declared to the game |
| A Street Fighter 6 still closing holds the old pak | Old costumes kept for good | Waits for it, and if it does not let go, changes nothing and retries at the next launch |
| A mod is removed while the game still remembers its outfit (last choice, replay) | Loading stalls on the removed outfit | Every unused slot number shows the character's Outfit 1 instead |
| Slots keep their number after a mod is removed | Gaps in the names (Outfit I, III, IV) | Names follow each other per character (Outfit I, II, III); the numbers behind them stay |
| Modular mod built for Fluffy (main files, "pick one" groups, optional extras) | Every option combined with every other: over a hundred outfits, the character full | One outfit per shape option (body, shoes, jacket), first texture option (skin), optional parts left out, as Fluffy installs by default |
| Options of one mod replacing the same files | Two options on the same slot | Each option gets its own identity; outfits made of exactly the same files are installed once |
| A character with more than 100 outfits | Outfits past the 100th silently missing | Named in the log |
| Mod that only changes a part of an original outfit (glasses, earrings) | Ignored | The original outfit with the change, in a slot of its own; the original stays as it is |

Every check is measured against the game's own files: the mesh and material rule holds for all 362
original costume parts, the texture rule for all 7,084 original costume textures, and the colour-file
reader rewrites all 1,530 original colour files byte for byte.

## Mods tested

Tested on 2026-09-27 with official REFramework 1.5.8 and the game up to date. "Works" means seen in game
(training, character select, replays); Ryu Outfit I and Vegeta Majin were also played online.

| Character | Mod | Result | Notes |
|---|---|---|---|
| Ryu | [Aloha Ryu bundle](https://www.nexusmods.com/streetfighter6/mods/3214) | Works | 12 variants (outfit 2 and 3, gloves and headband add-ons) |
| Ryu | [Vagrant Ryu](https://www.nexusmods.com/streetfighter6/mods/2663) | Works | Installed with Fluffy Mod Manager (patch pak) |
| Akuma | [Akuma Classic Barechest](https://www.nexusmods.com/streetfighter6/mods/1693) | Works | A ready-made `.pak`, read from its `.rar` too |
| Ken | [Alpha Ken](https://www.nexusmods.com/streetfighter6/mods/2822) | Works | Read straight from the `.rar` |
| Ken | [Alpha Ken (Ripped Gi)](https://www.nexusmods.com/streetfighter6/mods/2822) | Works | |
| Ken | [Ken SFV style](https://www.nexusmods.com/streetfighter6/mods/1145) | Works | October 2023: old textures and 2023 colour files, both converted |
| Ken | [Ken SFV Hair (V2)](https://www.nexusmods.com/streetfighter6/mods/3897) | Works | Hair only. On its own: Outfit 2 with that hair. In one sub-folder with Ken SFV style: Ken SFV with that hair |
| Ken | [Vegeta Majin](https://www.nexusmods.com/streetfighter6/mods/126) | Works | June 2023: head part rebuilt, two textures repaired; online too |
| Cammy | [DOA4 Christie](https://www.nexusmods.com/streetfighter6/mods/3166) | Works | Colour files upgraded; the costume was white before, all 10 colours now show |
| Lily | [Nico bundle](https://www.nexusmods.com/streetfighter6/mods/78) | Works | Outfits with hair and weapon add-ons, weapons follow the slot |
| Zangief | [Specter Zangief](https://www.nexusmods.com/streetfighter6/mods/3816) | Works | |
| Dhalsim | [Mummy Dhalsim](https://www.nexusmods.com/streetfighter6/mods/3645) | Works | Its head lives in its own folder, named by its scene: showed the original head before 1.7 |
| Juri | [Oni Juri](https://www.nexusmods.com/streetfighter6/mods/3900) | Works | |
| Juri | [Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3781) | Works | Together with Oni Juri, in either order |
| A.K.I. | [TFD Racer](https://www.nexusmods.com/streetfighter6/mods/3084) | Works | Same colour-file problem as Christie, fixed the same way |
| Akuma | [Specter Akuma](https://www.nexusmods.com/streetfighter6/mods/3805) | Works | |
| C. Viper | [Coat no mesh, Outfit 1](https://www.nexusmods.com/streetfighter6/mods/2966) | Works | Keeps the coat, removes the mesh fabric. Colours intact with the C3 body mod installed (reported lost with Fluffy alone) |
| C. Viper | An Outfit 3 body mod shared on Discord | Works | |
| C. Viper | [Swimsuit C. Viper](https://www.nexusmods.com/streetfighter6/mods/3909) | Works | Modular: 8 outfits (regular or thicc, barefoot or heels, gloves or not), default skin |
| C. Viper | [C.Viper Lace Lingerie](https://www.nexusmods.com/streetfighter6/mods/3015) | Works | Modular: 8 outfits (regular or thicc, barefoot or heels, jacket or not), default skin |
| C. Viper | [C. Viper - Coatless C1](https://www.nexusmods.com/streetfighter6/mods/2967) | Works |  |
| C. Viper | [C. Viper C2 - Glasses Removed](https://www.nexusmods.com/streetfighter6/mods/2972) | Works | Changes the head only: gives Outfit 2 without glasses in a slot of its own (since 1.6.0) |
| Cammy | [Imperium Cammy](https://www.nexusmods.com/streetfighter6/mods/3046) | Works |  |
| Cammy | [Cammy Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3711) | Works |  |
| Dee Jay | [Specter Deejay](https://www.nexusmods.com/streetfighter6/mods/3810) | Works |  |
| Dhalsim | [Dhalsim The Wrestler](https://www.nexusmods.com/streetfighter6/mods/296) | Works | Reported loading Drive Tech with the old loader; fixed by the Dhalsim folder rule |
| Ingrid | [Ingrid as Athena (KOF97)](https://www.nexusmods.com/streetfighter6/mods/3579) | Works, listed twice | The mod also changes a Drive Tech part, which gives a second entry |
| Ingrid | [Chique Casual Ingrid](https://www.nexusmods.com/streetfighter6/mods/3699) | Works | Hair physics since 1.7 (its physics files sit next to the parts) |
| Ingrid | [AoD replace Ingrid](https://www.nexusmods.com/streetfighter6/mods/3977) | Works | Full replacement: keeps the original colour swatches |
| Mai | [Changli Feixue SuiSui replace Mai](https://www.nexusmods.com/streetfighter6/mods/3921) | Works | Three outfits. Since 1.7: Feixue keeps its body and face (texture tables repaired), Mai's head and hair stay hidden as the mod intends, Changli is no longer left out |
| Mai | [Viper Racer](https://www.nexusmods.com/streetfighter6/mods/3018) | Works | Mai Racer (listed as a C. Viper mod in the community list) |
| Manon | [Manon Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3717) | Works |  |
| Marisa | [Marisa Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3782) | Works |  |
| Alex | [Minotaur Alex](https://www.nexusmods.com/streetfighter6/mods/3760) | Works |  |
| Yasmine | [Yasmine shorts](https://www.nexusmods.com/streetfighter6/mods/3904) | Works | Reported to crash on some AMD GPUs even without the loader |

## Not supported yet

- Mods that combine parts of two outfits (for example a head taken from Drive Tech Wear): they give
  one entry per outfit they touch.
- Colour-only mods, which change the colours of an original outfit without any model
  ([CVS Shin Akuma Inspired Color](https://www.nexusmods.com/streetfighter6/mods/3988)): ignored. How such
  a mod should appear (a slot of its own, or the original outfit changed) is still to be decided.
- Characters released after the loader's tables were built (they cover the 31 characters up to Yasmine).
- An accessory mod is applied to the original outfit, unless it sits in one sub-folder of the character
  with a costume for the same outfit, which it then completes.

## Reporting a mod

Send the mod link and `SF6_CostumeLoader.log` from the game folder. Lines starting with `WARN` or
`ERROR`, and the `check:` lines, tell what the loader found and repaired.
