#!/usr/bin/env python3
"""
costume_loader.py -- SF6 Costume Slot Loader (prototype)

Transforms installed Fluffy costume mods into additional costume slots,
restoring the vanilla outfits they overwrote.  Produces a single pak file.

Modes:
  Production (no --mod-dir):
    Scans re_chunk_000.pak.patch_NNN.pak files, skips our own (marker) and
    --ignore ones, treats each remaining pak as a mod source.
  Dev (--mod-dir):
    Treats each directory as an extracted Fluffy mod.
  Helper (--make-fluffy-pak):
    Builds a pak from a mod directory (simulates Fluffy install).

Usage:
    python costume_loader.py [--pak-dir <dir>] [--verify]
    python costume_loader.py --mod-dir <path> [--mod-dir <path2>] [--verify]
    python costume_loader.py --make-fluffy-pak <mod_dir> <out.pak>
"""

import os
import sys, os, io, re, json, struct, time, copy, uuid, argparse, hashlib
from collections import defaultdict, OrderedDict
import importlib.util

# ============================================================================
# Configuration
# ============================================================================

REASY = os.environ.get("REASY_PARSER", r"..\REasy-parser")  # point REASY_PARSER at your REasy-parser checkout
GAME_DIR_DEFAULT = r"C:\Program Files (x86)\Steam\steamapps\common\Street Fighter 6"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VANILLA_JSON = os.path.join(SCRIPT_DIR, "vanilla_costume_paths.json")

PAK_MAGIC = 0x414B504B
ENTRY_SIZE = 48
ZSTD_LEVEL = 2

ESF_ROOT_SCENE  = "natives/stm/product/charparam/esf/esf.scn.20"
COSTUME_TABLE   = "natives/stm/product/cfncontents/onlineshop/fightercostumeuserdata.user.2"
COSTUME_MSG     = "natives/stm/product/message/fgm/fighter/fightercostumemessage.msg.21"
COLOR_TABLE     = "natives/stm/product/cfncontents/onlineshop/fightercostumecoloruserdata.user.2"
MARKER_PATH     = "natives/stm/sf6_costume_slots.marker"

MODEL_FOLDER_RE = re.compile(r"natives/stm/product/model/esf/(esf\d{3})/(\d{3})/", re.I)
def _int_to_roman(n):
    """Convert integer 1..3999 to Roman numeral."""
    vals = [(1000,'M'),(900,'CM'),(500,'D'),(400,'CD'),
            (100,'C'),(90,'XC'),(50,'L'),(40,'XL'),
            (10,'X'),(9,'IX'),(5,'V'),(4,'IV'),(1,'I')]
    s = ''
    for v, r in vals:
        while n >= v:
            s += r; n -= v
    return s
ROMAN = [_int_to_roman(i) for i in range(1, 201)]  # I..CC (up to 200)
PATCHABLE_EXTS = {".mdf2.31", ".user.2", ".chain.52", ".scn.20"}
OLD_TEX_SUFFIXES = ["143230113"]  # known old tex version numbers

# ============================================================================
# Section 1 -- Murmur3 hash
# ============================================================================

def _rotl32(x, r):
    return ((x << r) | (x >> (32 - r))) & 0xFFFFFFFF

def _fmix(h):
    h ^= h >> 16; h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13; h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16; return h

def murmur3_hash(data: bytes) -> int:
    c1, c2 = 0xCC9E2D51, 0x1B873593
    h1 = 0xFFFFFFFF; length = 0; i = 0; n = len(data)
    while i < n:
        chunk = data[i:i+4]; i += len(chunk); length += len(chunk); k1 = 0
        if len(chunk) >= 1: k1  = chunk[0]
        if len(chunk) >= 2: k1 |= chunk[1] << 8
        if len(chunk) >= 3: k1 |= chunk[2] << 16
        if len(chunk) == 4:
            k1 |= chunk[3] << 24
            k1 = (k1 * c1) & 0xFFFFFFFF; k1 = _rotl32(k1, 15)
            k1 = (k1 * c2) & 0xFFFFFFFF; h1 ^= k1
            h1 = _rotl32(h1, 13); h1 = (h1 * 5 + 0xE6546B64) & 0xFFFFFFFF
        else:
            k1 = (k1 * c1) & 0xFFFFFFFF; k1 = _rotl32(k1, 15)
            k1 = (k1 * c2) & 0xFFFFFFFF; h1 ^= k1
    h1 ^= length; return _fmix(h1)

def filepath_hash(path: str) -> int:
    p = path.strip().replace("\\", "/")
    while "//" in p: p = p.replace("//", "/")
    lo = murmur3_hash(p.lower().encode("utf-16-le")) & 0xFFFFFFFF
    hi = murmur3_hash(p.upper().encode("utf-16-le")) & 0xFFFFFFFF
    return ((hi << 32) | lo) & 0xFFFFFFFFFFFFFFFF

def compute_hashes(path: str):
    p = path.strip().replace("\\", "/")
    while "//" in p: p = p.replace("//", "/")
    lo = murmur3_hash(p.lower().encode("utf-16-le")) & 0xFFFFFFFF
    hi = murmur3_hash(p.upper().encode("utf-16-le")) & 0xFFFFFFFF
    return lo, hi

# ============================================================================
# Section 2 -- Pak I/O
# ============================================================================

try:
    import zstandard as zstd
except ImportError:
    sys.exit("ERROR: 'zstandard' package required.  pip install zstandard")


class PakIndex:
    """Read-only index over one or more RE Engine v4 pak files."""

    def __init__(self):
        self.entries = {}
        self._handles = {}

    def add_pak(self, path):
        if not os.path.exists(path):
            print(f"  [skip] pak not found: {path}")
            return 0
        f = open(path, "rb")
        hdr = f.read(16)
        magic, maj, _, _, count, _ = struct.unpack("<IBBhII", hdr)
        assert magic == PAK_MAGIC and maj == 4, f"bad pak header in {path}"
        self._handles[path] = f
        for _ in range(count):
            lo, hi, off, cs, ds, att, _ = struct.unpack("<IIqqqqq", f.read(ENTRY_SIZE))
            combined = (hi << 32) | lo
            self.entries[combined] = (path, off, cs, ds, att)
        size_mb = os.path.getsize(path) / 1e6
        print(f"  pak {os.path.basename(path)}: {count} entries ({size_mb:.0f} MB)")
        return count

    def has(self, combined_hash):
        return combined_hash in self.entries

    def read_raw(self, combined_hash):
        path, off, cs, ds, att = self.entries[combined_hash]
        fh = self._handles[path]
        fh.seek(off)
        return fh.read(cs), ds, att

    def read_data(self, combined_hash):
        raw, ds, att = self.read_raw(combined_hash)
        compression = att & 0xFF
        if compression == 0 or len(raw) == ds:
            return raw
        if compression == 2:
            return zstd.ZstdDecompressor().decompress(raw, max_output_size=ds)
        if compression == 1:
            import zlib
            return zlib.decompress(raw, -15, ds)
        try:
            return zstd.ZstdDecompressor().decompress(raw, max_output_size=ds)
        except Exception:
            return raw

    def close(self):
        for fh in self._handles.values():
            fh.close()
        self._handles.clear()


def _compress(data):
    return zstd.ZstdCompressor(level=ZSTD_LEVEL).compress(data)

def _attrib_for(path):
    return 0x1002 if ".msg." in path.rsplit("/", 1)[-1].lower() else 0x2


class PakWriter:
    def __init__(self):
        self._entries = []
        self.expected_paths = set()

    def add_compressed(self, path, data, attrib=None):
        if attrib is None: attrib = _attrib_for(path)
        comp = _compress(data)
        self._entries.append((path, comp, len(comp), len(data), attrib))
        self.expected_paths.add(path)

    def add_uncompressed(self, path, data):
        self._entries.append((path, data, len(data), len(data), 0))
        self.expected_paths.add(path)

    def add_passthrough(self, path, raw_blob, dsize, attrib):
        self._entries.append((path, raw_blob, len(raw_blob), dsize, attrib))
        self.expected_paths.add(path)

    def has_path(self, path):
        return path in self.expected_paths

    def write(self, out_path):
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        n = len(self._entries)
        hdr_size = 16 + ENTRY_SIZE * n
        offset = hdr_size
        table = b""; blob = b""
        for path, data, cs, ds, att in self._entries:
            lo, hi = compute_hashes(path)
            table += struct.pack("<IIqqqqq", lo, hi, offset, cs, ds, att, 0)
            blob += data; offset += cs
        with open(out_path, "wb") as f:
            f.write(struct.pack("<IBBhII", PAK_MAGIC, 4, 0, 0, n, 0))
            f.write(table); f.write(blob)
        sz = os.path.getsize(out_path)
        print(f"  wrote {out_path} ({sz/1e6:.1f} MB, {n} entries)")
        return out_path


# ============================================================================
# Section 2b -- ModFile abstraction
# ============================================================================

class ModFile:
    """References a mod file in either a directory or a pak."""
    __slots__ = ("_disk_path", "_pak_source")

    def __init__(self, disk_path=None, pak_source=None):
        self._disk_path = disk_path          # str
        self._pak_source = pak_source         # (PakIndex, combined_hash)

    def read(self):
        if self._disk_path:
            p = self._disk_path
            if len(p) > 240 and not p.startswith("\\\\?\\"):
                p = "\\\\?\\" + os.path.abspath(p)
            with open(p, "rb") as f:
                return f.read()
        if self._pak_source:
            return self._pak_source[0].read_data(self._pak_source[1])
        raise ValueError("no source")

    def read_raw_if_pak(self):
        """Return (raw, dsize, attrib) if source is a pak, else None."""
        if self._pak_source:
            return self._pak_source[0].read_raw(self._pak_source[1])
        return None


# ============================================================================
# Section 3 -- Vanilla Inventory
# ============================================================================

class VanillaInventory:
    def __init__(self, json_path):
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.suffixes = data.get("version_suffixes", {})
        self.costumes = data["costumes"]
        self.by_key = {}
        self.by_fighter = defaultdict(list)
        self.folder_to_costume = {}
        self.all_hashes = set()
        self.hash_to_paths = defaultdict(list)   # hash -> [pak_path, ...]
        self.path_to_costumes = defaultdict(list)

        for c in self.costumes:
            fid, cno = c["fighter"], c["costume_no"]
            self.by_key[(fid, cno)] = c
            self.by_fighter[fid].append(c)
            md = c.get("model_dir", "")
            if md:
                folder = md.rsplit("/", 1)[-1]
                self.folder_to_costume[(c["fighter_dir"], folder)] = (fid, cno)
            for fi in c.get("files", []):
                h = fi.get("hash", 0)
                if h:
                    self.all_hashes.add(h)
                    self.hash_to_paths[h].append(fi["path"])
                self.path_to_costumes[fi["path"]].append((fid, cno))

    def max_costume_no(self, fighter):
        return max((c["costume_no"] for c in self.by_fighter[fighter]
                     if c["costume_no"] < 100), default=-1)

    def max_folder(self, fighter):
        mx = 0
        for c in self.by_fighter[fighter]:
            md = c.get("model_dir", "")
            if md:
                try: mx = max(mx, int(md.rsplit("/", 1)[-1]))
                except ValueError: pass
        return mx

    def model_prefix(self, fighter_dir, folder):
        return f"natives/stm/product/model/esf/{fighter_dir}/{folder}/"

    def get_costume(self, fighter, costume_no):
        return self.by_key.get((fighter, costume_no))

    def costume_model_files(self, fighter, costume_no):
        c = self.get_costume(fighter, costume_no)
        if not c or not c.get("model_dir"): return []
        prefix = c["model_dir"] + "/"
        return [fi for fi in c["files"] if fi["path"].startswith(prefix)]


# ============================================================================
# Section 4 -- Mod scanning and detection
# ============================================================================

def scan_mod_dir(mod_dir):
    """Walk a Fluffy mod directory.  Returns {pak_path: ModFile}."""
    files = {}
    for root, _dirs, names in os.walk(mod_dir):
        for name in names:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, mod_dir).replace("\\", "/")
            idx = rel.lower().find("natives/stm/")
            if idx < 0: continue
            pak_path = rel[idx:].lower()
            files[pak_path] = ModFile(disk_path=full)
    return files


def _find_natives_dir(folder, max_depth=3):
    """Find first 'natives' directory within max_depth levels of folder."""
    try:
        for entry in os.scandir(folder):
            if entry.is_dir(follow_symlinks=False) and entry.name.lower() == "natives":
                return entry.path
        if max_depth > 1:
            for entry in os.scandir(folder):
                if entry.is_dir(follow_symlinks=False) and entry.name.lower() != "natives":
                    result = _find_natives_dir(entry.path, max_depth - 1)
                    if result:
                        return result
    except OSError:
        pass
    return None


def scan_folder_mod(mod_dir, inv):
    """Walk a folder mod directory (natives/stm tree on disk).
    Returns ({pak_path: ModFile}, ignored_count).
    Old tex suffixes are auto-upgraded. Non-costume files outside model/esf
    are kept only if their filename matches a costume folder prefix
    (e.g. weather chains with esfXXX_YYY_ in the name)."""
    tex_version = inv.suffixes.get("tex", ".tex.241101895").rsplit(".", 1)[-1]
    walk_dir = mod_dir
    if len(mod_dir) > 240 and not mod_dir.startswith("\\\\?\\"):
        walk_dir = "\\\\?\\" + os.path.abspath(mod_dir)
    # First pass: collect all natives/stm/ files
    all_items = []  # (pak_path, disk_path)
    upgraded = 0
    for root, _dirs, names in os.walk(walk_dir):
        for name in names:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, walk_dir).replace("\\", "/")
            idx = rel.lower().find("natives/stm/")
            if idx < 0:
                continue
            pak_path = rel[idx:].lower()
            for old_suf in OLD_TEX_SUFFIXES:
                if pak_path.endswith(f".tex.{old_suf}"):
                    pak_path = pak_path[:-(len(old_suf))] + tex_version
                    upgraded += 1
                    break
            all_items.append((pak_path, full))
    if upgraded:
        print(f"    {upgraded} old-suffix textures upgraded")
    # Find costume folder prefixes from model/esf files (e.g. "esf010_002_")
    costume_prefixes = set()
    for pak_path, _ in all_items:
        m = MODEL_FOLDER_RE.search(pak_path)
        if m and m.group(2) != "000":
            costume_prefixes.add(f"{m.group(1).lower()}_{m.group(2)}_")
    # Second pass: classify
    files = {}
    ignored = 0
    extra = 0
    for pak_path, full in all_items:
        if "/product/model/esf/" in pak_path:
            files[pak_path] = ModFile(disk_path=full)
        elif "/streaming/product/model/esf/" in pak_path:
            files[pak_path] = ModFile(disk_path=full)
        else:
            fn = pak_path.rsplit("/", 1)[-1]
            if any(prefix in fn for prefix in costume_prefixes):
                files[pak_path] = ModFile(disk_path=full)
                extra += 1
            else:
                ignored += 1
    if extra:
        print(f"    {extra} extra-model files attached by name")
    return files, ignored


def identify_folder_mod(mod_files):
    """Content-based identity for a folder mod ({pak_path: ModFile}).
    Uses sorted (filepath_hash, file_size) pairs like identify_mod_pak."""
    entries = []
    for pak_path, mf in mod_files.items():
        h = filepath_hash(pak_path)
        try:
            p = mf._disk_path
            if p and len(p) > 240 and not p.startswith("\\\\?\\"):
                p = "\\\\?\\" + os.path.abspath(p)
            sz = os.path.getsize(p) if p else 0
        except OSError:
            sz = 0
        entries.append((h, sz))
    entries.sort()
    h = hashlib.md5()
    for combined, sz in entries:
        h.update(struct.pack("<QQ", combined, sz))
    return h.hexdigest()[:16]


CHARACTER_NAMES = [
    "Ryu", "Luke", "Kimberly", "Chun-Li", "Manon", "Zangief", "JP",
    "Dhalsim", "Cammy", "Ken", "Dee Jay", "Lily", "A.K.I.", "Rashid",
    "Blanka", "Juri", "Marisa", "Guile", "Ed", "E. Honda", "Jamie",
    "Akuma", "Sagat", "M. Bison", "Terry", "Mai", "Elena", "C. Viper",
    "Alex", "Ingrid", "Yasmine",
]


def _ensure_costume_mods_dirs(game_dir):
    """Create costume_mods/ and per-character subdirectories if missing."""
    base_dir = os.path.join(game_dir, "reframework", "costume_mods")
    try:
        os.makedirs(base_dir, exist_ok=True)
        for name in CHARACTER_NAMES:
            char_dir = os.path.join(base_dir, name)
            os.makedirs(char_dir, exist_ok=True)
    except OSError:
        pass
    return base_dir


def _scan_one_mod_folder(mod_dir, rel_path, inv, mods, all_files, pak_indexes, infos):
    """Scan a single mod folder (leaf or char-level fallback).
    Determines if it contains paks or a natives tree and processes accordingly."""
    try:
        dir_contents = os.listdir(mod_dir)
    except OSError:
        return

    pak_files = sorted(f for f in dir_contents
                       if f.lower().endswith(".pak")
                       and os.path.isfile(os.path.join(mod_dir, f)))

    if pak_files:
        # --- Pak-based folder mod ---
        for pf in pak_files:
            pak_path = os.path.join(mod_dir, pf)
            print(f"    pak: {pf}")
            mod_pak = PakIndex()
            mod_pak.add_pak(pak_path)
            pak_indexes.append(mod_pak)

            mod_id = identify_mod_pak(pak_path)
            mod_files, unattributed = scan_mod_pak(mod_pak, inv)
            print(f"    mod_id: {mod_id}")
            print(f"    resolved: {len(mod_files)} paths, "
                  f"unattributed: {len(unattributed)}")

            costumes = detect_mod_costumes(mod_files, inv)
            for mc in costumes:
                print(f"    -> {mc.fighter_dir} costume "
                      f"{mc.original_costume_no} "
                      f"(folder {mc.original_folder}): "
                      f"{len(mc.mod_files_in_folder)} files")
            mods.append((mod_id, costumes))
            all_files.update(mod_files)
            infos.append((rel_path + "/" + pf, pak_path))
    else:
        # --- Natives-based folder mod ---
        natives = _find_natives_dir(mod_dir)
        if not natives:
            return  # no natives dir = not a mod folder (may be empty char dir)

        mod_files, ignored_count = scan_folder_mod(mod_dir, inv)
        if ignored_count:
            print(f"    {ignored_count} non-costume files ignored "
                  f"(folder mods cannot provide global files)")
        print(f"    {len(mod_files)} costume files")

        if not mod_files:
            print(f"    WARN: no costume files found, skipping")
            return

        mod_id = identify_folder_mod(mod_files)
        print(f"    mod_id: {mod_id}")

        costumes = detect_mod_costumes(mod_files, inv)
        for mc in costumes:
            print(f"    -> {mc.fighter_dir} costume "
                  f"{mc.original_costume_no} "
                  f"(folder {mc.original_folder}): "
                  f"{len(mc.mod_files_in_folder)} files")
        mods.append((mod_id, costumes))
        all_files.update(mod_files)
        infos.append((rel_path, mod_dir))


def scan_costume_mods(game_dir, inv):
    """Scan <game_dir>/reframework/costume_mods/<Char>/<Name>/ for mods.
    Also accepts mods placed directly in <Char>/ (no sub-folder).
    Creates the directory structure if missing.
    Returns (mods_with_costumes, all_mod_files, mod_pak_indexes, folder_mod_infos)."""
    base_dir = _ensure_costume_mods_dirs(game_dir)

    print(f"\nScanning costume_mods in {base_dir}...")

    mods = []
    all_files = {}
    pak_indexes = []
    infos = []

    try:
        char_entries = sorted(os.listdir(base_dir))
    except OSError:
        return [], {}, [], []

    for char_name in char_entries:
        char_dir = os.path.join(base_dir, char_name)
        if not os.path.isdir(char_dir):
            continue

        # Check if char_dir itself is a mod (natives/ or .pak directly in it)
        has_sub_mods = False
        try:
            char_contents = sorted(os.listdir(char_dir))
        except OSError:
            continue

        # First pass: check for costume sub-folders
        for item in char_contents:
            item_path = os.path.join(char_dir, item)
            if os.path.isdir(item_path):
                # Skip if it looks like a natives/ dir (mod directly in char_dir)
                if item.lower() == "natives":
                    continue
                # This is a costume sub-folder
                rel_path = f"{char_name}/{item}"
                print(f"\n  folder: {rel_path}")
                _scan_one_mod_folder(item_path, rel_path, inv,
                                     mods, all_files, pak_indexes, infos)
                has_sub_mods = True

        # Check if mod placed directly in char_dir (no sub-folder)
        # Detect: natives/ dir or .pak files directly in char_dir
        has_direct_natives = _find_natives_dir(char_dir, max_depth=1)
        has_direct_paks = any(f.lower().endswith(".pak")
                              and os.path.isfile(os.path.join(char_dir, f))
                              for f in char_contents)
        if has_direct_natives or has_direct_paks:
            rel_path = char_name
            print(f"\n  folder: {rel_path} (mod directly in character dir)")
            _scan_one_mod_folder(char_dir, rel_path, inv,
                                 mods, all_files, pak_indexes, infos)

    return mods, all_files, pak_indexes, infos


def identify_mod_dir(mod_dir):
    """Stable identity for a mod directory."""
    h = hashlib.md5()
    h.update(os.path.basename(mod_dir).encode("utf-8"))
    for root, _d, names in os.walk(mod_dir):
        for n in sorted(names):
            fp = os.path.join(root, n)
            try: h.update(f"{n}:{os.path.getsize(fp)}".encode())
            except OSError: pass
    return h.hexdigest()[:16]


def identify_mod_pak(pak_path):
    """Stable content-based identity for a mod pak (sorted hashes+sizes)."""
    entries = []
    with open(pak_path, "rb") as f:
        hdr = f.read(16)
        _, _, _, _, count, _ = struct.unpack("<IBBhII", hdr)
        for _ in range(count):
            lo, hi, _, cs, _, _, _ = struct.unpack("<IIqqqqq", f.read(ENTRY_SIZE))
            entries.append(((hi << 32) | lo, cs))
    entries.sort()
    h = hashlib.md5()
    for combined, cs in entries:
        h.update(struct.pack("<QQ", combined, cs))
    return h.hexdigest()[:16]


def scan_mod_pak(mod_pak, inv):
    """Classify a mod pak's entries by hash-matching the vanilla inventory.
    Unknown entries are discovered via mdf2 texture reference parsing.
    Old tex suffixes are auto-upgraded to the current version.
    Returns ({pak_path: ModFile}, [unattributed_hashes])."""
    known = {}    # pak_path -> ModFile
    unknown = {}  # combined_hash -> True

    for combined_hash in mod_pak.entries:
        if combined_hash in inv.hash_to_paths:
            for path in inv.hash_to_paths[combined_hash]:
                known[path] = ModFile(pak_source=(mod_pak, combined_hash))
        else:
            unknown[combined_hash] = True

    tex_version = inv.suffixes.get("tex", ".tex.241101895").rsplit(".", 1)[-1]
    upgraded = 0

    # Try old tex suffixes for remaining unknowns against vanilla inventory
    if unknown:
        for vh, vpaths in inv.hash_to_paths.items():
            for vp in vpaths:
                if not vp.lower().endswith(f".tex.{tex_version}"):
                    continue
                stem = vp[:-(len(tex_version) + 1)]  # "...foo.tex"
                for old_suf in OLD_TEX_SUFFIXES:
                    old_path = f"{stem}.{old_suf}"
                    oh = filepath_hash(old_path)
                    if oh in unknown:
                        known[vp] = ModFile(pak_source=(mod_pak, oh))
                        del unknown[oh]
                        upgraded += 1

    # Parse known mdf2 entries to discover custom texture paths
    mdf2_paths = [p for p in list(known) if p.lower().endswith(".mdf2.31")]
    discovered = 0
    for mdf_path in mdf2_paths:
        try:
            _init_reasy()
            from file_handlers.mdf.mdf_file import MdfFile
            mdf_data = known[mdf_path].read()
            mdf = MdfFile()
            mdf.read(mdf_data, file_path=os.path.basename(mdf_path))
            for mat in mdf.materials:
                for tex in mat.textures:
                    tex_rel = tex.tex_path.strip().rstrip("\x00")
                    if not tex_rel: continue
                    tex_pak = f"natives/stm/{tex_rel.replace(chr(92), '/').lower()}.{tex_version}"
                    tex_hash = filepath_hash(tex_pak)
                    if tex_hash in unknown:
                        known[tex_pak] = ModFile(pak_source=(mod_pak, tex_hash))
                        del unknown[tex_hash]
                        discovered += 1
                    # Also try old suffixes
                    for old_suf in OLD_TEX_SUFFIXES:
                        old_pak = f"natives/stm/{tex_rel.replace(chr(92), '/').lower()}.{old_suf}"
                        oh = filepath_hash(old_pak)
                        if oh in unknown:
                            known[tex_pak] = ModFile(pak_source=(mod_pak, oh))
                            del unknown[oh]
                            discovered += 1
                            upgraded += 1
        except Exception:
            pass

    if discovered:
        print(f"    discovered {discovered} custom textures via mdf2 parsing")
    if upgraded:
        print(f"    {upgraded} old-suffix textures upgraded")

    # Resolve streaming twins: for each known texture, check if pak has
    # its streaming/ counterpart among the unknowns
    streaming_resolved = 0
    base_pfx = "natives/stm/product/"
    stream_pfx = "natives/stm/streaming/product/"
    for path in list(known.keys()):
        if ".tex." not in path or not path.startswith(base_pfx):
            continue
        sp = stream_pfx + path[len(base_pfx):]
        sh = filepath_hash(sp)
        if sh in unknown:
            known[sp] = ModFile(pak_source=(mod_pak, sh))
            del unknown[sh]
            streaming_resolved += 1
    if streaming_resolved:
        print(f"    {streaming_resolved} streaming textures resolved")

    return known, list(unknown.keys())


def classify_patch_paks(pak_dir, ignore_list=None):
    """Return (our_paks, mod_paks) as lists of (number, path).
    our_paks = those containing our marker.
    mod_paks = the rest (excluding --ignore)."""
    our, mod = [], []
    ignored = set(ignore_list or [])
    marker_h = filepath_hash(MARKER_PATH)

    if not os.path.isdir(pak_dir):
        return our, mod

    for fn in sorted(os.listdir(pak_dir)):
        m_fn = re.match(r"re_chunk_000\.pak\.patch_(\d+)\.pak$", fn)
        if not m_fn: continue
        num = int(m_fn.group(1))
        if fn in ignored or f"patch_{m_fn.group(1)}" in ignored:
            continue
        path = os.path.join(pak_dir, fn)
        try:
            has_marker = False
            with open(path, "rb") as f:
                hdr = f.read(16)
                _, _, _, _, count, _ = struct.unpack("<IBBhII", hdr)
                for _ in range(count):
                    lo, hi, _, _, _, _, _ = struct.unpack("<IIqqqqq", f.read(ENTRY_SIZE))
                    if ((hi << 32) | lo) == marker_h:
                        has_marker = True; break
            (our if has_marker else mod).append((num, path))
        except Exception:
            mod.append((num, path))
    return our, mod


class ModCostume:
    __slots__ = ("fighter", "fighter_dir", "original_costume_no", "original_folder",
                 "mod_files_in_folder", "mod_files_shared", "mod_files_other")

    def __init__(self, fighter, fighter_dir, orig_cno, orig_folder):
        self.fighter = fighter
        self.fighter_dir = fighter_dir
        self.original_costume_no = orig_cno
        self.original_folder = orig_folder
        self.mod_files_in_folder = {}
        self.mod_files_shared = {}
        self.mod_files_other = {}


def detect_mod_costumes(mod_files, inv):
    """Classify mod files by costume.  Returns list[ModCostume].
    mod_files: {pak_path: ModFile}.
    Rejects partial mods (no body mesh in part 01 of costume folder)."""
    groups = {}
    shared_files = {}  # 000/ files, keyed by fighter_dir
    other_files = {}

    for pak_path, mod_file in mod_files.items():
        m = MODEL_FOLDER_RE.search(pak_path)
        if not m:
            other_files[pak_path] = mod_file; continue
        fighter_dir = m.group(1).lower()
        folder = m.group(2)

        if folder == "000":
            shared_files.setdefault(fighter_dir, {})[pak_path] = mod_file
            continue

        key = (fighter_dir, folder)
        if key not in groups:
            lookup = inv.folder_to_costume.get(key)
            if lookup is None:
                print(f"  WARN: mod touches unknown folder {fighter_dir}/{folder}, skipping")
                continue
            fid, cno = lookup
            groups[key] = ModCostume(fid, fighter_dir, cno, folder)
        groups[key].mod_files_in_folder[pak_path] = mod_file

    # Attach 000/ shared files and other files to each group
    for mc in groups.values():
        # 000/ files for this fighter
        for pak_path, mod_file in shared_files.get(mc.fighter_dir, {}).items():
            mc.mod_files_shared[pak_path] = mod_file
        # Extra-model files: attach specifically by fighter+folder prefix
        prefix = f"{mc.fighter_dir}_{mc.original_folder}_"
        for pak_path, mod_file in other_files.items():
            fn = pak_path.rsplit("/", 1)[-1]
            if prefix in fn:
                mc.mod_files_other[pak_path] = mod_file
            elif not MODEL_FOLDER_RE.search(pak_path):
                # Non-model file without a specific prefix -> attach to all (legacy)
                if not any(f"esf" in fn and "_" in fn for _ in [1]):
                    mc.mod_files_other[pak_path] = mod_file

    # Reject partial mods: require at least one body mesh (part 01)
    result = []
    for mc in groups.values():
        has_body = any(f"/{mc.original_folder}/01/" in p and ".mesh." in p
                       for p in mc.mod_files_in_folder)
        if has_body:
            result.append(mc)
        else:
            print(f"  partial mod ignored: {mc.fighter_dir}/{mc.original_folder} "
                  f"(no body mesh in part 01, {len(mc.mod_files_in_folder)} files)")

    return result


# ============================================================================
# Section 5 -- Path manipulation and binary patching
# ============================================================================

def relocate_path(pak_path, fighter_dir, old_folder, new_folder):
    old_dir = f"{fighter_dir}/{old_folder}/"
    new_dir = f"{fighter_dir}/{new_folder}/"
    path = pak_path.replace(old_dir, new_dir, 1)
    old_pfx = f"{fighter_dir}_{old_folder}_"
    new_pfx = f"{fighter_dir}_{new_folder}_"
    if "/" in path:
        d, fn = path.rsplit("/", 1)
        fn = fn.replace(old_pfx, new_pfx, 1)
        return d + "/" + fn
    return path.replace(old_pfx, new_pfx, 1)

def relocate_path_dir_only(pak_path, fighter_dir, old_folder, new_folder):
    """Relocate: change ONLY the directory segment, keep filename as-is.
    Used for CCVD and CMD files whose _NNN_ is semantic, not a path component."""
    return pak_path.replace(f"{fighter_dir}/{old_folder}/", f"{fighter_dir}/{new_folder}/", 1)

def needs_content_patch(pak_path):
    lower = pak_path.lower()
    return any(lower.endswith(ext) for ext in PATCHABLE_EXTS)

_BASE_PFX = "natives/stm/product/"
_STREAM_PFX = "natives/stm/streaming/product/"

def _write_streaming_twin(writer, orig_path, new_path, all_mod_files, paks):
    """If a streaming twin of the texture at orig_path exists (in mod files
    or base pak), write it under the relocated streaming path.  Returns 1 if
    written, 0 otherwise."""
    if ".tex." not in orig_path.lower():
        return 0
    if not orig_path.startswith(_BASE_PFX):
        return 0
    suffix = orig_path[len(_BASE_PFX):]
    s_orig = _STREAM_PFX + suffix
    s_new  = _STREAM_PFX + new_path[len(_BASE_PFX):]
    if writer.has_path(s_new):
        return 0
    amf = all_mod_files or {}
    if s_orig in amf:
        mf = amf[s_orig]
        raw = mf.read_raw_if_pak()
        if raw:
            writer.add_passthrough(s_new, *raw)
        else:
            writer.add_compressed(s_new, mf.read())
        return 1
    sh = filepath_hash(s_orig)
    if paks.has(sh):
        raw, ds, att = paks.read_raw(sh)
        writer.add_passthrough(s_new, raw, ds, att)
        return 1
    return 0


def patch_paths(data, fighter_dir, old_folder, new_folder):
    """FULL mode: global replace of both dir segment and filename prefix."""
    replacements = [
        (f"{fighter_dir}/{old_folder}/", f"{fighter_dir}/{new_folder}/"),
        (f"{fighter_dir}_{old_folder}_", f"{fighter_dir}_{new_folder}_"),
    ]
    buf = bytearray(data); total = 0
    for old_str, new_str in replacements:
        assert len(old_str) == len(new_str)
        for enc in ("utf-16-le", "utf-8"):
            for case_fn in (str.lower, str.upper):
                old_b = case_fn(old_str).encode(enc)
                new_b = case_fn(new_str).encode(enc)
                count = buf.count(old_b)
                if count:
                    buf = bytearray(bytes(buf).replace(old_b, new_b))
                    total += count
    return bytes(buf), total


def patch_scene_minimal(data, fighter_dir, old_folder, new_folder,
                        relocated_pak_paths=None, suffixes=None):
    """MINIMAL mode scene patching via byte scan with type-aware occurrence
    tracking.  Resource paths (mesh/mdf2) are patched in ALL their binary
    locations (resource_infos string pool + instance data).  UserData paths
    (CCVD) are patched only in the SECOND occurrence (instance data section),
    matching the proven manual pak behavior.
    C++ port: same byte scan with occurrence counter per unique string."""
    old_dir_b = f"{fighter_dir}/{old_folder}/".encode("utf-16-le")
    new_dir_b = f"{fighter_dir}/{new_folder}/".encode("utf-16-le")
    old_pfx_b = f"{fighter_dir}_{old_folder}_".encode("utf-16-le")
    new_pfx_b = f"{fighter_dir}_{new_folder}_".encode("utf-16-le")
    buf = bytearray(data); total = 0; i = 0
    # Track per-string occurrence count to skip first CCVD occurrence
    seen_count = {}  # normalized_string -> count

    while True:
        i = buf.find(old_dir_b, i)
        if i < 0: break
        # Read full enclosing null-terminated UTF-16LE string
        start = i
        while start >= 2:
            if buf[start - 2] == 0 and buf[start - 1] == 0: break
            start -= 2
        end = i
        while end < len(buf) - 1:
            if buf[end] == 0 and buf[end + 1] == 0: break
            end += 2
        try:
            full_str = bytes(buf[start:end]).decode("utf-16-le", errors="replace")
            low = full_str.lower()
        except:
            i += len(old_dir_b); continue

        is_mesh_mdf2 = low.endswith(".mesh") or low.endswith(".mdf2")
        is_ccvd = "ccvd.user" in low
        is_chain_user = "chain.user" in low or "havok.user" in low
        is_userdata = is_ccvd or is_chain_user
        is_visual = is_mesh_mdf2 or is_userdata

        if not is_visual:
            i += len(old_dir_b); continue

        # Existence check against relocated set
        if relocated_pak_paths is not None and suffixes:
            old_dir_s = f"{fighter_dir}/{old_folder}/"
            new_dir_s = f"{fighter_dir}/{new_folder}/"
            old_pfx_s = f"{fighter_dir}_{old_folder}_"
            new_pfx_s = f"{fighter_dir}_{new_folder}_"
            wb = low.replace(old_dir_s, new_dir_s, 1)
            if not is_userdata:
                wb = wb.replace(old_pfx_s, new_pfx_s, 1)
            wb = wb.lstrip("@").strip()
            bn = wb.rsplit("/", 1)[-1] if "/" in wb else wb
            ext = bn.rsplit(".", 1)[-1] if "." in bn else ""
            suf = suffixes.get(ext, "")
            if suf:
                ver = suf.rsplit(".", 1)[-1]
                cand = f"natives/stm/{wb}.{ver}"
            else:
                cand = ""
            if cand and cand not in relocated_pak_paths:
                i += len(old_dir_b); continue

        # Track occurrences: CCVD in userdata_infos table (first occurrence)
        # should NOT be patched; only the instance data copy (second) is.
        # Resource strings (mesh/mdf2) are patched in both locations.
        norm = low.rstrip("\x00")
        seen_count[norm] = seen_count.get(norm, 0) + 1
        if is_userdata and seen_count[norm] == 1:
            # First occurrence (userdata_infos string pool): skip
            i += len(old_dir_b); continue

        # Patch: replace dir segment
        buf[i:i + len(old_dir_b)] = new_dir_b
        total += 1
        # For mesh/mdf2: also replace filename prefix.
        # For CCVD/chain.user: dir-only (the filename prefix is semantic).
        if not is_userdata:
            j = buf.find(old_pfx_b, i, end + 40)
            if j >= 0:
                buf[j:j + len(old_pfx_b)] = new_pfx_b
                total += 1
        i += len(old_dir_b)

    return bytes(buf), total


def patch_mdf2_minimal(data, fighter_dir, old_folder, new_folder, mod_tex_stems):
    """MINIMAL mode mdf2 patching.  Rewrites the directory segment ONLY for
    texture references whose filename the mod provides.  Vanilla texture
    references stay pointed at the vanilla folder.
    mod_tex_stems: set of lowercase texture stems e.g. {'rope_albd.tex'}.
    C++ port: same byte scan with filename check."""
    old_dir = f"{fighter_dir}/{old_folder}/".encode("utf-16-le")
    new_dir = f"{fighter_dir}/{new_folder}/".encode("utf-16-le")
    buf = bytearray(data); total = 0; i = 0
    while True:
        i = buf.find(old_dir, i)
        if i < 0: break
        # Read forward to find the full texture path after the dir segment
        end = i + len(old_dir)
        while end < len(buf) - 1:
            if buf[end] == 0 and buf[end + 1] == 0: break
            end += 2
        try:
            rest = bytes(buf[i + len(old_dir):end]).decode("utf-16-le", errors="replace")
            # Extract the filename (after last /)
            fn = rest.rsplit("/", 1)[-1].rstrip("\x00").lower()
            if fn in mod_tex_stems:
                buf[i:i + len(old_dir)] = new_dir
                total += 1
        except: pass
        i += len(old_dir)
    return bytes(buf), total


# ============================================================================
# Section 6 -- Registry
# ============================================================================

class SlotInfo:
    __slots__ = ("mod_id", "fighter", "fighter_dir", "original_costume_no",
                 "original_folder", "new_costume_no", "new_folder", "scene_name",
                 "record_id", "manage_id", "sort_no", "visual_no",
                 "message_name", "message_guid", "message_text",
                 "color_base_id", "color_base_manage_id", "mod_costume",
                 "_weather_chain")
    def __init__(self):
        for s in self.__slots__: setattr(self, s, None)

def load_registry(path):
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f: return json.load(f)
    return {"version": 1, "slots": [], "next_ids": {}}

def save_registry(path, reg):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(reg, f, indent=2, ensure_ascii=False)

def assign_slots(mods_with_costumes, inv, registry):
    # Build set of mod_ids STILL PRESENT in this run
    present_ids = {mid for mid, _ in mods_with_costumes}

    # Existing assignments (only for mods still present -> freed slots are reused)
    existing = {}
    for s in registry.get("slots", []):
        if s["mod_id"] not in present_ids:
            continue  # mod removed -> slot freed
        existing[(s["mod_id"], s["fighter"], s["original_costume_no"])] = s

    # Per-fighter: taken costume_nos and folder numbers from present mods
    taken_cno = defaultdict(set)
    taken_folder = defaultdict(set)
    for s in existing.values():
        taken_cno[s["fighter"]].add(s["new_costume_no"])
        taken_folder[s["fighter"]].add(int(s["new_folder"]))

    def _smallest_free(taken, lo, hi):
        for v in range(lo, hi + 1):
            if v not in taken:
                return v
        return -1

    slots = []; new_reg = []
    for mod_id, mod_costumes in mods_with_costumes:
        for mc in mod_costumes:
            key = (mod_id, mc.fighter, mc.original_costume_no)
            if key in existing:
                es = existing[key]; si = SlotInfo()
                si.mod_id = mod_id; si.fighter = mc.fighter
                si.fighter_dir = mc.fighter_dir
                si.original_costume_no = mc.original_costume_no
                si.original_folder = mc.original_folder
                si.new_costume_no = es["new_costume_no"]
                si.new_folder = es["new_folder"]
                si.scene_name = es["scene_name"]; si.mod_costume = mc
            else:
                cno = _smallest_free(taken_cno[mc.fighter], 5, 104)
                if cno < 0:
                    print(f"  WARN: no free slot for fighter {mc.fighter}"); continue
                fld_min = inv.max_folder(mc.fighter) + 1
                fld = _smallest_free(taken_folder[mc.fighter], fld_min, fld_min + 200)
                if fld < 0: continue
                taken_cno[mc.fighter].add(cno)
                taken_folder[mc.fighter].add(fld)
                si = SlotInfo(); si.mod_id = mod_id; si.fighter = mc.fighter
                si.fighter_dir = mc.fighter_dir
                si.original_costume_no = mc.original_costume_no
                si.original_folder = mc.original_folder
                si.new_costume_no = cno; si.new_folder = f"{fld:03d}"
                si.scene_name = f"{mc.fighter_dir}v{cno:02d}"; si.mod_costume = mc
            slots.append(si)
            new_reg.append({
                "mod_id": si.mod_id, "fighter": si.fighter,
                "fighter_dir": si.fighter_dir,
                "original_costume_no": si.original_costume_no,
                "original_folder": si.original_folder,
                "new_costume_no": si.new_costume_no,
                "new_folder": si.new_folder, "scene_name": si.scene_name,
            })
    registry["slots"] = new_reg
    return slots

# ============================================================================
# Section 7 -- REasy wrappers (lazy import)
# ============================================================================

_reasy_loaded = False; _type_registry = None

def _init_reasy():
    global _reasy_loaded, _type_registry
    if _reasy_loaded: return
    sys.path.insert(0, REASY)
    from utils.type_registry import TypeRegistry
    _type_registry = TypeRegistry(os.path.join(REASY, "resources", "data", "dumps", "rszsf6.json"))
    info, _ = _type_registry.find_type_by_name("app.FighterCostumeUserDataRecord")
    existing = {f["name"] for f in info["fields"]}
    for fname in ("sortNo", "visualNo", "arrangeId"):
        if fname not in existing:
            info["fields"].append({"align":4,"array":False,"name":fname,"native":False,
                                    "original_type":"System.UInt32","size":4,"type":"U32"})
    _reasy_loaded = True; print("  REasy type registry loaded")

def _parse_rsz(data, filepath):
    _init_reasy()
    from file_handlers.rsz.rsz_file import RszFile
    rsz = RszFile(); rsz.filepath = filepath
    rsz.type_registry = _type_registry; rsz.game_version = "SF6"
    rsz.read(data, validate_type_registry=False); return rsz

def _rsz_guard_rail(rsz, orig_data, label):
    rebuilt = rsz.build()
    if bytes(rebuilt) != orig_data:
        print(f"  FATAL: RSZ round-trip failed for {label}"); sys.exit(1)

# ---- esf.scn.20 ----
def build_esf_scene(vanilla_data, slots, base_fighter_dirs):
    _init_reasy()
    from file_handlers.rsz.rsz_file import RszFolderInfo, RszInstanceInfo
    rsz = _parse_rsz(vanilla_data, "esf.scn.20")
    _rsz_guard_rail(rsz, vanilla_data, "esf.scn.20")
    templates = {}
    for fi in rsz.folder_infos:
        iid = rsz.object_table[fi.id]
        fields = rsz.parsed_elements.get(iid, {})
        name_f = fields.get("Name")
        if not name_f: continue
        name = (name_f.value if hasattr(name_f,"value") else str(name_f)).rstrip("\x00")
        m = re.match(r"(esf\d{3})v00$", name)
        if m: templates[m.group(1)] = (fi, iid)
    added = 0
    for sl in slots:
        if sl.fighter_dir not in base_fighter_dirs: continue
        tmpl = templates.get(sl.fighter_dir)
        if not tmpl: continue
        tmpl_fi, tmpl_iid = tmpl; src = rsz.instance_infos[tmpl_iid]
        new_ii = RszInstanceInfo(); new_ii.type_id = src.type_id; new_ii.crc = src.crc
        new_iid = len(rsz.instance_infos); rsz.instance_infos.append(new_ii)
        nf = {k: copy.deepcopy(v) for k, v in rsz.parsed_elements[tmpl_iid].items()}
        sfx = "\x00" if nf["Name"].value.endswith("\x00") else ""
        nf["Name"].value = sl.scene_name + sfx
        scene_rel = f"Product/CharParam/esf/{sl.fighter_dir}/{sl.scene_name}.scn"
        sp_sfx = "\x00" if nf["ScenePath"].value.endswith("\x00") else ""
        nf["ScenePath"].value = scene_rel + sp_sfx
        rsz.parsed_elements[new_iid] = nf
        new_obj = len(rsz.object_table); rsz.object_table.append(new_iid)
        new_fi = RszFolderInfo(); new_fi.id = new_obj; new_fi.parent_id = tmpl_fi.parent_id
        rsz.folder_infos.append(new_fi); added += 1
    result = rsz.build()
    print(f"  esf.scn.20: {len(vanilla_data)}B -> {len(result)}B, {added} folders added")
    return bytes(result)

# ---- costume table ----
def parse_costume_ids(vanilla_data):
    rsz = _parse_rsz(vanilla_data, "fightercostumeuserdata.user.2")
    root = rsz.object_table[0]; da = rsz.parsed_elements[root]["DataArray"]
    id_map = {}; max_rid = 0; max_mid = 0
    for v in da.values:
        f = rsz.parsed_elements[v.value]
        rid, mid = f["id"].value, f["ManageId"].value
        id_map[(f["fighterId"].value, f["costumeNo"].value)] = rid
        max_rid = max(max_rid, rid); max_mid = max(max_mid, mid)
    _, msg_tid = _type_registry.find_type_by_name(
        "app.FighterCostumeUserDataRecord.FighterCostumeMessage")
    max_msg_id = 0
    for idx, inst in enumerate(rsz.instance_infos):
        if inst.type_id == msg_tid:
            mf = rsz.parsed_elements.get(idx, {})
            if "id" in mf: max_msg_id = max(max_msg_id, mf["id"].value)
    return id_map, max_rid, max_mid, msg_tid, max_msg_id

def build_costume_table(vanilla_data, slots):
    _init_reasy()
    from file_handlers.rsz.rsz_file import RszInstanceInfo
    from file_handlers.rsz.rsz_data_types import ObjectData, U32Data, GuidData
    rsz = _parse_rsz(vanilla_data, "fightercostumeuserdata.user.2")
    _rsz_guard_rail(rsz, vanilla_data, "fightercostumeuserdata.user.2")
    root = rsz.object_table[0]; da = rsz.parsed_elements[root]["DataArray"]
    templates = {}
    for v in da.values:
        f = rsz.parsed_elements[v.value]
        fid, cno = f["fighterId"].value, f["costumeNo"].value
        if cno == 0 and fid not in templates: templates[fid] = v.value
    _, msg_tid = _type_registry.find_type_by_name(
        "app.FighterCostumeUserDataRecord.FighterCostumeMessage")
    msg_tmpl_idx = None
    for idx, inst in enumerate(rsz.instance_infos):
        if inst.type_id == msg_tid: msg_tmpl_idx = idx; break
    for sl in slots:
        ti = templates.get(sl.fighter)
        if ti is None: continue
        src = rsz.instance_infos[ti]
        ni = RszInstanceInfo(); ni.type_id = src.type_id; ni.crc = src.crc
        new_idx = len(rsz.instance_infos); rsz.instance_infos.append(ni)
        nf = {k: copy.deepcopy(v) for k, v in rsz.parsed_elements[ti].items()}
        nf["id"].value = sl.record_id; nf["ManageId"].value = sl.manage_id
        nf["fighterId"].value = sl.fighter; nf["isDefault"].value = True
        nf["costumeNo"].value = sl.new_costume_no; nf["sortNo"].value = sl.sort_no
        nf["visualNo"].value = sl.visual_no; nf["arrangeId"].value = 0
        if msg_tmpl_idx is not None:
            ms = rsz.instance_infos[msg_tmpl_idx]
            mi = RszInstanceInfo(); mi.type_id = ms.type_id; mi.crc = ms.crc
            mix = len(rsz.instance_infos); rsz.instance_infos.append(mi)
            tmf = rsz.parsed_elements[msg_tmpl_idx]
            rsz.parsed_elements[mix] = {
                "id": U32Data(sl.record_id, tmf["id"].orig_type),
                "GUID": GuidData(sl.message_guid, None, tmf["GUID"].orig_type),
            }
            nf["messageId"] = ObjectData(mix, "app.FighterCostumeUserDataRecord.FighterCostumeMessage")
        rsz.parsed_elements[new_idx] = nf
        da.values.append(ObjectData(new_idx, "app.FighterCostumeUserDataRecord"))
    result = rsz.build()
    print(f"  costume table: {len(vanilla_data)}B -> {len(result)}B, {len(slots)} records added")
    return bytes(result)

# ---- msg ----
def build_costume_msg(vanilla_data, slots):
    _init_reasy()
    from file_handlers.msg.msg_handler import MsgHandler
    h = MsgHandler(); h.read(vanilla_data)
    hc = MsgHandler(); hc.read(bytes(h.rebuild()))
    ok = all(h.entries[i].get(k)==hc.entries[i].get(k) for i in range(len(h.entries)) for k in ("uuid","name","content"))
    if not ok: print("  FATAL: MsgHandler rebuild corrupts values"); sys.exit(1)
    n_langs = len(h.useLanguages); existing_names = {e["name"] for e in h.entries}; mc = len(h.entries)
    for sl in slots:
        while f"FighterCostumeMessage{mc}" in existing_names: mc += 1
        sl.message_name = f"FighterCostumeMessage{mc}"
        h.add_entry(uuid_str=sl.message_guid, name=sl.message_name, contents=[sl.message_text]*n_langs)
        mc += 1
    result = h.rebuild()
    print(f"  costume msg: {len(vanilla_data)}B -> {len(result)}B, {len(slots)} entries added")
    return bytes(result)

# ---- color table ----
def build_color_table(vanilla_data, slots):
    _init_reasy()
    from file_handlers.rsz.rsz_file import RszInstanceInfo
    from file_handlers.rsz.rsz_data_types import ObjectData
    rsz = _parse_rsz(vanilla_data, "fightercostumecoloruserdata.user.2")
    _rsz_guard_rail(rsz, vanilla_data, "fightercostumecoloruserdata.user.2")
    root = rsz.object_table[0]; da = rsz.parsed_elements[root]["DataArray"]
    color_map = defaultdict(list); max_cid = 0; max_cmid = 0
    for v in da.values:
        f = rsz.parsed_elements[v.value]
        fcid, cno = f["fighterCostumeId"].value, f["colorNo"].value
        max_cid = max(max_cid, f["id"].value); max_cmid = max(max_cmid, f["ManageId"].value)
        color_map[fcid].append((v.value, cno))
    added = 0; cid = max_cid + 1; cmid = max_cmid + 1
    tmpl_fcid = next((fc for fc, cs in color_map.items() if len(cs) >= 10), None)
    if tmpl_fcid is None:
        print("  WARN: no template colors found"); return bytes(rsz.build())
    for sl in slots:
        tmpl_colors = sorted(color_map[tmpl_fcid], key=lambda x: x[1])
        sl.color_base_id = cid; sl.color_base_manage_id = cmid
        for ti, tc in tmpl_colors[:10]:
            si = rsz.instance_infos[ti]
            ni = RszInstanceInfo(); ni.type_id = si.type_id; ni.crc = si.crc
            nix = len(rsz.instance_infos); rsz.instance_infos.append(ni)
            nf = {k: copy.deepcopy(v) for k, v in rsz.parsed_elements[ti].items()}
            nf["id"].value = cid; nf["ManageId"].value = cmid
            nf["fighterCostumeId"].value = sl.record_id; nf["colorNo"].value = tc
            nf["isDefault"].value = (tc == 0); rsz.parsed_elements[nix] = nf
            tn = (_type_registry.get_type_info(si.type_id) or {}).get("name", "")
            da.values.append(ObjectData(nix, tn)); cid += 1; cmid += 1; added += 1
    result = rsz.build()
    print(f"  color table: {len(vanilla_data)}B -> {len(result)}B, {added} color entries added")
    return bytes(result)

def build_colors_json(slots):
    records = []
    for sl in slots:
        if sl.color_base_id is None: continue
        for i in range(10):
            records.append({"id":sl.color_base_id+i,"ManageId":sl.color_base_manage_id+i,
                            "fighterCostumeId":sl.record_id,"colorNo":i,"isDefault":(i==0)})
    return json.dumps(records, indent=2, ensure_ascii=False)

# ============================================================================
# Section 8 -- Slot file collection + reference closure
# ============================================================================

def _ref_to_pak(rel_path, suffixes):
    """Convert a Resource/UserData relative path (e.g.
    'Product/Model/esf/esf001/001/01/foo.mesh') to a full pak path
    (e.g. 'natives/stm/product/model/esf/esf001/001/01/foo.mesh.230110883').
    Returns '' if the extension has no known version suffix."""
    p = rel_path.replace("\\", "/").strip().rstrip("\x00").lstrip("@")
    if not p: return ""
    base = p.rsplit("/", 1)[-1]
    parts = base.rsplit(".", 1)
    if len(parts) < 2: return ""
    ext = parts[-1].lower()
    suf_str = suffixes.get(ext, "")
    if not suf_str: return ""
    ver = suf_str.rsplit(".", 1)[-1]
    return f"natives/stm/{p.lower()}.{ver}"


def _extract_rsz_refs(data, label, suffixes):
    """Parse RSZ data, return set of pak paths referenced via Resource/UserData.
    Returns (resolved_pak_paths, raw_rel_paths_without_suffix)."""
    _init_reasy()
    from file_handlers.rsz.rsz_data_types import ResourceData, UserDataData, ArrayData
    paks_out = set()
    raw_out = set()
    try:
        rsz = _parse_rsz(data, label)
        for idx in range(len(rsz.instance_infos)):
            for _fname, fval in rsz.parsed_elements.get(idx, {}).items():
                vals = []
                if isinstance(fval, ResourceData):
                    vals.append(fval.value)
                elif isinstance(fval, UserDataData):
                    vals.append(fval.string)
                elif isinstance(fval, ArrayData):
                    for elt in fval.values:
                        if isinstance(elt, ResourceData):
                            vals.append(elt.value)
                for raw in vals:
                    if not raw: continue
                    p = raw.strip().rstrip("\x00").lstrip("@")
                    if not p: continue
                    raw_out.add(p)
                    pak = _ref_to_pak(p, suffixes)
                    if pak: paks_out.add(pak)
    except Exception:
        pass
    return paks_out, raw_out


def close_references(writer, slots, inv, paks, all_mod_files):
    """After building slot files, scan every patched output file for rewritten
    references whose target does not exist.  Copy the original file (with
    content patching) to close the gap.  Iterate until stable.
    C++ port: same loop with RSZ reference extraction or binary scan."""
    suffixes = inv.suffixes
    dead_vanilla = []  # references whose original doesn't exist either

    # Collect all patched content for re-reading (the PakWriter doesn't
    # expose stored data, so we keep a side-dict of patched bytes)
    patched_cache = {}  # pak_path -> bytes  (only patched content files)

    for sl in slots:
        fd = sl.fighter_dir
        old_f = sl.original_folder; new_f = sl.new_folder
        old_dir = f"{fd}/{old_f}/"; new_dir = f"{fd}/{new_f}/"
        old_pfx = f"{fd}_{old_f}_"; new_pfx = f"{fd}_{new_f}_"

        # Determine which output paths belong to this slot and are patched
        slot_patched = [p for p in writer.expected_paths
                        if needs_content_patch(p)
                        and (new_dir in p or new_pfx in p.rsplit("/",1)[-1]
                             or f"/{sl.scene_name}." in p)]
        # Also include the costume scene
        scene_path = f"natives/stm/product/charparam/esf/{fd}/{sl.scene_name}.scn.20"
        if scene_path not in slot_patched and scene_path in writer.expected_paths:
            slot_patched.append(scene_path)

        iteration = 0
        while True:
            iteration += 1
            added_this_round = 0

            for out_path in list(slot_patched):
                # Get the patched data (from cache or by re-reading the slot content)
                if out_path not in patched_cache:
                    # We need the data.  Re-derive it the same way add_slot_files did.
                    # For the scene: it was patched from the vanilla scene.
                    # For model files: patched from vanilla or mod.
                    # This is expensive but only runs for patched files (~10 per slot).
                    orig_path = out_path
                    if new_dir in orig_path:
                        orig_path = orig_path.replace(new_dir, old_dir, 1)
                    fn = orig_path.rsplit("/", 1)[-1] if "/" in orig_path else orig_path
                    fn = fn.replace(new_pfx, old_pfx, 1)
                    if "/" in orig_path:
                        orig_path = orig_path.rsplit("/", 1)[0] + "/" + fn
                    else:
                        orig_path = fn
                    # Special case: costume scene
                    if sl.scene_name in out_path:
                        orig_costume = inv.get_costume(sl.fighter, sl.original_costume_no)
                        if orig_costume:
                            orig_path = orig_costume["scene"]
                    h = filepath_hash(orig_path)
                    # Try mod first, then vanilla
                    if orig_path in all_mod_files:
                        raw_data = all_mod_files[orig_path].read()
                    elif paks.has(h):
                        raw_data = paks.read_data(h)
                    else:
                        continue
                    patched_data, _ = patch_paths(raw_data, fd, old_f, new_f)
                    patched_cache[out_path] = patched_data

                data = patched_cache[out_path]
                ref_paks, ref_raws = _extract_rsz_refs(data, os.path.basename(out_path), suffixes)

                for ref_pak in ref_paks:
                    if writer.has_path(ref_pak): continue
                    if paks.has(filepath_hash(ref_pak)): continue

                    # This reference is dangling.  Compute the original path.
                    orig_ref = ref_pak
                    if new_dir in orig_ref:
                        orig_ref = orig_ref.replace(new_dir, old_dir, 1)
                    ofn = orig_ref.rsplit("/", 1)[-1] if "/" in orig_ref else orig_ref
                    ofn = ofn.replace(new_pfx, old_pfx, 1)
                    if "/" in orig_ref:
                        orig_ref = orig_ref.rsplit("/", 1)[0] + "/" + ofn
                    else:
                        orig_ref = ofn

                    orig_h = filepath_hash(orig_ref)
                    # Check original in mod files, base pak
                    if orig_ref in all_mod_files:
                        orig_data = all_mod_files[orig_ref].read()
                    elif paks.has(orig_h):
                        orig_data = None  # will read below
                    else:
                        dead_vanilla.append((out_path, ref_pak, orig_ref))
                        continue

                    if needs_content_patch(ref_pak):
                        if orig_data is None:
                            orig_data = paks.read_data(orig_h)
                        patched, _ = patch_paths(orig_data, fd, old_f, new_f)
                        writer.add_uncompressed(ref_pak, patched)
                        patched_cache[ref_pak] = patched
                        slot_patched.append(ref_pak)  # scan this new file too
                    else:
                        if orig_data is not None:
                            writer.add_compressed(ref_pak, orig_data)
                        else:
                            raw, ds, att = paks.read_raw(orig_h)
                            writer.add_passthrough(ref_pak, raw, ds, att)
                    added_this_round += 1

            if added_this_round == 0:
                break
            print(f"    closure round {iteration}: {added_this_round} files added for {fd}/{new_f}")

    if dead_vanilla:
        print(f"  reference closure: {len(dead_vanilla)} dead-in-vanilla references ignored:")
        seen = set()
        for src, ref, orig in dead_vanilla:
            if ref not in seen:
                print(f"    {ref} (original {orig} not in base pak either)")
                seen.add(ref)

    return dead_vanilla


def add_slot_files(writer, slot, inv, paks, scope="minimal", all_mod_files=None):
    """Add model files for one slot.
    scope='minimal': relocate only mod files; scene/mdf2 targeted patching
                     (matches manual pak behavior -- proven in game).
    scope='full':    relocate ALL vanilla+mod files; global path replacement
                     (experimental, needs reference closure)."""
    mc = slot.mod_costume; fd = slot.fighter_dir
    old_f = slot.original_folder; new_f = slot.new_folder
    model_prefix = inv.model_prefix(fd, old_f)
    mod_in_folder = mc.mod_files_in_folder
    count = 0; patched_count = 0; streaming_count = 0

    if scope == "minimal":
        # -- MINIMAL: relocate only mod-provided files -----------------------
        # Build set of mod texture stems for targeted mdf2 repointing
        mod_tex_stems = set()
        for p in mod_in_folder:
            fn = p.rsplit("/", 1)[-1]
            parts = fn.rsplit(".", 1)  # remove version suffix: 'rope_albd.tex.241101895' -> 'rope_albd.tex'
            if len(parts) == 2 and ".tex." in fn:
                mod_tex_stems.add(parts[0])

        streaming_count = 0
        relocated_tex_pairs = []  # (orig, new) for streaming twin processing
        for path in sorted(mod_in_folder.keys()):
            lower = path.lower()
            fn_low = lower.rsplit("/", 1)[-1]
            # Chain/havok files: dir-only relocation (keep original filename prefix)
            is_chain_file = ("_chain.chain." in fn_low or "_chain.user." in fn_low
                             or "_havok." in fn_low or "havokcloth" in fn_low)
            # Streaming textures: same relocation as their base counterpart
            is_streaming = lower.startswith(_STREAM_PFX)
            if is_streaming:
                # Derive relocation from the base path
                base_path = _BASE_PFX + path[len(_STREAM_PFX):]
                base_new = (relocate_path_dir_only(base_path, fd, old_f, new_f)
                            if is_chain_file
                            else relocate_path(base_path, fd, old_f, new_f))
                new_path = _STREAM_PFX + base_new[len(_BASE_PFX):]
            else:
                new_path = (relocate_path_dir_only(path, fd, old_f, new_f)
                            if is_chain_file
                            else relocate_path(path, fd, old_f, new_f))
            data = mod_in_folder[path].read()
            if lower.endswith(".mdf2.31"):
                data, n = patch_mdf2_minimal(data, fd, old_f, new_f, mod_tex_stems)
                if n > 0: patched_count += 1
                writer.add_uncompressed(new_path, data)
            elif needs_content_patch(path):
                writer.add_uncompressed(new_path, data)
            else:
                raw = mod_in_folder[path].read_raw_if_pak()
                if raw:
                    writer.add_passthrough(new_path, *raw)
                else:
                    writer.add_compressed(new_path, data)
            count += 1
            if ".tex." in lower and not is_streaming:
                relocated_tex_pairs.append((path, new_path))
        # Write streaming twins for relocated textures
        amf = all_mod_files or {}
        for orig, new in relocated_tex_pairs:
            streaming_count += _write_streaming_twin(writer, orig, new, amf, paks)

        # ---- Phase 1b: vanilla mdf2 relocation if mod has none ----
        has_mod_mdf2 = any(p.lower().endswith(".mdf2.31") for p in mod_in_folder)
        if not has_mod_mdf2:
            # Collect hashes present in the mod pak (for texture matching)
            mod_pak_hashes = set()
            for mf in mod_in_folder.values():
                if mf._pak_source:
                    mod_pak_hashes.add(mf._pak_source[1])
            tex_version = inv.suffixes.get("tex", ".tex.241101895").rsplit(".", 1)[-1]
            # Find vanilla mdf2 files in this costume's model folder
            for vh, vpaths in inv.hash_to_paths.items():
                for vp in vpaths:
                    if not vp.lower().startswith(model_prefix.lower()):
                        continue
                    if not vp.lower().endswith(".mdf2.31"):
                        continue
                    new_path = relocate_path(vp, fd, old_f, new_f)
                    if writer.has_path(new_path):
                        continue
                    if not paks.has(vh):
                        continue
                    mdf_data = bytearray(paks.read_data(vh))
                    # Patch: for each tex ref, if its hash is in the mod pak,
                    # replace the dir segment (dir-only, keep filename)
                    old_d = f"{fd}/{old_f}/".encode("utf-16-le")
                    new_d = f"{fd}/{new_f}/".encode("utf-16-le")
                    ci = 0; n_patched = 0
                    while True:
                        ci = mdf_data.find(old_d, ci)
                        if ci < 0:
                            break
                        # Read forward to null terminator to get full ref
                        ce = ci + len(old_d)
                        while ce < len(mdf_data) - 1:
                            if mdf_data[ce] == 0 and mdf_data[ce + 1] == 0:
                                break
                            ce += 2
                        try:
                            ref = bytes(mdf_data[ci:ce]).decode("utf-16-le",
                                                                 errors="replace")
                            lr = ref.lower()
                            # Build pak path for this texture
                            tex_pak = f"natives/stm/product/model/esf/{lr}.{tex_version}"
                            tex_h = filepath_hash(tex_pak)
                            if tex_h in mod_pak_hashes:
                                mdf_data[ci:ci + len(old_d)] = new_d
                                n_patched += 1
                        except Exception:
                            pass
                        ci += len(old_d)
                    writer.add_uncompressed(new_path, bytes(mdf_data))
                    count += 1
                    if n_patched > 0:
                        patched_count += 1

        # ---- Phase 2: CCVD + CMD + chain (dir-only relocation) ----
        # The CCVD references CMD_000..010 + CMD_Dx_001..007.  We relocate
        # the CCVD with directory-only patch of its content, and relocate ALL
        # CMD files (mod version if present, vanilla otherwise) so the CCVD's
        # patched references resolve.  Filenames keep their _001_ prefix.
        amf = all_mod_files or {}
        base_model = f"natives/stm/product/model/esf/{fd}/{old_f}/"

        # 2a. CCVD: relocate dir-only, patch content (dir segment only)
        ccvd_orig = f"{base_model}{fd}_{old_f}_ccvd.user.2"
        ccvd_target = relocate_path_dir_only(ccvd_orig, fd, old_f, new_f)
        if not writer.has_path(ccvd_target):
            src = mc.mod_files_in_folder.get(ccvd_orig) or amf.get(ccvd_orig)
            if src:
                ccvd_data = src.read()
            elif paks.has(filepath_hash(ccvd_orig)):
                ccvd_data = paks.read_data(filepath_hash(ccvd_orig))
            else:
                ccvd_data = None
            if ccvd_data is not None:
                # Patch dir segment in CCVD content (all CMD refs: 001/ -> 006/)
                old_d = f"{fd}/{old_f}/".encode("utf-16-le")
                new_d = f"{fd}/{new_f}/".encode("utf-16-le")
                ccvd_data = bytes(bytearray(ccvd_data).replace(old_d, new_d))
                writer.add_uncompressed(ccvd_target, ccvd_data)
                count += 1; patched_count += 1

        # 2b. CMD files: discover from CCVD content, relocate dir-only
        # Parse the PATCHED ccvd binary for UserData paths to find all CMD refs
        cmd_names = set()
        if ccvd_data is not None:
            old_dir_s = f"{fd}/{new_f}/"  # already patched dir
            # Scan UTF-16LE for .user references under the (now patched) folder
            pattern = old_dir_s.encode("utf-16-le")
            ci = 0
            while True:
                ci = ccvd_data.find(pattern, ci)
                if ci < 0: break
                # Read forward to null terminator
                ce = ci
                while ce < len(ccvd_data) - 1:
                    if ccvd_data[ce] == 0 and ccvd_data[ce+1] == 0: break
                    ce += 2
                try:
                    ref = bytes(ccvd_data[ci:ce]).decode("utf-16-le", errors="replace")
                    fn = ref.rsplit("/", 1)[-1].rstrip("\x00").lower()
                    if fn.endswith(".user"):
                        # Convert to pak filename: fn + ".2" (version suffix)
                        cmd_names.add(fn + ".2")
                except: pass
                ci += len(pattern)
        # Fallback: hardcoded set if CCVD parsing found nothing
        if not cmd_names:
            cmd_names = set(f"{fd}_{old_f}_cmd_{i:03d}.user.2" for i in range(11))
            cmd_names |= set(f"{fd}_{old_f}_cmd_dx_{i:03d}.user.2" for i in range(1, 8))
        for cn in sorted(cmd_names):
            # cn is the filename (e.g. 'esf022_002_cmd_ex_001.user.2')
            # Originals live in the OLD folder
            cmd_orig = f"{base_model}{cn}"
            cmd_target = relocate_path_dir_only(cmd_orig, fd, old_f, new_f)
            # If filename uses old_f prefix, keep it for dir-only relocation
            if writer.has_path(cmd_target):
                continue
            src = mc.mod_files_in_folder.get(cmd_orig) or amf.get(cmd_orig)
            if src:
                raw = src.read_raw_if_pak()
                if raw: writer.add_passthrough(cmd_target, *raw)
                else: writer.add_compressed(cmd_target, src.read())
                count += 1
            elif paks.has(filepath_hash(cmd_orig)):
                raw, ds, att = paks.read_raw(filepath_hash(cmd_orig))
                writer.add_passthrough(cmd_target, raw, ds, att)
                count += 1

        # 2c. chain.52 (part 01): from THIS mod only, else vanilla
        # (dir-only relocation: keep filename prefix)
        chain_orig = f"{base_model}{fd}_{old_f}_01_chain.chain.52"
        chain_target = relocate_path_dir_only(chain_orig, fd, old_f, new_f)
        if not writer.has_path(chain_target):
            src = mc.mod_files_in_folder.get(chain_orig)
            if src:
                writer.add_compressed(chain_target, src.read()); count += 1
            elif paks.has(filepath_hash(chain_orig)):
                raw, ds, att = paks.read_raw(filepath_hash(chain_orig))
                writer.add_passthrough(chain_target, raw, ds, att); count += 1

        # 2d. chain.user: for each part with mod-provided chain.chain,
        # relocate vanilla chain.user dir-only with internal dir patching
        mod_chain_parts = set()
        for path in mc.mod_files_in_folder:
            fn = path.rsplit("/", 1)[-1].lower()
            if "_chain.chain." in fn or "_havok." in fn:
                m_cp = MODEL_FOLDER_RE.search(path)
                if m_cp:
                    rest = path[m_cp.end():]
                    if "/" in rest:
                        mod_chain_parts.add(rest.split("/")[0])
        for part in sorted(mod_chain_parts):
            cu_orig = f"{base_model}{fd}_{old_f}_{part}_chain.user.2"
            cu_target = relocate_path_dir_only(cu_orig, fd, old_f, new_f)
            if writer.has_path(cu_target):
                continue
            src = mc.mod_files_in_folder.get(cu_orig)
            if src:
                cu_data = src.read()
            elif paks.has(filepath_hash(cu_orig)):
                cu_data = paks.read_data(filepath_hash(cu_orig))
            else:
                continue
            # Patch internal dir ref: fd/old_f/part/ -> fd/new_f/part/
            old_d = f"{fd}/{old_f}/{part}/".encode("utf-16-le")
            new_d = f"{fd}/{new_f}/{part}/".encode("utf-16-le")
            cu_data = bytes(bytearray(cu_data).replace(old_d, new_d))
            writer.add_uncompressed(cu_target, cu_data)
            count += 1; patched_count += 1

        # ---- Phase 1c: Relocate mod-provided 000/ shared files ----
        # When a mod provides mesh/mdf2/tex under 000/<part>/, relocate them
        # to <new_folder>/<part>/ so the slot gets its own copy.
        shared_parts = set()  # track which 000/ parts we relocated
        for path in sorted(mc.mod_files_shared.keys()):
            m_sh = MODEL_FOLDER_RE.search(path)
            if not m_sh or m_sh.group(2) != "000":
                continue
            rest = path[m_sh.end():]  # e.g. "00/esf010_000_00.mesh.230110883"
            part = rest.split("/")[0] if "/" in rest else ""
            if not part:
                continue
            # Relocate: 000/<part>/ -> <new_f>/<part>/, rename stem 000 -> new_f
            new_path = relocate_path(path, fd, "000", new_f)
            if writer.has_path(new_path):
                continue
            shared_parts.add(part)
            data = mc.mod_files_shared[path].read()
            lower = path.lower()
            if lower.endswith(".mdf2.31"):
                # Build tex stems for shared-part mdf2 patching
                sh_tex_stems = set()
                for sp in mc.mod_files_shared:
                    if f"/{fd}/000/{part}/" in sp:
                        fn2 = sp.rsplit("/", 1)[-1]
                        pts = fn2.rsplit(".", 1)
                        if len(pts) == 2 and ".tex." in fn2:
                            sh_tex_stems.add(pts[0])
                data, n = patch_mdf2_minimal(data, fd, "000", new_f, sh_tex_stems)
                if n > 0: patched_count += 1
                writer.add_uncompressed(new_path, data)
            elif needs_content_patch(path):
                writer.add_uncompressed(new_path, data)
            else:
                raw_sh = mc.mod_files_shared[path].read_raw_if_pak()
                if raw_sh:
                    writer.add_passthrough(new_path, *raw_sh)
                else:
                    writer.add_compressed(new_path, data)
            count += 1
        if shared_parts:
            # Streaming twins for shared textures
            for path in sorted(mc.mod_files_shared.keys()):
                if ".tex." not in path or "/000/" not in path:
                    continue
                sh_new = relocate_path(path, fd, "000", new_f)
                streaming_count += _write_streaming_twin(writer, path, sh_new, amf, paks)
            print(f"      relocated {len([p for p in mc.mod_files_shared if '/000/' in p])} "
                  f"shared files from 000/ parts {sorted(shared_parts)}")

        # ---- Phase 3: External files matching this costume's prefix ----
        # Weather chains and other files outside model/esf identified by
        # filename containing esfXXX_<folder>_
        ext_prefix = f"{fd}_{old_f}_"
        ext_count = 0
        for path, mf in mc.mod_files_other.items():
            fn = path.rsplit("/", 1)[-1]
            if ext_prefix not in fn:
                continue
            if "/product/model/esf/" in path:
                continue  # already handled
            new_fn = fn.replace(f"{fd}_{old_f}_", f"{fd}_{new_f}_", 1)
            new_path = path.rsplit("/", 1)[0] + "/" + new_fn if "/" in path else new_fn
            if writer.has_path(new_path):
                continue
            raw_ext = mf.read_raw_if_pak()
            if raw_ext:
                writer.add_passthrough(new_path, *raw_ext)
            else:
                writer.add_compressed(new_path, mf.read())
            count += 1; ext_count += 1
        if ext_count:
            print(f"      {ext_count} extra-model files relocated")

        # Build set of all relocated pak paths (for scene reference check)
        relocated_set = set(writer.expected_paths)  # includes everything added so far

        # Costume scene: targeted patching (only mesh/mdf2/CCVD refs that exist)
        orig_costume = inv.get_costume(slot.fighter, slot.original_costume_no)
        if orig_costume:
            h = filepath_hash(orig_costume["scene"])
            if paks.has(h):
                scene_data = paks.read_data(h)
                patched_scene, n = patch_scene_minimal(
                    scene_data, fd, old_f, new_f,
                    relocated_pak_paths=relocated_set, suffixes=inv.suffixes)
                # Second pass: patch 000/ references for parts we relocated
                if shared_parts:
                    patched_scene, n2 = patch_scene_minimal(
                        patched_scene, fd, "000", new_f,
                        relocated_pak_paths=relocated_set, suffixes=inv.suffixes)
                    n += n2
                # Third pass: weather chain filename-prefix rename in scene
                # References like "esf010_002_02_chain.chain" -> "esf010_006_02_chain.chain"
                weather_mode = getattr(slot, '_weather_chain', 'rename')
                if weather_mode == 'rename' and mod_chain_parts:
                    scene_buf = bytearray(patched_scene)
                    old_wpfx = f"{fd}_{old_f}_".encode("utf-16-le")
                    new_wpfx = f"{fd}_{new_f}_".encode("utf-16-le")
                    # Only patch weather/havok chain refs (not model refs)
                    wi = 0
                    while True:
                        wi = scene_buf.find(old_wpfx, wi)
                        if wi < 0: break
                        # Read the full string to check it's a chain ref
                        we = wi
                        while we < len(scene_buf) - 1:
                            if scene_buf[we] == 0 and scene_buf[we+1] == 0: break
                            we += 2
                        try:
                            wstr = bytes(scene_buf[wi:we]).decode("utf-16-le", errors="replace").lower()
                            if "chain.chain" in wstr or "havok" in wstr:
                                scene_buf[wi:wi+len(old_wpfx)] = new_wpfx
                                n += 1
                        except: pass
                        wi += len(old_wpfx)
                    patched_scene = bytes(scene_buf)
                new_scene = f"natives/stm/product/charparam/esf/{fd}/{slot.scene_name}.scn.20"
                writer.add_uncompressed(new_scene, patched_scene)
                count += 1; patched_count += 1

    else:
        # -- FULL: relocate all vanilla+mod files, global patching -----------
        vanilla_files = inv.costume_model_files(slot.fighter, slot.original_costume_no)
        vanilla_in_folder = {fi["path"]: fi for fi in vanilla_files
                             if fi["path"].lower().startswith(model_prefix.lower())}
        all_paths = set(vanilla_in_folder.keys()) | set(mod_in_folder.keys())
        for path in sorted(all_paths):
            new_path = relocate_path(path, fd, old_f, new_f)
            if path in mod_in_folder:
                data = mod_in_folder[path].read()
            else:
                h = filepath_hash(path)
                if not paks.has(h): continue
                data = paks.read_data(h)
            if needs_content_patch(path):
                data, n = patch_paths(data, fd, old_f, new_f)
                if n > 0: patched_count += 1
                writer.add_uncompressed(new_path, data)
            else:
                if path in mod_in_folder:
                    raw = mod_in_folder[path].read_raw_if_pak()
                    if raw: writer.add_passthrough(new_path, *raw)
                    else: writer.add_compressed(new_path, data)
                else:
                    h = filepath_hash(path)
                    raw, ds, att = paks.read_raw(h)
                    writer.add_passthrough(new_path, raw, ds, att)
            count += 1
        # Costume scene: global patching
        orig_costume = inv.get_costume(slot.fighter, slot.original_costume_no)
        if orig_costume:
            h = filepath_hash(orig_costume["scene"])
            if paks.has(h):
                scene_data = paks.read_data(h)
                patched_scene, _ = patch_paths(scene_data, fd, old_f, new_f)
                new_scene = f"natives/stm/product/charparam/esf/{fd}/{slot.scene_name}.scn.20"
                writer.add_uncompressed(new_scene, patched_scene); count += 1

    if streaming_count:
        print(f"      {streaming_count} streaming textures relocated")
    print(f"    slot {fd}/{new_f} (v{slot.new_costume_no:02d}): {count} files, {patched_count} patched [{scope}]")


def add_restorations(writer, slots, inv, paks, all_mod_files):
    relocated_prefixes = set()
    for sl in slots:
        relocated_prefixes.add(inv.model_prefix(sl.fighter_dir, sl.original_folder).lower())
    restored = 0; added = 0; streaming_restored = 0
    for pak_path, mod_file in all_mod_files.items():
        if writer.has_path(pak_path): continue
        # Skip streaming/ paths here (handled below as twins)
        if pak_path.startswith(_STREAM_PFX):
            continue
        in_relocated = any(pak_path.startswith(pfx) for pfx in relocated_prefixes)
        h = filepath_hash(pak_path)
        if paks.has(h):
            raw, dsize, attrib = paks.read_raw(h)
            writer.add_passthrough(pak_path, raw, dsize, attrib); restored += 1
            # Also restore streaming twin if it exists
            if ".tex." in pak_path and pak_path.startswith(_BASE_PFX):
                sp = _STREAM_PFX + pak_path[len(_BASE_PFX):]
                if not writer.has_path(sp):
                    sh = filepath_hash(sp)
                    if paks.has(sh):
                        sr, sd, sa = paks.read_raw(sh)
                        writer.add_passthrough(sp, sr, sd, sa)
                        streaming_restored += 1
        elif "/model/esf/" in pak_path and not in_relocated:
            data = mod_file.read()
            writer.add_compressed(pak_path, data); added += 1
    s_msg = f", {streaming_restored} streaming" if streaming_restored else ""
    print(f"  restored {restored} vanilla files, added {added} mod-only files{s_msg}")

# ============================================================================
# Section 9 -- Verification
# ============================================================================

def verify_output(pak_path, expected_paths, base_paks, slots=None, inv=None, scope="minimal"):
    print("\n=== VERIFICATION ===")
    vpak = PakIndex(); vpak.add_pak(pak_path)
    missing = [p for p in sorted(expected_paths) if not vpak.has(filepath_hash(p))]
    if missing:
        print(f"  FAIL: {len(missing)} expected paths missing:")
        for m in missing[:20]: print(f"    {m}")
    else:
        print(f"  OK: all {len(expected_paths)} expected paths present")

    folder_remap = {}
    if slots:
        for sl in slots: folder_remap[(sl.fighter_dir, sl.new_folder)] = sl.original_folder

    # In minimal mode, mdf2 and ccvd files intentionally keep vanilla refs.
    # Only the scene should have NO stale refs.
    stale = []
    for p in sorted(expected_paths):
        if not needs_content_patch(p): continue
        # In minimal mode, only check scene files for stale refs (mdf2/user keep vanilla)
        if scope == "minimal" and not p.lower().endswith(".scn.20"):
            continue
        h = filepath_hash(p)
        if not vpak.has(h): continue
        m = MODEL_FOLDER_RE.search(p)
        if not m: continue
        fd, nf = m.group(1).lower(), m.group(2)
        old_f = folder_remap.get((fd, nf))
        if not old_f: continue
        data = vpak.read_data(h)
        for enc in ("utf-16-le", "utf-8"):
            for ps in (f"{fd}/{old_f}/", f"{fd}_{old_f}_", f"{fd.upper()}/{old_f}/", f"{fd.upper()}_{old_f}_"):
                cnt = data.count(ps.encode(enc))
                if cnt > 0: stale.append((p, ps, enc, cnt))
    if stale:
        print(f"  WARN: {len(stale)} stale old-folder references found:")
        for path, ref, enc, cnt in stale[:20]: print(f"    {path}: {cnt}x '{ref}' ({enc})")
    else:
        print(f"  OK: no stale old-folder references in patched files")

    # Check for dangling NEW-folder references: parse each patched file,
    # extract Resource/UserData refs, verify each exists in output or base pak
    suffixes = inv.suffixes if inv else {}
    dangling = []
    patched_with_new = []
    for p in sorted(expected_paths):
        if not needs_content_patch(p): continue
        # Check if this file belongs to any slot (has new folder in path or filename)
        slot_match = None
        for sl in (slots or []):
            nd = f"{sl.fighter_dir}/{sl.new_folder}/"
            np = f"{sl.fighter_dir}_{sl.new_folder}_"
            if nd in p or np in p.rsplit("/",1)[-1] or f"/{sl.scene_name}." in p:
                slot_match = sl; break
        if not slot_match: continue
        h = filepath_hash(p)
        if not vpak.has(h): continue
        data = vpak.read_data(h)
        ref_paks, _ = _extract_rsz_refs(data, os.path.basename(p), suffixes)
        for rp in ref_paks:
            if vpak.has(filepath_hash(rp)): continue
            if base_paks.has(filepath_hash(rp)): continue
            # Dangling reference
            dangling.append((p, rp))

    if dangling:
        print(f"  FAIL: {len(dangling)} dangling new-folder references:")
        seen = set()
        for src, ref in dangling:
            if ref not in seen:
                print(f"    {os.path.basename(src)} -> {ref}")
                seen.add(ref)
    else:
        print(f"  OK: no dangling new-folder references")

    # Check 000/ restorations are vanilla (not mod) blobs
    res_ok = True
    for p in sorted(expected_paths):
        if "/esf" not in p or "/000/" not in p: continue
        h = filepath_hash(p)
        if not vpak.has(h) or not base_paks.has(h): continue
        out_data = vpak.read_data(h); van_data = base_paks.read_data(h)
        if out_data != van_data:
            print(f"  WARN: {p} differs from vanilla (mod blob leaked?)"); res_ok = False
    if res_ok:
        print(f"  OK: 000/ restorations match vanilla blobs")

    vpak.close()
    return len(missing) == 0 and len(stale) == 0 and len(dangling) == 0

# ============================================================================
# Section 10 -- make-fluffy-pak helper
# ============================================================================

def make_fluffy_pak(mod_dir, out_pak):
    """Build the pak that Fluffy would produce from a mod directory."""
    print(f"Building Fluffy pak from {mod_dir} -> {out_pak}")
    writer = PakWriter()
    for root, _dirs, names in os.walk(mod_dir):
        for name in names:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, mod_dir).replace("\\", "/")
            idx = rel.lower().find("natives/stm/")
            if idx < 0: continue
            pak_path = rel[idx:].lower()
            p = full
            if len(p) > 240 and not p.startswith("\\\\?\\"):
                p = "\\\\?\\" + os.path.abspath(p)
            with open(p, "rb") as f: data = f.read()
            writer.add_compressed(pak_path, data)
    writer.write(out_pak)

# ============================================================================
# Section 11 -- Main
# ============================================================================

def main():
    t0 = time.time()
    parser = argparse.ArgumentParser(description="SF6 Costume Slot Loader (prototype)")
    parser.add_argument("game_dir", nargs="?", default=GAME_DIR_DEFAULT)
    parser.add_argument("--mod-dir", action="append", default=[],
                        help="Fluffy mod directory (repeatable, dev mode)")
    parser.add_argument("--pak-dir", default=None,
                        help="Directory containing mod patch paks (default: game root)")
    parser.add_argument("--ignore", action="append", default=[])
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--install", action="store_true",
                        help="Write pak to game/pak dir instead of out/")
    parser.add_argument("--make-fluffy-pak", nargs=2, metavar=("MOD_DIR","OUT_PAK"),
                        help="Build a Fluffy-style pak from a mod directory")
    parser.add_argument("--registry-dir", default=None,
                        help="Override registry directory")
    parser.add_argument("--patch-scope", choices=["minimal", "full"], default="minimal",
                        help="minimal (default): rewrite only mesh/mdf2/CCVD refs, "
                             "proven in game.  full: global rewrite + closure (experimental).")
    parser.add_argument("--static", action="store_true",
                        help="Use pre-built structural files from out/static/ instead of "
                             "building them at runtime.  Slots constrained to 5..10.")
    parser.add_argument("--weather-chain", choices=["rename", "skip"], default="rename",
                        help="Weather chain handling: rename (default) rewrites scene "
                             "references to use mod's chain; skip keeps vanilla.")
    args = parser.parse_args()
    game_dir = args.game_dir

    # ---- make-fluffy-pak mode ----
    if args.make_fluffy_pak:
        make_fluffy_pak(args.make_fluffy_pak[0], args.make_fluffy_pak[1])
        return

    print("=== SF6 Costume Slot Loader ===\n")

    # 1. Load vanilla inventory
    print("Loading vanilla inventory...")
    inv = VanillaInventory(VANILLA_JSON)
    print(f"  {len(inv.costumes)} costumes, {len(inv.all_hashes)} unique file hashes")

    # 2. Open base pak index
    print("Loading base pak indices...")
    paks = PakIndex()
    paks.add_pak(os.path.join(game_dir, "re_chunk_000.pak"))
    for dlc in ("re_dlc_stm_1792750.pak", "re_dlc_stm_1792751.pak"):
        paks.add_pak(os.path.join(game_dir, "dlc", dlc))

    # 3. Scan mods
    mods_with_costumes = []
    all_mod_files = {}
    mod_pak_indexes = []   # keep alive for pak-source ModFiles

    if args.mod_dir:
        # ---- Dev mode: scan directories ----
        print("\nScanning mod directories...")
        for md in args.mod_dir:
            if not os.path.isdir(md):
                print(f"  WARN: mod dir not found: {md}"); continue
            mod_id = identify_mod_dir(md)
            mod_files = scan_mod_dir(md)
            print(f"  {os.path.basename(md)}: {len(mod_files)} files (id={mod_id})")
            costumes = detect_mod_costumes(mod_files, inv)
            for mc in costumes:
                print(f"    -> {mc.fighter_dir} costume {mc.original_costume_no} "
                      f"(folder {mc.original_folder}): {len(mc.mod_files_in_folder)} files")
            mods_with_costumes.append((mod_id, costumes))
            all_mod_files.update(mod_files)
    else:
        # ---- Production mode: scan patch paks ----
        pak_dir = args.pak_dir or game_dir
        print(f"\nScanning patch paks in {pak_dir}...")
        our_paks, mod_paks = classify_patch_paks(pak_dir, args.ignore)
        if our_paks:
            print(f"  our paks (will be replaced): {[n for n,_ in our_paks]}")
        if not mod_paks:
            print("  no mod paks found")

        for num, pak_path in sorted(mod_paks):
            print(f"\n  patch_{num:03d}: {os.path.basename(pak_path)}")
            mod_pak = PakIndex()
            mod_pak.add_pak(pak_path)
            mod_pak_indexes.append(mod_pak)  # keep alive

            mod_id = identify_mod_pak(pak_path)
            mod_files, unattributed = scan_mod_pak(mod_pak, inv)
            print(f"    resolved: {len(mod_files)} paths, unattributed: {len(unattributed)} hashes")
            if unattributed:
                for uh in unattributed[:5]:
                    print(f"      {uh:016X}")
                if len(unattributed) > 5:
                    print(f"      ... and {len(unattributed)-5} more")

            costumes = detect_mod_costumes(mod_files, inv)
            for mc in costumes:
                print(f"    -> {mc.fighter_dir} costume {mc.original_costume_no} "
                      f"(folder {mc.original_folder}): {len(mc.mod_files_in_folder)} files")
            mods_with_costumes.append((mod_id, costumes))
            all_mod_files.update(mod_files)

    # ---- Scan costume_mods folders (both dev and production modes) ----
    cm_mods, cm_files, cm_pak_indexes, cm_infos = scan_costume_mods(game_dir, inv)
    if cm_mods:
        # Folder mods come AFTER pak mods (alphabetical order within folders)
        mods_with_costumes.extend(cm_mods)
        all_mod_files.update(cm_files)
        mod_pak_indexes.extend(cm_pak_indexes)

    if not mods_with_costumes or not any(cs for _, cs in mods_with_costumes):
        print("No costume mods detected. Nothing to do."); paks.close(); return

    # 4. Registry
    reg_dir = args.registry_dir or os.path.join(game_dir, "reframework", "data", "SF6_Costumes_Data")
    registry_path = os.path.join(reg_dir, "registry.json")
    registry = load_registry(registry_path)

    # 5. Assign slots
    print("\nAssigning slots...")
    slots = assign_slots(mods_with_costumes, inv, registry)
    if not slots:
        print("No slots to create."); paks.close(); return

    # 6. Assign record ids + prepare structural files
    use_static = args.static
    static_dir = os.path.join(SCRIPT_DIR, "out", "static")

    if use_static:
        # ---- Static mode: read pre-built files, look up ids from metadata ----
        print("Loading static structural files...")
        meta_path = os.path.join(static_dir, "static_meta.json")
        with open(meta_path, "r", encoding="utf-8") as f:
            static_meta = json.load(f)
        # Enforce slot range constraint
        slot_lo, slot_hi = static_meta["slot_range"]
        for sl in slots:
            if sl.new_costume_no < slot_lo or sl.new_costume_no >= slot_hi:
                print(f"  ERROR: slot {sl.new_costume_no} outside static range "
                      f"[{slot_lo},{slot_hi})")
                sys.exit(1)
        # Look up record ids from static metadata
        rec_lookup = {(r["fighter"], r["costume_no"]): r
                      for r in static_meta["records"]}
        fighter_outfit_counter = defaultdict(int)
        for i, sl in enumerate(slots):
            key = (sl.fighter, sl.new_costume_no)
            rec = rec_lookup.get(key)
            if not rec:
                print(f"  ERROR: no static record for fighter {sl.fighter} "
                      f"costume {sl.new_costume_no}"); sys.exit(1)
            sl.record_id = rec["record_id"]; sl.manage_id = rec["manage_id"]
            sl.sort_no = 0; sl.visual_no = sl.new_costume_no
            idx = fighter_outfit_counter[sl.fighter]
            fighter_outfit_counter[sl.fighter] += 1
            sl.message_guid = static_meta["msg_guids"][idx] if idx < len(static_meta["msg_guids"]) else ""
            sl.message_text = f"Outfit {ROMAN[idx]}" if idx < len(ROMAN) else f"Outfit {idx+1}"
            print(f"  slot {i}: {sl.fighter_dir}/v{sl.new_costume_no:02d} -> "
                  f"folder {sl.new_folder}, record {sl.record_id}, "
                  f"\"{sl.message_text}\" [static]")
        # Read pre-built files using the files list in metadata
        static_files = {}  # pak_path -> bytes
        for entry in static_meta.get("files", []):
            with open(os.path.join(static_dir, entry["local"]), "rb") as f:
                static_files[entry["pak_path"]] = f.read()
            print(f"  {entry['local']} ({len(static_files[entry['pak_path']])}B)")
        cl_patched = None  # no color table in static mode

    else:
        # ---- Dynamic mode: build structural files at runtime ----
        print("Preparing structural data...")
        _init_reasy()
        ct_data = paks.read_data(filepath_hash(COSTUME_TABLE))
        id_map, max_rid, max_mid, msg_tid, max_msg_id = parse_costume_ids(ct_data)

        esf_data = paks.read_data(filepath_hash(ESF_ROOT_SCENE))
        rsz_tmp = _parse_rsz(esf_data, "esf.scn.20")
        base_fighter_dirs = set()
        for fi in rsz_tmp.folder_infos:
            iid = rsz_tmp.object_table[fi.id]
            fields = rsz_tmp.parsed_elements.get(iid, {})
            nf = fields.get("Name")
            if nf:
                name = (nf.value if hasattr(nf,"value") else "").rstrip("\x00")
                m = re.match(r"(esf\d{3})v\d{2}$", name)
                if m: base_fighter_dirs.add(m.group(1))
        del rsz_tmp
        print(f"  {len(base_fighter_dirs)} base fighters in esf.scn.20")

        fighter_outfit_counter = defaultdict(int)
        for i, sl in enumerate(slots):
            sl.record_id = max_rid + 1 + i; sl.manage_id = max_mid + 1 + i
            sl.sort_no = 100 + sl.new_costume_no + 1; sl.visual_no = sl.new_costume_no
            sl.message_guid = str(uuid.uuid4())
            idx = fighter_outfit_counter[sl.fighter]; fighter_outfit_counter[sl.fighter] += 1
            sl.message_text = f"Outfit {ROMAN[idx]}" if idx < len(ROMAN) else f"Outfit {idx+1}"
            print(f"  slot {i}: {sl.fighter_dir}/v{sl.new_costume_no:02d} -> folder {sl.new_folder}, "
                  f"record {sl.record_id}, \"{sl.message_text}\"")

        print("\nBuilding structural files...")
        esf_patched = build_esf_scene(esf_data, slots, base_fighter_dirs)
        ct_patched = build_costume_table(ct_data, slots)
        msg_data = paks.read_data(filepath_hash(COSTUME_MSG))
        msg_patched = build_costume_msg(msg_data, slots)
        cl_data = paks.read_data(filepath_hash(COLOR_TABLE))
        cl_patched = build_color_table(cl_data, slots)

    # 8. Assemble output pak
    scope = args.patch_scope
    print(f"\nAssembling output pak (scope={scope})...")
    writer = PakWriter()
    weather_mode = args.weather_chain
    for sl in slots:
        sl._weather_chain = weather_mode
        add_slot_files(writer, sl, inv, paks, scope, all_mod_files)

    # 8b. In full mode, close dangling references created by global patching
    if scope == "full":
        print("\n  Closing references...")
        close_references(writer, slots, inv, paks, all_mod_files)

    if use_static:
        for pak_path, data in static_files.items():
            writer.add_compressed(pak_path, data, attrib=_attrib_for(pak_path))
    else:
        writer.add_compressed(ESF_ROOT_SCENE, esf_patched)
        writer.add_compressed(COSTUME_TABLE, ct_patched)
        writer.add_compressed(COSTUME_MSG, msg_patched, attrib=0x1002)
        if cl_patched:
            writer.add_compressed(COLOR_TABLE, cl_patched)
    add_restorations(writer, slots, inv, paks, all_mod_files)
    writer.add_compressed(MARKER_PATH, b"SF6_CostumeSlots v1\n")

    # 9. Determine output path and numbering
    if args.install or (not args.mod_dir):
        out_dir = args.pak_dir or game_dir
    else:
        out_dir = os.path.join(SCRIPT_DIR, "out")

    our_paks_out, mod_paks_out = classify_patch_paks(out_dir, args.ignore)
    mod_nums = [n for n, _ in mod_paks_out]
    target_num = max(mod_nums, default=0) + 1

    # Remove old our-paks at wrong numbers
    for num, old_path in our_paks_out:
        if num != target_num:
            print(f"  removing stale output pak: {old_path}")
            os.remove(old_path)

    out_path = os.path.join(out_dir, f"re_chunk_000.pak.patch_{target_num:03d}.pak")
    writer.write(out_path)

    # 10. Save registry + colors
    # In static mode, add possession info for Lua
    if use_static:
        registry["possession"] = [
            {"fighter": sl.fighter, "fighter_dir": sl.fighter_dir,
             "costume_no": sl.new_costume_no, "record_id": sl.record_id,
             "manage_id": sl.manage_id, "name": sl.message_text}
            for sl in slots
        ]
    save_registry(registry_path, registry)
    print(f"  registry saved to {registry_path}")
    if not use_static:
        colors_dir = os.path.join(reg_dir, "..", "SF6_CostumeSlots_data")
        colors_dir = os.path.normpath(colors_dir)
        os.makedirs(colors_dir, exist_ok=True)
        colors_path = os.path.join(colors_dir, "colors.json")
        with open(colors_path, "w", encoding="utf-8") as f: f.write(build_colors_json(slots))
        print(f"  colors.json saved to {colors_path}")

    elapsed = time.time() - t0
    print(f"\nDone in {elapsed:.1f}s")

    # 11. Verify
    if args.verify:
        ok = verify_output(out_path, writer.expected_paths, paks, slots, inv, scope)
        print("\nVERIFICATION:", "PASS" if ok else "ISSUES FOUND (see above)")

    paks.close()
    for mp in mod_pak_indexes: mp.close()


if __name__ == "__main__":
    main()
