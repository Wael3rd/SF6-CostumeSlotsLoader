# Changelog

## Unreleased (since 1.5.0)

### Installing mods
- Costume mods are read as downloaded: `.zip`, `.7z` and `.rar` files in
  `reframework\costume_mods\<Character>\`, unpacked once into a hidden cache.
- Bundles give one outfit slot per option; add-ons (no gloves, other hair, a weapon) are combined with
  the outfit they complete, one variant per add-on plus one with all of them.
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

### Names
- Outfit names follow each other per character (Outfit I, II, III) after a mod is removed; each slot
  keeps its number behind the name, so saved choices and replays stay valid.

### Log
- Each slot line shows its outfit name (BrewedVFX).
