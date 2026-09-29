# Changelog

## 1.7.0 (2026-09-29)

### Colour swatches (experimental)
- The two squares shown next to each colour in the costume menu are computed for every modded outfit: the
  tints of its two largest colour zones, from the mod's own colour files and colour masks, instead of the
  original outfit's squares. Capcom picks its squares by hand, so the result looks like the outfit without
  matching what Capcom would have chosen. This is a trial and may be removed. Outfits whose materials do
  not use the game's colour system keep the original squares.

### Compatibility
- A mod that ships its own costume scene and points a part at another folder gets that part: Mummy Dhalsim
  showed the original head. Followed only when the file exists and the mod's scene matches the game's
  (same model references, same path length); otherwise the log says it was not followed.
- Textures whose small mip levels were sized with fractional blocks are repaired as a whole (Feixue for
  Mai lost its body and face). The mip table is rewritten only when the one computed from the format
  fills the file exactly.
- A part hidden behind a stripped mesh of another part keeps that mesh, paired with a material file that
  defines its materials, instead of getting the original part back (Mai's hair on Feixue and SuiSui) or
  being left out (Changli).
- Physics files kept next to the parts, where the game keeps them, are followed: the slot's physics
  settings point at the mod's files (Chique Casual Ingrid's hair). Every mod laid out that way now uses its
  own physics.

### Building
- `build.bat` finds Visual Studio or the Build Tools wherever they are installed.
- `loader/third_party/bcdec.h` (MIT or public domain) decodes the colour masks.

## 1.6.1 (2026-09-27)

- `reframework\costume_mods\SF6_CostumeAudit.bat`: lists what is installed (loader, REFramework, script,
  plugin), what the last launch did, the costume mods found with where they come from (Fluffy pak,
  archive, folder) and the outfits each one gave. Read-only; the report is saved next to it.
- The loader writes `reframework\data\SF6_Costumes_Data\outfits.json` at each generation, the list the
  audit reads.
- Notes or other files dropped in `costume_mods` no longer make the next launch rebuild everything; only
  archives, `.pak` files and folders count.
- A mod without a name in its `modinfo.ini` is named after its archive or folder in the log.
- Documented: a costume and an add-on from two archives are combined when put in one sub-folder of the
  character.

## 1.6.0 (2026-09-27)

### Installing mods
- Costume mods are read as downloaded: `.zip`, `.7z` and `.rar` files in
  `reframework\costume_mods\<Character>\`, unpacked once into a hidden cache.
- Bundles give one outfit slot per option; add-ons (no gloves, other hair, a weapon) are combined with
  the outfit they complete: up to three add-ons, one variant each, plus one with all of them when they
  do not replace the same files.
- Identical files shared by several variants are stored once in the generated pak.

### Mods made for older versions of the game
- Colour files whose type signature predates a game update are brought up to date
  (DOA4 Christie, TFD Racer showed up white).
- Colour files in the 2023 layout are converted to the current one (Ken SFV style).
- Textures whose mip table declares a wrong row pitch are repaired (Vegeta Majin).
- Material references are renamed together with the textures they point at (white Specter Zangief).
- A part shipped without a material gets the original material of that part only (see-through Lily).
- Weapons and props listed in `model_parts` follow the slot (Lily's clubs).
- A model folder shared by two outfits is mapped to the right one (Mummy Dhalsim).

### Loading safety
- Every slot is read back the way the game will load it before the pak is written: files present,
  every mesh material defined by its material file, every texture present and consistent. Broken parts
  are rebuilt from the mod's shared part or the original outfit; a slot that still fails is left out
  and named in the log.
- Files only a mod has, stored in another outfit's folder, are kept (Juri stalled with two mods).
- Only the slots in use are declared to the game (a character on trial listed 100 empty outfits).
- The pak is really written or nothing changes: a game still closing no longer leaves an old pak
  marked up to date.
- Removing a mod no longer stalls the character select: every unused slot number shows the
  character's Outfit 1, for a choice saved by the game or a replay that still names it.

### Modular mods and variants
- Modular mods built for Fluffy are installed the way Fluffy does by default: one outfit per shape
  option, first texture option, optional parts left out (C. Viper went from 134 entries to 18).
- Options that replace the same files no longer share a slot; identical outfits are installed once.
- A character that runs out of its 100 slots is named in the log.
- A mod that only changes a part of an original outfit (glasses, earrings) gives that outfit with
  the change, in a slot of its own, instead of being ignored.

### Names
- Outfit names follow each other per character (Outfit I, II, III) after a mod is removed; each slot
  keeps its number behind the name, so saved choices and replays stay valid.

### Log
- Each slot line shows its outfit name (BrewedVFX).
