# NOTE: this script is kept as reference. It expects the layout of a live installation
# (game root / reframework / ...) and picks up the built DLLs and the generated data files
# from there, none of which live in this repository.
"""Assemble le zip Fluffy du mod de base "SF6 Costume Slots Loader".

Contenu (chemins relatifs a la racine du jeu, modinfo.ini a la racine du zip) :
  amd_ags_x64.dll                       proxy AGS + loader (SF6_CostumeLoader/amd_ags_x64.dll)
  amd_ags_x64_real.dll                  la vraie bibliotheque AMD AGS du jeu (MIT), que le proxy relaie
  reframework/autorun/SF6_CostumeSlots.lua
  reframework/plugins/SF6_CostumeSlotsNative.dll   (alias en match sur REFramework officiel)
  reframework/data/SF6_Costumes_Data/loader/vanilla_costume_index.tsv
  reframework/data/SF6_Costumes_Data/loader/static/*      (5 fichiers structurels + static_meta.json)

Usage : python make_fluffy_package.py [--version 1.0] [--out "C:\\Users\\...\\Documents\\SF6_CostumeSlotsLoader_v1.0_FluffyMod.zip"]
La vraie AGS est prise dans l'ordre : <jeu>/amd_ags_x64_real.dll, puis reframework/_backup/amd_ags_x64_real_orig_20260922.dll.
"""
import argparse, os, sys, zipfile, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.dirname(HERE)                      # reframework/
GAME = os.path.dirname(REF)                      # racine du jeu

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""): h.update(c)
    return h.hexdigest()[:16]

def is_real_ags(p):
    # la vraie lib AMD importe VCRUNTIME/USER32 et ne contient pas nos chaines ; le proxy contient "amd_ags_x64_real"
    b = open(p, "rb").read()
    return b"amd_ags_x64_real" not in b and b"hardread_core" not in b

CHARACTERS = [
    "Ryu", "Luke", "Kimberly", "Chun-Li", "Manon", "Zangief", "JP",
    "Dhalsim", "Cammy", "Ken", "Dee Jay", "Lily", "A.K.I", "Rashid",
    "Blanka", "Juri", "Marisa", "Guile", "Ed", "E. Honda", "Jamie",
    "Akuma", "Sagat", "M. Bison", "Terry", "Mai", "Elena", "C. Viper",
    "Alex", "Ingrid", "Yasmine",
]

CRLF = chr(13) + chr(10)

COSTUME_MODS_TXT = CRLF.join([
    "Drop costume mods here, one folder per costume:",
    "  reframework/costume_mods/<Character>/<Costume name>/",
    "",
    "The folder can hold a 'natives' tree or a .pak file, exactly as downloaded from Nexus.",
    "Restart the game: each costume becomes an EXTRA outfit slot (Outfit I, II, ...).",
    "Delete the folder and restart to remove the slot. Fluffy Mod Manager paks work too.",
    "",
])

README_TXT = CRLF.join([
    "SF6 Costume Slots Loader v%s",
    "=============================",
    "",
    "Costume mods become EXTRA outfit slots instead of replacing an existing one, and the",
    "original outfit stays available. Nothing to do at runtime, nothing to convert.",
    "",
    "INSTALL (manual): copy the contents of this archive into the Street Fighter 6 folder,",
    "keeping the directory structure:",
    "  amd_ags_x64.dll             loader (AMD AGS proxy, loaded by the game itself)",
    "  amd_ags_x64_real.dll        the original AMD library, called by the proxy",
    "  reframework/autorun/        Lua script (menus, colours)",
    "  reframework/plugins/        native plugin (online matches)",
    "  reframework/data/           static tables + vanilla costume index",
    "  reframework/costume_mods/   drop your costume mods here, one folder each",
    "",
    "INSTALL (Fluffy Mod Manager): install this zip as a mod, then install costume mods as usual.",
    "",
    "REQUIREMENTS: REFramework in dinput8.dll (official 1.5.8 or newer).",
    "",
    "CONFLICTS: this DLL loads nothing else. If another tool wants the same entry point",
    "(HARD READ, MatchScout, any amd_ags_x64.dll proxy), rename THAT one to amd_ags_x64_chain.dll",
    "and keep the genuine AMD library as amd_ags_x64_real.dll: both are then loaded, in that order.",
    "If the game already has an amd_ags_x64.dll from another tool, chain it instead of",
    "overwriting it: rename that one to amd_ags_x64_real.dll first.",
    "",
    "The loader rebuilds re_chunk_000.pak.patch_004.pak at launch (about 1 s when the mods",
    "changed, 30 ms otherwise) and writes SF6_CostumeLoader.log in the game folder.",
    "",
])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.0")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    out = a.out or os.path.join(os.path.expanduser("~"), "Documents", "SF6_CostumeSlotsLoader_v%s_FluffyMod.zip" % a.version)

    proxy = os.path.join(HERE, "amd_ags_x64.dll")
    real = None
    for cand in (os.path.join(GAME, "amd_ags_x64_real.dll"), os.path.join(REF, "_backup", "amd_ags_x64_real_orig_20260922.dll")):
        if os.path.exists(cand) and is_real_ags(cand): real = cand; break
    if real is None: sys.exit("vraie amd_ags_x64.dll introuvable")
    if is_real_ags(proxy): sys.exit("SF6_CostumeLoader/amd_ags_x64.dll n'est pas le proxy (build.bat ?)")
    lua = os.path.join(REF, "autorun", "SF6_CostumeSlots.lua")
    plugin = os.path.join(REF, "plugins", "native", "costumeslots", "out", "SF6_CostumeSlotsNative.dll")
    loader_dir = os.path.join(REF, "data", "SF6_Costumes_Data", "loader")
    files = [
        (proxy, "amd_ags_x64.dll"),
        (real, "amd_ags_x64_real.dll"),
        (lua, "reframework/autorun/SF6_CostumeSlots.lua"),
        (plugin, "reframework/plugins/SF6_CostumeSlotsNative.dll"),
        (os.path.join(loader_dir, "vanilla_costume_index.tsv"), "reframework/data/SF6_Costumes_Data/loader/vanilla_costume_index.tsv"),
    ]
    for n in sorted(os.listdir(os.path.join(loader_dir, "static"))):
        if n.lower().endswith((".md",)): continue
        files.append((os.path.join(loader_dir, "static", n), "reframework/data/SF6_Costumes_Data/loader/static/" + n))
    for src, _ in files:
        if not os.path.exists(src): sys.exit("manquant : " + src)
    modinfo = (
        "name=SF6 Costume Slots Loader\n"
        "version=%s\n"
        "description=Turns installed costume mods (Fluffy paks) into EXTRA outfit slots at game launch, restoring the original outfit. Install once; then install costume mods as usual and restart the game.\n"
        "author=Wael\n"
        "NameAsBundle=SF6 Costume Slots Loader\n" % a.version)
    readme = README_TXT % a.version
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("modinfo.ini", modinfo)
        z.writestr("README.txt", readme)
        for src, dst in files:
            z.write(src, dst)
            print("  %-70s %10d  %s" % (dst, os.path.getsize(src), sha(src)))
        # structure vide livree telle quelle : un dossier par personnage + son mode d'emploi
        z.writestr("reframework/costume_mods/LISEZMOI.txt", COSTUME_MODS_TXT)
        for name in CHARACTERS:
            zi = zipfile.ZipInfo("reframework/costume_mods/%s/" % name)
            zi.external_attr = (0o40755 << 16) | 0x10   # repertoire
            z.writestr(zi, b"")
        print("  %-70s %10s  %s" % ("reframework/costume_mods/<31 personnages>/", "-", "structure"))
    print("zip :", out, os.path.getsize(out), "octets")

if __name__ == "__main__":
    main()
