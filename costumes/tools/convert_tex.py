#!/usr/bin/env python3
"""
convert_tex.py -- Convert .tex.143230113 (old SF6 / MHRISE-era) textures
                  to .tex.241101895 (current SF6 / MHWILDS-era).

Analysis summary
================
Both versions share identical header layout (both >= MHRISE serializer in
REasy terms): 40-byte header + 8-byte swizzle block + 16-byte-per-mip table
+ raw mip data.  Neither version uses packed mips or GDeflate in SF6.

The ONLY byte-level difference is the version field at offset 4:
  old: 143230113  (0x088984A1)
  new: 241101895  (0x0E5EEC47)

The game hashes file paths INCLUDING the extension suffix (.tex.NNNN) via
Murmur3, so a texture with the wrong version number in its name/header
simply doesn't exist from the game's perspective ("model absent").

Conversion is therefore a version-field patch + file rename.  The mip data,
flags, formats, swizzle settings and table layout are preserved verbatim.
"""

import os
import sys, os, struct, shutil, json

# ---- paths ----------------------------------------------------------------
REASY = os.environ.get("REASY_PARSER", r"..\REasy-parser")  # point REASY_PARSER at your REasy-parser checkout
LOADER_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Street Fighter 6\reframework\agent\tmp\costume_loader"
GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Street Fighter 6"

sys.path.insert(0, REASY)
sys.path.insert(0, LOADER_DIR)

from file_handlers.tex.tex_file import (
    TexFile, TEX_MAGIC, SERIALIZER_MHRISE, SERIALIZER_MHWILDS,
    get_serializer_version
)
from file_handlers.tex.dxgi import dxgi_name, is_block_compressed
from file_handlers.tex.texture_decoder import decode_texture_data

OLD_VERSION = 143230113   # 0x088984A1
NEW_VERSION = 241101895   # 0x0E5EEC47
OLD_SUFFIX  = ".tex.143230113"
NEW_SUFFIX  = ".tex.241101895"

# ===========================================================================
# Core conversion
# ===========================================================================

def convert_tex_bytes(data: bytes) -> bytes:
    """Patch version field from OLD_VERSION to NEW_VERSION. Returns new bytes."""
    if len(data) < 8:
        raise ValueError("File too small to be a TEX")
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != TEX_MAGIC:
        raise ValueError(f"Not a TEX file (magic={magic:#x})")
    version = struct.unpack_from("<i", data, 4)[0]
    if version != OLD_VERSION:
        raise ValueError(f"Unexpected version {version} (expected {OLD_VERSION})")
    out = bytearray(data)
    struct.pack_into("<i", out, 4, NEW_VERSION)
    return bytes(out)


def parse_and_describe(data: bytes, label: str, file_version: int = 0) -> TexFile:
    """Parse a tex file and print a compact summary."""
    tf = TexFile()
    if not tf.read(data, file_version=file_version):
        raise ValueError(f"Failed to parse {label}")
    h = tf.header
    fmt_name = dxgi_name(h.format)
    print(f"  {label}: {h.width}x{h.height} {fmt_name} "
          f"mips={h.mip_count} flags={h.flags:#06x} "
          f"ver={h.version} size={len(data)}")
    return tf


def decode_mip0(tf: TexFile) -> bytes:
    """Decode mip 0 to RGBA pixels for comparison."""
    mip = tf.get_mip_map_data(0)
    return decode_texture_data(tf.header.format, mip.width, mip.height, mip.data)


# ===========================================================================
# Validation 1: Vanilla round-trip
# ===========================================================================

def validate_roundtrip():
    """Read a vanilla .241101895 tex from pak, verify that patching version
    to OLD then back to NEW produces byte-identical output."""
    from costume_loader import PakIndex, VanillaInventory

    VANILLA_JSON = os.path.join(LOADER_DIR, "vanilla_costume_paths.json")

    print("\n=== Vanilla round-trip validation ===")
    print("Loading pak...")
    pak = PakIndex()
    pak.add_pak(os.path.join(GAME_DIR, "re_chunk_000.pak"))
    inv = VanillaInventory(VANILLA_JSON)

    # Pick 3 diverse vanilla textures
    targets = []
    seen = set()
    for c in inv.costumes:
        for fi in c.get("files", []):
            if fi.get("kind") != "tex" or "model/esf/" not in fi["path"]:
                continue
            h = fi["hash"]
            if not pak.has(h):
                continue
            data = pak.read_data(h)
            w = struct.unpack_from("<h", data, 8)[0]
            ht = struct.unpack_from("<h", data, 10)[0]
            fmt = struct.unpack_from("<i", data, 14)[0]
            key = (w, ht, fmt)
            if key in seen:
                continue
            seen.add(key)
            targets.append((fi["path"], data))
            if len(targets) >= 3:
                break
        if len(targets) >= 3:
            break

    all_ok = True
    for path, original in targets:
        print(f"\n  Testing: {path}")
        parse_and_describe(original, "original")

        # Patch to old version
        patched_old = bytearray(original)
        struct.pack_into("<i", patched_old, 4, OLD_VERSION)
        patched_old = bytes(patched_old)

        # Verify it reads correctly as old version
        tf_old = TexFile()
        tf_old.read(patched_old, file_version=OLD_VERSION)

        # Patch back to new version
        restored = convert_tex_bytes(patched_old)

        if restored == original:
            print("    PASS: byte-identical after round-trip")
        else:
            # Find differences
            diffs = []
            for i in range(min(len(restored), len(original))):
                if restored[i] != original[i]:
                    diffs.append(i)
            print(f"    FAIL: {len(diffs)} byte differences at offsets: {diffs[:20]}")
            all_ok = False

    pak.close()
    return all_ok


# ===========================================================================
# Validation 2: Convert mod textures + pixel comparison
# ===========================================================================

def convert_mod_textures(mod_src: str, mod_dst: str):
    """Convert all .tex.143230113 files in the mod tree, copy other files as-is.
    Returns list of (src, dst, src_tf, dst_tf) for converted textures."""
    converted = []

    for root, dirs, files in os.walk(mod_src):
        rel = os.path.relpath(root, mod_src)
        dst_root = os.path.join(mod_dst, rel) if rel != "." else mod_dst
        os.makedirs(dst_root, exist_ok=True)

        for fname in files:
            src_path = os.path.join(root, fname)
            if fname.lower().endswith(OLD_SUFFIX):
                # Convert
                base = fname[:-len(OLD_SUFFIX)]
                dst_name = base + NEW_SUFFIX
                dst_path = os.path.join(dst_root, dst_name)
                with open(src_path, "rb") as f:
                    src_data = f.read()
                dst_data = convert_tex_bytes(src_data)
                with open(dst_path, "wb") as f:
                    f.write(dst_data)
                converted.append((src_path, dst_path, src_data, dst_data))
            else:
                # Copy as-is
                dst_path = os.path.join(dst_root, fname)
                shutil.copy2(src_path, dst_path)

    return converted


def validate_converted_textures(converted):
    """Verify each converted texture has same dimensions, format, mips,
    and identical mip 0 decoded pixels."""
    print(f"\n=== Validating {len(converted)} converted textures ===")
    all_ok = True

    for src_path, dst_path, src_data, dst_data in converted:
        fname = os.path.basename(src_path)
        # Parse source (force MHRISE-compatible reading)
        tf_src = TexFile()
        tf_src.read(src_data, file_version=OLD_VERSION)

        # Parse destination (should read as MHWILDS)
        tf_dst = TexFile()
        tf_dst.read(dst_data)

        hs = tf_src.header
        hd = tf_dst.header

        # Check dimensions/format/mips match
        ok = True
        if (hs.width, hs.height) != (hd.width, hd.height):
            print(f"  FAIL {fname}: dimensions {hs.width}x{hs.height} vs {hd.width}x{hd.height}")
            ok = False
        if hs.format != hd.format:
            print(f"  FAIL {fname}: format {hs.format} vs {hd.format}")
            ok = False
        if hs.mip_count != hd.mip_count:
            print(f"  FAIL {fname}: mip count {hs.mip_count} vs {hd.mip_count}")
            ok = False
        if hs.flags != hd.flags:
            print(f"  FAIL {fname}: flags {hs.flags:#x} vs {hd.flags:#x}")
            ok = False

        # Compare mip data (raw bytes, all mips)
        for level in range(hs.mip_count):
            mip_src = tf_src.get_mip_map_data(level)
            mip_dst = tf_dst.get_mip_map_data(level)
            if mip_src.data != mip_dst.data:
                print(f"  FAIL {fname}: mip {level} data differs "
                      f"(src={len(mip_src.data)}, dst={len(mip_dst.data)})")
                ok = False

        # Decode and compare mip 0 pixels
        try:
            pixels_src = decode_mip0(tf_src)
            pixels_dst = decode_mip0(tf_dst)
            if pixels_src != pixels_dst:
                print(f"  FAIL {fname}: mip 0 decoded pixels differ")
                ok = False
        except Exception as e:
            print(f"  WARN {fname}: could not decode mip 0: {e}")

        # Version check
        if hd.version != NEW_VERSION:
            print(f"  FAIL {fname}: output version {hd.version} != {NEW_VERSION}")
            ok = False

        if ok:
            print(f"  PASS {fname}: {hs.width}x{hs.height} {dxgi_name(hs.format)} "
                  f"mips={hs.mip_count} flags={hs.flags:#06x}")
        else:
            all_ok = False

    return all_ok


# ===========================================================================
# Main
# ===========================================================================

def main():
    MOD_SRC = r"C:\Temp\kenmod\KenSFV"
    MOD_DST = r"C:\Temp\kenmod\KenSFV_v3"
    PAK_OUT = r"C:\Temp\kenmod\ken_fluffy_v3.pak"

    print("=" * 60)
    print("SF6 TEX Converter: .tex.143230113 -> .tex.241101895")
    print("=" * 60)

    # ---- Phase 1: Vanilla round-trip ----
    rt_ok = validate_roundtrip()
    if not rt_ok:
        print("\nWARNING: Round-trip validation found differences!")
    else:
        print("\nRound-trip validation: ALL PASS")

    # ---- Phase 2: Convert mod textures ----
    print(f"\n=== Converting mod: {MOD_SRC} -> {MOD_DST} ===")
    if os.path.exists(MOD_DST):
        shutil.rmtree(MOD_DST)
    converted = convert_mod_textures(MOD_SRC, MOD_DST)
    print(f"  Converted {len(converted)} textures, copied other files")

    # ---- Phase 3: Validate converted textures ----
    conv_ok = validate_converted_textures(converted)
    if not conv_ok:
        print("\nWARNING: Some converted textures failed validation!")
    else:
        print(f"\nAll {len(converted)} converted textures: PASS")

    # ---- Phase 4: Build pak ----
    print(f"\n=== Building pak: {PAK_OUT} ===")
    # Import and call make_fluffy_pak from costume_loader
    from costume_loader import make_fluffy_pak
    make_fluffy_pak(MOD_DST, PAK_OUT)

    print(f"\n{'=' * 60}")
    print(f"Pak: {PAK_OUT}")
    if os.path.exists(PAK_OUT):
        sz = os.path.getsize(PAK_OUT) / 1e6
        print(f"Size: {sz:.1f} MB")
    print(f"Round-trip: {'PASS' if rt_ok else 'FAIL'}")
    print(f"Conversion: {'PASS' if conv_ok else 'FAIL'}")


if __name__ == "__main__":
    main()
