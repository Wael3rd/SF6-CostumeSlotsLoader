#!/usr/bin/env python3
"""
make_static_structural.py -- Generate static structural files for the costume
slot loader.  Built ONCE, shipped as-is; no RSZ parser needed at runtime.

Produces out/static/:
  esf.scn.20                         -- 1800 via.Folder for 18 base fighters
  ver_02_0300_esf.scn.20             -- 1200 via.Folder for 12 DLC fighters
  ver_02_0400_esf.scn.20             -- 100  via.Folder for esf033 (Yasmine)
  fightercostumeuserdata.user.2      -- 3100 records (31 fighters x 100 slots)
  fightercostumemessage.msg.21       -- 100 messages ("Outfit I" .. "Outfit C")
  static_meta.json                   -- id map + file list for C++ loader
"""

import os
import sys, os, copy, json, re as _re

REASY = os.environ.get("REASY_PARSER", r"..\REasy-parser")  # point REASY_PARSER at your REasy-parser checkout
sys.path.insert(0, REASY)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from costume_loader import (PakIndex, filepath_hash, GAME_DIR_DEFAULT,
                             ESF_ROOT_SCENE, COSTUME_TABLE, COSTUME_MSG)

SLOT_RANGE = range(5, 105)
RECORD_ID_BASE = 5000
MSG_ID_BASE = 5000
SORT_NO_BASE = 101
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "static")

def _int_to_roman(n):
    """Convert integer 1..100 to Roman numeral."""
    vals = [(100,'C'),(90,'XC'),(50,'L'),(40,'XL'),(10,'X'),(9,'IX'),(5,'V'),(4,'IV'),(1,'I')]
    result = ''
    for v, s in vals:
        while n >= v:
            result += s; n -= v
    return result

ROMAN = [_int_to_roman(i) for i in range(1, 101)]  # I..C
MSG_GUIDS = [f"a1b2c3d4-{i+1:04x}-4000-8000-{5000+i:012x}" for i in range(100)]
ALL_FIGHTER_IDS = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,
                   19,20,21,22,25,26,27,28,29,30,31,32,33]

VER_0300_SCENE = "natives/stm/product/flow/version_update/ver_02_0300/esf.scn.20"
VER_0400_SCENE = "natives/stm/product/flow/version_update/ver_02_0400/esf.scn.20"


def add_folders_to_scene(rsz, data, label, dlc_fighter_dirs, slot_range):
    """Add via.Folder entries to an RSZ scene for the specified fighters.
    Uses the existing v04 folder as template for each fighter.
    Returns (patched_bytes, added_count)."""
    from file_handlers.rsz.rsz_file import RszFolderInfo, RszInstanceInfo

    assert bytes(rsz.build()) == data, f"round-trip failed for {label}"

    # Build map: fighter_dir -> (parent_folder_id, template_inst_id)
    # In ver scenes, the parent is a 'fighterXXX' folder at the root,
    # and costume folders (esfXXXv04) are children.
    templates = {}  # fighter_dir -> (parent_id, inst_id)
    for fi in rsz.folder_infos:
        iid = rsz.object_table[fi.id]
        fields = rsz.parsed_elements.get(iid, {})
        nf = fields.get("Name")
        if not nf: continue
        name = (nf.value if hasattr(nf, "value") else str(nf)).rstrip("\x00")
        m = _re.match(r"(esf\d{3})v04$", name)
        if m:
            fd = m.group(1)
            if fd in dlc_fighter_dirs:
                templates[fd] = (fi.parent_id, iid)

    added = 0
    for fd in sorted(dlc_fighter_dirs):
        if fd not in templates:
            print(f"  WARN: no v04 template for {fd} in {label}")
            continue
        parent_id, tmpl_iid = templates[fd]
        src_inst = rsz.instance_infos[tmpl_iid]
        for cno in slot_range:
            new_ii = RszInstanceInfo()
            new_ii.type_id = src_inst.type_id; new_ii.crc = src_inst.crc
            new_iid = len(rsz.instance_infos); rsz.instance_infos.append(new_ii)
            nf = {k: copy.deepcopy(v) for k, v in rsz.parsed_elements[tmpl_iid].items()}
            sfx = "\x00" if nf["Name"].value.endswith("\x00") else ""
            scene_name = f"{fd}v{cno:02d}"
            nf["Name"].value = scene_name + sfx
            sp = f"Product/CharParam/esf/{fd}/{scene_name}.scn"
            sp_sfx = "\x00" if nf["ScenePath"].value.endswith("\x00") else ""
            nf["ScenePath"].value = sp + sp_sfx
            rsz.parsed_elements[new_iid] = nf
            new_obj = len(rsz.object_table); rsz.object_table.append(new_iid)
            new_fi = RszFolderInfo(); new_fi.id = new_obj
            new_fi.parent_id = parent_id
            rsz.folder_infos.append(new_fi)
            added += 1

    result = bytes(rsz.build())
    print(f"  {label}: {len(data)}B -> {len(result)}B, {added} folders added")
    return result, added


def main():
    from utils.type_registry import TypeRegistry
    from file_handlers.rsz.rsz_file import RszFile, RszFolderInfo, RszInstanceInfo
    from file_handlers.rsz.rsz_data_types import ObjectData, U32Data, GuidData
    from file_handlers.msg.msg_handler import MsgHandler

    reg_path = os.path.join(REASY, "resources", "data", "dumps", "rszsf6.json")
    type_registry = TypeRegistry(reg_path)
    info, _ = type_registry.find_type_by_name("app.FighterCostumeUserDataRecord")
    for fname in ("sortNo", "visualNo", "arrangeId"):
        if fname not in {f["name"] for f in info["fields"]}:
            info["fields"].append({"align":4,"array":False,"name":fname,"native":False,
                                    "original_type":"System.UInt32","size":4,"type":"U32"})

    def parse_rsz(data, fp):
        rsz = RszFile(); rsz.filepath = fp
        rsz.type_registry = type_registry; rsz.game_version = "SF6"
        rsz.read(data, validate_type_registry=False); return rsz

    print("Loading base pak...")
    paks = PakIndex()
    paks.add_pak(os.path.join(GAME_DIR_DEFAULT, "re_chunk_000.pak"))
    for d in ("re_dlc_stm_1792750.pak", "re_dlc_stm_1792751.pak"):
        paks.add_pak(os.path.join(GAME_DIR_DEFAULT, "dlc", d))

    # ---- Determine which fighters go where ----
    esf_data = paks.read_data(filepath_hash(ESF_ROOT_SCENE))
    esf_rsz = parse_rsz(esf_data, "esf.scn.20")
    base_dirs = set()
    for fi in esf_rsz.folder_infos:
        iid = esf_rsz.object_table[fi.id]
        nf = esf_rsz.parsed_elements.get(iid, {}).get("Name")
        if nf:
            m = _re.match(r"(esf\d{3})v00$", (nf.value if hasattr(nf,"value") else "").rstrip("\x00"))
            if m: base_dirs.add(m.group(1))

    ver0300_data = paks.read_data(filepath_hash(VER_0300_SCENE))
    ver0300_rsz = parse_rsz(ver0300_data, "ver_02_0300_esf.scn.20")
    ver0300_dirs = set()
    for fi in ver0300_rsz.folder_infos:
        iid = ver0300_rsz.object_table[fi.id]
        nf = ver0300_rsz.parsed_elements.get(iid, {}).get("Name")
        if nf:
            m = _re.match(r"(esf\d{3})v\d{2}$", (nf.value if hasattr(nf,"value") else "").rstrip("\x00"))
            if m: ver0300_dirs.add(m.group(1))
    dlc_in_0300 = ver0300_dirs - base_dirs  # DLC fighters in ver_02_0300

    ver0400_data = paks.read_data(filepath_hash(VER_0400_SCENE))
    ver0400_rsz = parse_rsz(ver0400_data, "ver_02_0400_esf.scn.20")
    ver0400_dirs = set()
    for fi in ver0400_rsz.folder_infos:
        iid = ver0400_rsz.object_table[fi.id]
        nf = ver0400_rsz.parsed_elements.get(iid, {}).get("Name")
        if nf:
            m = _re.match(r"(esf\d{3})v\d{2}$", (nf.value if hasattr(nf,"value") else "").rstrip("\x00"))
            if m: ver0400_dirs.add(m.group(1))
    dlc_in_0400 = ver0400_dirs - base_dirs - dlc_in_0300

    print(f"\nBase fighters (esf.scn.20): {len(base_dirs)}")
    print(f"DLC in ver_02_0300: {sorted(dlc_in_0300)} ({len(dlc_in_0300)})")
    print(f"DLC in ver_02_0400: {sorted(dlc_in_0400)} ({len(dlc_in_0400)})")

    # ================================================================
    # 1. esf.scn.20 -- base fighters (v00 template)
    # ================================================================
    print("\n=== esf.scn.20 ===")
    # Re-parse fresh (add_folders_to_scene consumes the rsz)
    esf_rsz2 = parse_rsz(esf_data, "esf.scn.20")
    # Build templates from v00
    tmpl_base = {}
    for fi in esf_rsz2.folder_infos:
        iid = esf_rsz2.object_table[fi.id]
        nf = esf_rsz2.parsed_elements.get(iid, {}).get("Name")
        if nf:
            m = _re.match(r"(esf\d{3})v00$", (nf.value if hasattr(nf,"value") else "").rstrip("\x00"))
            if m: tmpl_base[m.group(1)] = (fi, iid)

    assert bytes(esf_rsz2.build()) == esf_data
    added_base = 0
    for fd in sorted(base_dirs):
        if fd not in tmpl_base: continue
        tmpl_fi, tmpl_iid = tmpl_base[fd]
        src = esf_rsz2.instance_infos[tmpl_iid]
        for cno in SLOT_RANGE:
            ni = RszInstanceInfo(); ni.type_id = src.type_id; ni.crc = src.crc
            new_iid = len(esf_rsz2.instance_infos); esf_rsz2.instance_infos.append(ni)
            nf = {k: copy.deepcopy(v) for k, v in esf_rsz2.parsed_elements[tmpl_iid].items()}
            sfx = "\x00" if nf["Name"].value.endswith("\x00") else ""
            sn = f"{fd}v{cno:02d}"
            nf["Name"].value = sn + sfx
            sp_sfx = "\x00" if nf["ScenePath"].value.endswith("\x00") else ""
            nf["ScenePath"].value = f"Product/CharParam/esf/{fd}/{sn}.scn" + sp_sfx
            esf_rsz2.parsed_elements[new_iid] = nf
            no = len(esf_rsz2.object_table); esf_rsz2.object_table.append(new_iid)
            nfi = RszFolderInfo(); nfi.id = no; nfi.parent_id = tmpl_fi.parent_id
            esf_rsz2.folder_infos.append(nfi); added_base += 1
    esf_out = bytes(esf_rsz2.build())
    print(f"  {len(esf_data)}B -> {len(esf_out)}B, {added_base} folders")

    # ================================================================
    # 2. ver_02_0300/esf.scn.20 -- DLC fighters (v04 template)
    # ================================================================
    print("\n=== ver_02_0300/esf.scn.20 ===")
    ver0300_rsz2 = parse_rsz(ver0300_data, "ver_02_0300_esf.scn.20")
    ver0300_out, added_0300 = add_folders_to_scene(
        ver0300_rsz2, ver0300_data, "ver_02_0300", dlc_in_0300, SLOT_RANGE)

    # ================================================================
    # 3. ver_02_0400/esf.scn.20 -- esf033 (v04 template)
    # ================================================================
    print("\n=== ver_02_0400/esf.scn.20 ===")
    ver0400_rsz2 = parse_rsz(ver0400_data, "ver_02_0400_esf.scn.20")
    ver0400_out, added_0400 = add_folders_to_scene(
        ver0400_rsz2, ver0400_data, "ver_02_0400", dlc_in_0400, SLOT_RANGE)

    # ================================================================
    # 4. Costume table
    # ================================================================
    print("\n=== fightercostumeuserdata.user.2 ===")
    ct_data = paks.read_data(filepath_hash(COSTUME_TABLE))
    ct = parse_rsz(ct_data, "fightercostumeuserdata.user.2")
    assert bytes(ct.build()) == ct_data
    root = ct.object_table[0]; da = ct.parsed_elements[root]["DataArray"]
    tmpl_idx = None
    for v in da.values:
        f = ct.parsed_elements[v.value]
        if f["fighterId"].value == 1 and f["costumeNo"].value == 0:
            tmpl_idx = v.value; break
    _, msg_tid = type_registry.find_type_by_name(
        "app.FighterCostumeUserDataRecord.FighterCostumeMessage")
    msg_tmpl_idx = None
    for idx, inst in enumerate(ct.instance_infos):
        if inst.type_id == msg_tid: msg_tmpl_idx = idx; break
    tmpl_msg_fields = ct.parsed_elements[msg_tmpl_idx]
    msg_instances = []
    msg_src_ii = ct.instance_infos[msg_tmpl_idx]
    for so in range(len(SLOT_RANGE)):
        mi = RszInstanceInfo(); mi.type_id = msg_src_ii.type_id; mi.crc = msg_src_ii.crc
        mix = len(ct.instance_infos); ct.instance_infos.append(mi)
        ct.parsed_elements[mix] = {
            "id": U32Data(MSG_ID_BASE + so, tmpl_msg_fields["id"].orig_type),
            "GUID": GuidData(MSG_GUIDS[so], None, tmpl_msg_fields["GUID"].orig_type),
        }
        msg_instances.append(mix)
    added_records = 0; sort_ctr = SORT_NO_BASE
    fighter_indices = {fid: i for i, fid in enumerate(ALL_FIGHTER_IDS)}
    for fid in ALL_FIGHTER_IDS:
        fi_idx = fighter_indices[fid]
        for so, cno in enumerate(SLOT_RANGE):
            rid = RECORD_ID_BASE + fi_idx * len(SLOT_RANGE) + so
            src_ii = ct.instance_infos[tmpl_idx]
            ni = RszInstanceInfo(); ni.type_id = src_ii.type_id; ni.crc = src_ii.crc
            nx = len(ct.instance_infos); ct.instance_infos.append(ni)
            nf = {k: copy.deepcopy(v) for k, v in ct.parsed_elements[tmpl_idx].items()}
            nf["id"].value = rid; nf["ManageId"].value = rid
            nf["fighterId"].value = fid; nf["costumeNo"].value = cno
            nf["isDefault"].value = False
            nf["isShowName"].value = ct.parsed_elements[tmpl_idx]["isShowName"].value
            nf["sortNo"].value = sort_ctr; nf["visualNo"].value = cno; nf["arrangeId"].value = 0
            nf["messageId"] = ObjectData(msg_instances[so],
                "app.FighterCostumeUserDataRecord.FighterCostumeMessage")
            ct.parsed_elements[nx] = nf
            da.values.append(ObjectData(nx, "app.FighterCostumeUserDataRecord"))
            sort_ctr += 1; added_records += 1
    ct_out = bytes(ct.build())
    print(f"  {len(ct_data)}B -> {len(ct_out)}B, {added_records} records")

    # ================================================================
    # 5. Messages
    # ================================================================
    print("\n=== fightercostumemessage.msg.21 ===")
    msg_data = paks.read_data(filepath_hash(COSTUME_MSG))
    h = MsgHandler(); h.read(msg_data)
    for so in range(len(SLOT_RANGE)):
        h.add_entry(uuid_str=MSG_GUIDS[so], name=f"FighterCostumeMessage{6+so}",
                     contents=[f"Outfit {ROMAN[so]}"] * len(h.useLanguages))
    msg_out = bytes(h.rebuild())
    print(f"  {len(msg_data)}B -> {len(msg_out)}B, {len(SLOT_RANGE)} messages")

    # ================================================================
    # Write
    # ================================================================
    os.makedirs(OUT_DIR, exist_ok=True)
    output_files = [
        ("esf.scn.20", ESF_ROOT_SCENE, esf_out),
        ("ver_02_0300_esf.scn.20", VER_0300_SCENE, ver0300_out),
        ("ver_02_0400_esf.scn.20", VER_0400_SCENE, ver0400_out),
        ("fightercostumeuserdata.user.2", COSTUME_TABLE, ct_out),
        ("fightercostumemessage.msg.21", COSTUME_MSG, msg_out),
    ]
    files_meta = []
    for local_name, pak_path, data in output_files:
        path = os.path.join(OUT_DIR, local_name)
        with open(path, "wb") as f: f.write(data)
        print(f"  wrote {local_name} ({len(data)}B)")
        files_meta.append({"local": local_name, "pak_path": pak_path})

    # Metadata
    all_base_and_dlc = sorted(base_dirs | dlc_in_0300 | dlc_in_0400)
    meta = {
        "slot_range": [SLOT_RANGE.start, SLOT_RANGE.stop],
        "record_id_base": RECORD_ID_BASE,
        "msg_id_base": MSG_ID_BASE,
        "sort_no_base": SORT_NO_BASE,
        "msg_guids": MSG_GUIDS,
        "fighter_ids": ALL_FIGHTER_IDS,
        "base_fighter_dirs": sorted(base_dirs),
        "dlc_fighter_dirs_0300": sorted(dlc_in_0300),
        "dlc_fighter_dirs_0400": sorted(dlc_in_0400),
        "files": files_meta,
        "records": [
            {"fighter": fid, "costume_no": cno,
             "record_id": RECORD_ID_BASE + fighter_indices[fid]*len(SLOT_RANGE) + so,
             "manage_id": RECORD_ID_BASE + fighter_indices[fid]*len(SLOT_RANGE) + so}
            for fid in ALL_FIGHTER_IDS for so, cno in enumerate(SLOT_RANGE)
        ],
    }
    with open(os.path.join(OUT_DIR, "static_meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)
    print(f"  wrote static_meta.json")

    paks.close()
    total_folders = added_base + added_0300 + added_0400
    print(f"\nDone. {total_folders} folders, {added_records} records, "
          f"{len(SLOT_RANGE)} messages.")


if __name__ == "__main__":
    main()
