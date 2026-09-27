// loader_core.cpp -- SF6 Costume Slot Loader core logic
// Shared by costume_loader.exe and amd_ags_x64.dll (proxy).

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "pak.hpp"
#include "patch.hpp"
#include "loader_core.hpp"
#include "archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <memory>
#include <functional>

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

// ============================================================================
// Logging
// ============================================================================

extern bool g_is_exe;

static FILE* g_log = nullptr;

static void log_open(const std::string& game_dir) {
    std::string path = game_dir + "\\SF6_CostumeLoader.log";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) {
        uint64_t sz = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        if (sz > 1024 * 1024) DeleteFileA(path.c_str());
    }
    g_log = fopen(path.c_str(), "a");
}

static void log_close() { if (g_log) { fclose(g_log); g_log = nullptr; } }

static void logf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[2048]; vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char ts[32]; sprintf(ts, "%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    if (g_log) { fprintf(g_log, "[%s] %s", ts, buf); fflush(g_log); }
    if (g_is_exe) printf("%s", buf);
}

static std::string w2a(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_ACP, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ============================================================================
// Constants
// ============================================================================

static const char* MARKER_PATH  = "natives/stm/sf6_costume_slots.marker";
static const char* BASE_PFX     = "natives/stm/product/";
static const char* STREAM_PFX   = "natives/stm/streaming/product/";
static const char* ESF_ROOT     = "natives/stm/product/charparam/esf/esf.scn.20";
static const char* COSTUME_TBL  = "natives/stm/product/cfncontents/onlineshop/fightercostumeuserdata.user.2";
static const char* COSTUME_MSG  = "natives/stm/product/message/fgm/fighter/fightercostumemessage.msg.21";
static std::string int_to_roman(int n) {
    static const int vals[] =  {1000,900,500,400,100,90,50,40,10,9,5,4,1};
    static const char* syms[] = {"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};
    std::string s;
    for (int i = 0; i < 13; i++)
        while (n >= vals[i]) { s += syms[i]; n -= vals[i]; }
    return s;
}
static const char* OLD_TEX_SUFFIXES[] = { "143230113" }; // known old tex versions
static const int   N_OLD_TEX = 1;

// Part of the fingerprint: change it whenever the loader starts producing a different pak, so
// that installing a new loader regenerates the pak once even if no mod changed.
static const char* LOADER_BUILD_ID = "2026-09-27-texmips-safewrite";

// ============================================================================
// Utility
// ============================================================================

static std::string str_lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool str_ends_with(const std::string& s, const char* suf) {
    size_t sl = strlen(suf);
    return s.size() >= sl && s.compare(s.size() - sl, sl, suf) == 0;
}

static bool needs_content_patch(const std::string& p) {
    auto l = str_lower(p);
    return str_ends_with(l,".mdf2.31") || str_ends_with(l,".user.2")
        || str_ends_with(l,".chain.52") || str_ends_with(l,".scn.20");
}

// Replace printf with logf throughout
#define printf logf

static std::string read_file_text(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string s(sz, 0);
    fread(s.data(), 1, sz, f);
    fclose(f);
    return s;
}

static std::vector<uint8_t> read_file_bin(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v(sz);
    fread(v.data(), 1, sz, f);
    fclose(f);
    return v;
}

// ============================================================================
// MD5 via Windows CNG
// ============================================================================

static std::string md5_hex(const void* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0);
    UCHAR hash[16];
    BCryptFinishHash(hHash, hash, 16, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + i*2, "%02x", hash[i]);
    hex[32] = 0;
    return hex;
}

// ============================================================================
// Vanilla Index (parsed from TSV)
// ============================================================================

struct CostumeInfo {
    int fighter;
    std::string fighter_dir;
    int costume_no;
    std::string model_dir;
    std::string scene;
};

struct VanillaIndex {
    std::unordered_map<std::string, std::string> suffixes; // ext -> version
    std::vector<CostumeInfo> costumes;
    std::unordered_map<uint64_t, std::vector<std::string>> hash_to_paths;
    std::unordered_set<uint64_t> all_hashes;
    // folder_to_costume: "esf001\t001" -> index into costumes
    std::unordered_map<std::string, size_t> folder_to_costume;

    bool load(const char* tsv_path) {
        FILE* f = fopen(tsv_path, "r");
        if (!f) return false;
        int section = 0; // 0=none, 1=suffixes, 2=costumes, 3=files
        char line[2048];
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
            if (len == 0) continue;
            if (line[0] == '#') {
                if (strncmp(line, "#SUFFIXES", 9) == 0) section = 1;
                else if (strncmp(line, "#COSTUMES", 9) == 0) section = 2;
                else if (strncmp(line, "#FILES", 6) == 0) section = 3;
                continue;
            }
            if (section == 1) {
                char ext[64], ver[64];
                if (sscanf(line, "%63[^\t]\t%63s", ext, ver) == 2)
                    suffixes[ext] = ver;
            } else if (section == 2) {
                CostumeInfo ci;
                char fdir[32], mdir[256], scn[256];
                mdir[0] = scn[0] = 0;
                if (sscanf(line, "%d\t%31[^\t]\t%d\t%255[^\t]\t%255s",
                           &ci.fighter, fdir, &ci.costume_no, mdir, scn) >= 3) {
                    ci.fighter_dir = fdir;
                    ci.model_dir = mdir;
                    ci.scene = scn;
                    costumes.push_back(ci);
                    if (ci.model_dir.size() > 0) {
                        auto slash = ci.model_dir.rfind('/');
                        if (slash != std::string::npos) {
                            std::string folder = ci.model_dir.substr(slash+1);
                            std::string key = ci.fighter_dir + "\t" + folder;
                            // A model folder can serve two costumes (Dhalsim: C1 and DriveTech both
                            // use 001). A mod of that folder is a mod of the lowest costume number;
                            // keeping the last one made every Dhalsim C1 mod load as DriveTech.
                            auto prev = folder_to_costume.find(key);
                            if (prev == folder_to_costume.end()
                                || ci.costume_no < costumes[prev->second].costume_no)
                                folder_to_costume[key] = costumes.size() - 1;
                        }
                    }
                }
            } else if (section == 3) {
                char hash_s[32], kind[32], path[512];
                int fighter, cno;
                if (sscanf(line, "%31[^\t]\t%d\t%d\t%31[^\t]\t%511s",
                           hash_s, &fighter, &cno, kind, path) == 5) {
                    uint64_t h = strtoull(hash_s, nullptr, 16);
                    hash_to_paths[h].push_back(path);
                    all_hashes.insert(h);
                }
            }
        }
        fclose(f);
        return true;
    }

    int max_costume_no(int fighter) const {
        int mx = -1;
        for (auto& c : costumes)
            if (c.fighter == fighter && c.costume_no < 100)
                mx = std::max(mx, c.costume_no);
        return mx;
    }

    int max_folder(int fighter) const {
        int mx = 0;
        for (auto& c : costumes) {
            if (c.fighter != fighter || c.model_dir.empty()) continue;
            auto s = c.model_dir.rfind('/');
            if (s != std::string::npos) {
                int v = atoi(c.model_dir.c_str() + s + 1);
                mx = std::max(mx, v);
            }
        }
        return mx;
    }

    const CostumeInfo* get_costume(int fighter, int cno) const {
        for (auto& c : costumes)
            if (c.fighter == fighter && c.costume_no == cno) return &c;
        return nullptr;
    }

    std::string model_prefix(const std::string& fd, const std::string& folder) const {
        return "natives/stm/product/model/esf/" + fd + "/" + folder + "/";
    }
};

// ============================================================================
// Registry (JSON parse/write for fixed schema)
// ============================================================================

struct RegSlot {
    std::string mod_id, fighter_dir, original_folder, new_folder, scene_name;
    int fighter=0, original_costume_no=0, new_costume_no=0;
};

struct Possession {
    int fighter=0, costume_no=0, record_id=0, manage_id=0;
    std::string fighter_dir, name;
};

struct Registry {
    int version = 1;
    std::string fingerprint;
    std::vector<RegSlot> slots;
    std::vector<Possession> possession;
};

// Minimal JSON string extractor
static std::string json_str(const std::string& line, const char* key) {
    auto kp = line.find(std::string("\"") + key + "\"");
    if (kp == std::string::npos) return {};
    auto colon = line.find(':', kp);
    if (colon == std::string::npos) return {};
    auto q1 = line.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    auto q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return line.substr(q1+1, q2-q1-1);
}

static int json_int(const std::string& line, const char* key) {
    auto kp = line.find(std::string("\"") + key + "\"");
    if (kp == std::string::npos) return 0;
    auto colon = line.find(':', kp);
    if (colon == std::string::npos) return 0;
    return atoi(line.c_str() + colon + 1);
}

static Registry load_registry(const char* path) {
    Registry reg;
    auto txt = read_file_text(path);
    if (txt.empty()) return reg;

    // Parse line by line
    // Parse fingerprint (top-level string)
    {
        auto fp = txt.find("\"fingerprint\"");
        if (fp != std::string::npos) {
            auto q1 = txt.find('"', txt.find(':', fp) + 1);
            auto q2 = txt.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
                reg.fingerprint = txt.substr(q1 + 1, q2 - q1 - 1);
        }
    }

    enum { TOP, IN_SLOTS, IN_SLOT, IN_POSS, IN_POSS_ITEM } state = TOP;
    RegSlot cur_slot;
    Possession cur_poss;

    size_t pos = 0;
    while (pos < txt.size()) {
        size_t eol = txt.find('\n', pos);
        if (eol == std::string::npos) eol = txt.size();
        std::string line = txt.substr(pos, eol - pos);
        pos = eol + 1;

        if (line.find("\"slots\"") != std::string::npos && line.find('[') != std::string::npos)
            { state = IN_SLOTS; continue; }
        if (line.find("\"possession\"") != std::string::npos && line.find('[') != std::string::npos)
            { state = IN_POSS; continue; }

        if (state == IN_SLOTS) {
            if (line.find('{') != std::string::npos)
                { state = IN_SLOT; cur_slot = {}; continue; }
            if (line.find(']') != std::string::npos) { state = TOP; continue; }
        }
        if (state == IN_SLOT) {
            if (line.find("mod_id") != std::string::npos) cur_slot.mod_id = json_str(line, "mod_id");
            if (line.find("\"fighter\"") != std::string::npos) cur_slot.fighter = json_int(line, "fighter");
            if (line.find("fighter_dir") != std::string::npos) cur_slot.fighter_dir = json_str(line, "fighter_dir");
            if (line.find("original_costume_no") != std::string::npos) cur_slot.original_costume_no = json_int(line, "original_costume_no");
            if (line.find("original_folder") != std::string::npos) cur_slot.original_folder = json_str(line, "original_folder");
            if (line.find("new_costume_no") != std::string::npos) cur_slot.new_costume_no = json_int(line, "new_costume_no");
            if (line.find("new_folder") != std::string::npos) cur_slot.new_folder = json_str(line, "new_folder");
            if (line.find("scene_name") != std::string::npos) cur_slot.scene_name = json_str(line, "scene_name");
            if (line.find('}') != std::string::npos)
                { reg.slots.push_back(cur_slot); state = IN_SLOTS; }
        }
        if (state == IN_POSS) {
            if (line.find('{') != std::string::npos) { state = IN_POSS_ITEM; cur_poss = {}; continue; }
            if (line.find(']') != std::string::npos) { state = TOP; continue; }
        }
        if (state == IN_POSS_ITEM) {
            if (line.find("\"fighter\"") != std::string::npos) cur_poss.fighter = json_int(line, "fighter");
            if (line.find("fighter_dir") != std::string::npos) cur_poss.fighter_dir = json_str(line, "fighter_dir");
            if (line.find("costume_no") != std::string::npos) cur_poss.costume_no = json_int(line, "costume_no");
            if (line.find("record_id") != std::string::npos) cur_poss.record_id = json_int(line, "record_id");
            if (line.find("manage_id") != std::string::npos) cur_poss.manage_id = json_int(line, "manage_id");
            if (line.find("\"name\"") != std::string::npos) cur_poss.name = json_str(line, "name");
            if (line.find('}') != std::string::npos) { reg.possession.push_back(cur_poss); state = IN_POSS; }
        }
    }
    return reg;
}

static void save_registry(const char* path, const Registry& reg) {
    // Ensure directory exists
    std::string dir(path);
    auto sl = dir.rfind('\\');
    auto sl2 = dir.rfind('/');
    if (sl2 != std::string::npos && (sl == std::string::npos || sl2 > sl)) sl = sl2;
    if (sl != std::string::npos) {
        std::string d = dir.substr(0, sl);
        CreateDirectoryA(d.c_str(), nullptr);
    }

    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\n  \"version\": %d,\n  \"fingerprint\": \"%s\",\n  \"slots\": [\n",
            reg.version, reg.fingerprint.c_str());
    for (size_t i = 0; i < reg.slots.size(); i++) {
        auto& s = reg.slots[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"mod_id\": \"%s\",\n", s.mod_id.c_str());
        fprintf(f, "      \"fighter\": %d,\n", s.fighter);
        fprintf(f, "      \"fighter_dir\": \"%s\",\n", s.fighter_dir.c_str());
        fprintf(f, "      \"original_costume_no\": %d,\n", s.original_costume_no);
        fprintf(f, "      \"original_folder\": \"%s\",\n", s.original_folder.c_str());
        fprintf(f, "      \"new_costume_no\": %d,\n", s.new_costume_no);
        fprintf(f, "      \"new_folder\": \"%s\",\n", s.new_folder.c_str());
        fprintf(f, "      \"scene_name\": \"%s\"\n", s.scene_name.c_str());
        fprintf(f, "    }%s\n", i+1 < reg.slots.size() ? "," : "");
    }
    fprintf(f, "  ],\n  \"next_ids\": {},\n  \"possession\": [\n");
    for (size_t i = 0; i < reg.possession.size(); i++) {
        auto& p = reg.possession[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"fighter\": %d,\n", p.fighter);
        fprintf(f, "      \"fighter_dir\": \"%s\",\n", p.fighter_dir.c_str());
        fprintf(f, "      \"costume_no\": %d,\n", p.costume_no);
        fprintf(f, "      \"record_id\": %d,\n", p.record_id);
        fprintf(f, "      \"manage_id\": %d,\n", p.manage_id);
        fprintf(f, "      \"name\": \"%s\"\n", p.name.c_str());
        fprintf(f, "    }%s\n", i+1 < reg.possession.size() ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

// ============================================================================
// Static meta parser
// ============================================================================

struct StaticRecord {
    int fighter, costume_no, record_id, manage_id;
};

struct StaticFile {
    std::string local;    // filename inside static/ dir
    std::string pak_path; // full pak path (e.g. "natives/stm/...")
};

struct StaticMeta {
    int slot_min = 5, slot_max = 104;
    std::vector<StaticRecord> records;
    std::vector<StaticFile> files;  // generic list from "files" section

    bool load(const char* path) {
        auto txt = read_file_text(path);
        if (txt.empty()) return false;
        // Parse slot_range
        auto sr = txt.find("\"slot_range\"");
        if (sr != std::string::npos) {
            auto br = txt.find('[', sr);
            if (br != std::string::npos) {
                sscanf(txt.c_str() + br, "[ %d , %d", &slot_min, &slot_max);
                slot_max--; // [5, 11) -> max = 10
            }
        }
        // Parse files array (generic static file list)
        auto fa = txt.find("\"files\"");
        if (fa != std::string::npos) {
            // Make sure this is the top-level "files", not inside a "records" object
            // by checking it comes before "records"
            auto ra_check = txt.find("\"records\"");
            if (ra_check == std::string::npos || fa < ra_check) {
                size_t fpos = txt.find('[', fa);
                auto fend = txt.find(']', fpos);
                if (fpos != std::string::npos && fend != std::string::npos) {
                    size_t p = fpos;
                    while (p < fend) {
                        auto ob = txt.find('{', p);
                        if (ob == std::string::npos || ob >= fend) break;
                        auto cb = txt.find('}', ob);
                        if (cb == std::string::npos) break;
                        std::string block = txt.substr(ob, cb - ob + 1);
                        StaticFile sf;
                        sf.local = json_str(block, "local");
                        sf.pak_path = json_str(block, "pak_path");
                        if (!sf.local.empty() && !sf.pak_path.empty())
                            files.push_back(sf);
                        p = cb + 1;
                    }
                }
            }
        }
        // Parse records array
        auto ra = txt.find("\"records\"");
        if (ra == std::string::npos) return false;
        size_t pos = txt.find('[', ra);
        while (pos < txt.size()) {
            auto ob = txt.find('{', pos);
            if (ob == std::string::npos) break;
            auto cb = txt.find('}', ob);
            if (cb == std::string::npos) break;
            std::string block = txt.substr(ob, cb - ob + 1);
            StaticRecord r;
            r.fighter = json_int(block, "fighter");
            r.costume_no = json_int(block, "costume_no");
            r.record_id = json_int(block, "record_id");
            r.manage_id = json_int(block, "manage_id");
            records.push_back(r);
            pos = cb + 1;
        }
        return true;
    }

    const StaticRecord* find(int fighter, int costume_no) const {
        for (auto& r : records)
            if (r.fighter == fighter && r.costume_no == costume_no) return &r;
        return nullptr;
    }
};

// ============================================================================
// MDF2 texture path scanner (UTF-16LE binary scan)
// ============================================================================

static std::vector<std::string> scan_mdf2_textures(const uint8_t* data, size_t len) {
    std::vector<std::string> paths;
    // Search for ".tex\0\0" in UTF-16LE
    const uint8_t pat[] = {0x2E,0x00, 0x74,0x00, 0x65,0x00, 0x78,0x00, 0x00,0x00};
    for (size_t i = 0; i + 10 <= len; i += 2) {
        if (memcmp(data + i, pat, 10) != 0) continue;
        size_t end = i + 8; // end of ".tex" (before null)
        size_t start = i;
        while (start >= 2 && !(data[start-2]==0 && data[start-1]==0))
            start -= 2;
        std::string path;
        for (size_t j = start; j < end; j += 2) {
            uint16_t c = data[j] | (uint16_t(data[j+1]) << 8);
            if (c == 0) break;
            path += (char)(c & 0xFF);
        }
        if (path.find('/') != std::string::npos || path.find('\\') != std::string::npos)
            paths.push_back(path);
        i = end;
    }
    return paths;
}

// ============================================================================
// Model folder parser (manual regex replacement)
// ============================================================================

static bool parse_model_folder(const std::string& path,
                               std::string& fighter_dir, std::string& folder) {
    const char* pfx = "natives/stm/product/model/esf/";
    size_t plen = strlen(pfx);
    // Case-insensitive find
    std::string low = str_lower(path);
    auto it = low.find(pfx);
    if (it == std::string::npos) return false;
    size_t pos = it + plen;
    if (pos + 6 >= low.size()) return false;
    if (low.substr(pos, 3) != "esf") return false;
    for (int k = 3; k < 6; k++) if (!isdigit(low[pos+k])) return false;
    fighter_dir = low.substr(pos, 6);
    pos += 6;
    if (pos >= low.size() || low[pos] != '/') return false;
    pos++;
    if (pos + 3 >= low.size()) return false;
    for (int k = 0; k < 3; k++) if (!isdigit(low[pos+k])) return false;
    folder = low.substr(pos, 3);
    pos += 3;
    if (pos >= low.size() || low[pos] != '/') return false;
    return true;
}

static std::string relocate_path(const std::string& pak_path,
                                  const std::string& fd,
                                  const std::string& old_f,
                                  const std::string& new_f) {
    std::string old_dir = fd + "/" + old_f + "/";
    std::string new_dir = fd + "/" + new_f + "/";
    std::string path = pak_path;
    auto p = path.find(old_dir);
    if (p != std::string::npos) path.replace(p, old_dir.size(), new_dir);
    std::string old_pfx = fd + "_" + old_f + "_";
    std::string new_pfx = fd + "_" + new_f + "_";
    auto sl = path.rfind('/');
    size_t from = (sl != std::string::npos) ? sl : 0;
    auto fp = path.find(old_pfx, from);
    if (fp != std::string::npos) path.replace(fp, old_pfx.size(), new_pfx);
    return path;
}

// ============================================================================
// Long-path file helpers (\\?\ for MAX_PATH bypass)
// ============================================================================

static std::wstring make_wide_path(const std::string& path) {
    std::wstring wp;
    if (path.size() > 240) {
        wp.reserve(path.size() + 5);
        wp = L"\\\\?\\";
        for (char c : path) wp += (wchar_t)(unsigned char)c;
    } else {
        wp.reserve(path.size());
        for (char c : path) wp += (wchar_t)(unsigned char)c;
    }
    return wp;
}

static std::vector<uint8_t> read_file_long(const std::string& path) {
    auto wp = make_wide_path(path);
    HANDLE h = CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER li;
    GetFileSizeEx(h, &li);
    std::vector<uint8_t> data((size_t)li.QuadPart);
    DWORD rd;
    ReadFile(h, data.data(), (DWORD)data.size(), &rd, nullptr);
    CloseHandle(h);
    return data;
}

static bool file_stat_long(const std::string& path, uint64_t& size, uint64_t& mtime) {
    auto wp = make_wide_path(path);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(wp.c_str(), GetFileExInfoStandard, &fa)) return false;
    size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    mtime = (uint64_t(fa.ftLastWriteTime.dwHighDateTime) << 32)
          | fa.ftLastWriteTime.dwLowDateTime;
    return true;
}

// ============================================================================
// Stale type signatures in mod user files
// ============================================================================
// A game update changed the CRC of app.CostumeMaterialData.MaterialData (0x4D532A61 -> current)
// without changing its layout: the colour files (cmd_*.user) of mods made before it are refused by the
// game, and the costume shows up white, hair included. Measured on DOA4 Christie (Cammy): its cmd_000
// and cmd_005 are the game's own files byte for byte, CRC aside. The current CRCs are read from a
// vanilla colour file at each generation; only types whose layout is known to be unchanged are
// rewritten, the others are reported.

static std::unordered_map<uint32_t, uint32_t> g_current_crc;          // type -> CRC of this game build
static const std::unordered_set<uint32_t> CRC_UPGRADABLE = {
    0x9E32F1F4,   // app.CostumeMaterialData.MaterialData: Type (enum ePartsType) + Clusters, unchanged
};
static std::unordered_set<std::string> g_crc_upgraded_files;
static std::unordered_map<uint32_t, std::unordered_set<std::string>> g_crc_stale_files;
// Colour files of 2023 (Cloth without ClothFur_Color, e.g. KenSFV): converted to the current layout,
// the fur block copied from this game colour file, where it is disabled.
static std::vector<uint8_t> g_cmd_reference;
// Textures whose mip table declared a wrong row pitch (repaired) or stays inconsistent
static std::unordered_set<std::string> g_tex_repaired_files, g_tex_bad_files;
static std::unordered_set<std::string> g_layout_upgraded_files;

static const char* type_label(uint32_t t) {
    switch (t) {
        case 0x9E32F1F4: return "app.CostumeMaterialData.MaterialData";
        case 0x9F67268B: return "app.CostumeMaterialData.Cloth";
        default: return nullptr;
    }
}

static void load_current_crcs(PakReader& base_pak) {
    g_current_crc.clear(); g_crc_upgraded_files.clear(); g_crc_stale_files.clear();
    g_cmd_reference.clear(); g_layout_upgraded_files.clear();
    g_tex_repaired_files.clear(); g_tex_bad_files.clear();
    const char* ref = "natives/stm/product/model/esf/esf001/001/esf001_001_cmd_000.user.2";
    if (auto* e = base_pak.find(pak_path_hash(std::string_view(ref)))) {
        g_cmd_reference = base_pak.read(*e);
        user_type_crcs(g_cmd_reference, g_current_crc);
    }
}

// source: file name for the log (disk path, or the pak entry hash)
static size_t upgrade_mod_user(std::vector<uint8_t>& data, const std::string& source) {
    if (data.size() >= 4 && memcmp(data.data(), "TEX\0", 4) == 0) {
        int fixed = 0;
        int bad = texture_mip_check(data, true, &fixed);
        if (fixed) g_tex_repaired_files.insert(source);
        if (bad) g_tex_bad_files.insert(source);
        return size_t(fixed);
    }
    if (g_current_crc.empty() || data.size() < 4 || memcmp(data.data(), "USR\0", 4) != 0) return 0;
    size_t garments = 0;
    if (upgrade_costume_material_layout(data, g_current_crc, g_cmd_reference, &garments)) {
        g_layout_upgraded_files.insert(source);
        return garments;
    }
    std::unordered_set<uint32_t> stale;
    size_t n = upgrade_user_crcs(data, g_current_crc, CRC_UPGRADABLE, &stale);
    if (n > 0) g_crc_upgraded_files.insert(source);
    for (uint32_t t : stale) g_crc_stale_files[t].insert(source);
    return n;
}

// ============================================================================
// Mod file reference (pak or disk)
// ============================================================================

struct ModFileRef {
    PakReader* pak = nullptr;
    uint64_t hash = 0;
    std::string disk_path;  // non-empty -> disk source

    bool is_disk() const { return !disk_path.empty(); }

    std::string source() const {
        if (!disk_path.empty()) return disk_path;
        char buf[32]; sprintf(buf, "pak entry %016llx", (unsigned long long)hash);
        return buf;
    }

    // Read decompressed data (from pak or disk). User files come back with current type signatures.
    std::vector<uint8_t> read_data() const {
        auto d = disk_path.empty() ? pak->read(*pak->find(hash)) : read_file_long(disk_path);
        upgrade_mod_user(d, source());
        return d;
    }

    // Read raw: returns (blob, attrib, decompressed_size).
    // Disk files -> attrib=0, blob=file content (uncompressed).
    // A small pak entry is decompressed to check whether it is a user file with a stale signature;
    // if so it is returned upgraded and uncompressed.
    void read_raw(std::vector<uint8_t>& out, int64_t& attrib, int64_t& dsize) const {
        if (!disk_path.empty()) {
            out = read_data();
            attrib = 0; dsize = (int64_t)out.size();
            return;
        }
        auto* e = pak->find(hash);
        if (e->decompressed_size <= (1 << 20) || (e->attributes & 0xFF) == 0) {
            auto d = pak->read(*e);
            if (upgrade_mod_user(d, source()) > 0) {
                out = std::move(d); attrib = 0; dsize = (int64_t)out.size();
                return;
            }
            if ((e->attributes & 0xFF) == 0) {   // stored: what was read is the raw blob
                out = std::move(d); attrib = e->attributes; dsize = e->decompressed_size;
                return;
            }
        }
        out = pak->read_raw(*e);
        attrib = e->attributes; dsize = e->decompressed_size;
    }
};

// Write the streaming twin of a relocated texture if it exists in mod files or base pak
static int write_streaming_twin(PakWriter& writer,
                                std::unordered_set<std::string>& written,
                                const std::string& orig_path,
                                const std::string& new_path,
                                const std::unordered_map<std::string, ModFileRef>& all_mod,
                                PakReader& base_pak) {
    if (str_lower(orig_path).find(".tex.") == std::string::npos) return 0;
    size_t bp = strlen(BASE_PFX);
    if (orig_path.size() <= bp || orig_path.compare(0, bp, BASE_PFX) != 0) return 0;
    std::string s_orig = std::string(STREAM_PFX) + orig_path.substr(bp);
    std::string s_new  = std::string(STREAM_PFX) + new_path.substr(bp);
    if (written.count(s_new)) return 0;
    auto mit = all_mod.find(s_orig);
    if (mit != all_mod.end()) {
        auto data = mit->second.read_data();
        uint64_t h = pak_path_hash(std::string_view(s_new));
        writer.add_uncompressed(h, std::move(data));
        written.insert(s_new);
        return 1;
    }
    uint64_t sh = pak_path_hash(std::string_view(s_orig));
    auto* se = base_pak.find(sh);
    if (se) {
        auto raw = base_pak.read_raw(*se);
        uint64_t h = pak_path_hash(std::string_view(s_new));
        writer.add_raw(h, std::move(raw), se->attributes, se->decompressed_size);
        written.insert(s_new);
        return 1;
    }
    return 0;
}

// ============================================================================
// Pak classification and mod identification
// ============================================================================

struct PakInfo { int num; std::string path; };

// Deletes a file, retrying for ~10 s while another process keeps it open. True once it is gone.
static bool delete_with_retry(const std::string& path) {
    for (int i = 0; i < 20; i++) {
        if (DeleteFileA(path.c_str())) return true;
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return true;
        Sleep(500);
    }
    return false;
}

static void classify_patch_paks(const std::string& pak_dir,
                                std::vector<PakInfo>& our_paks,
                                std::vector<PakInfo>& mod_paks) {
    uint64_t marker_h = pak_path_hash(std::string_view(MARKER_PATH));
    WIN32_FIND_DATAA fd;
    std::string pattern = pak_dir + "\\re_chunk_000.pak.patch_*.pak";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string fn = fd.cFileName;
        // Extract number
        auto ps = fn.find("patch_");
        if (ps == std::string::npos) continue;
        int num = atoi(fn.c_str() + ps + 6);
        std::string full = pak_dir + "\\" + fn;
        // Check for marker
        FILE* fp = fopen(full.c_str(), "rb");
        if (!fp) continue;
        struct { uint32_t magic; uint8_t maj,min; int16_t feat; uint32_t count,fp_; } hdr;
        fread(&hdr, 1, 16, fp);
        bool has_marker = false;
        for (uint32_t i = 0; i < hdr.count; i++) {
            uint32_t lo, hi; int64_t dummy[5];
            fread(&lo, 4, 1, fp); fread(&hi, 4, 1, fp);
            fread(dummy, 8, 5, fp);
            if (((uint64_t(hi)<<32)|lo) == marker_h) { has_marker = true; break; }
        }
        fclose(fp);
        (has_marker ? our_paks : mod_paks).push_back({num, full});
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(mod_paks.begin(), mod_paks.end(), [](auto& a, auto& b){ return a.num < b.num; });
}

static std::string identify_mod_pak(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    struct { uint32_t magic; uint8_t maj,min; int16_t feat; uint32_t count,fp; } hdr;
    fread(&hdr, 1, 16, f);
    struct E { uint64_t hash; int64_t cs; };
    std::vector<E> entries(hdr.count);
    for (uint32_t i = 0; i < hdr.count; i++) {
        uint32_t lo, hi; int64_t off, cs, ds, att, ck;
        fread(&lo,4,1,f); fread(&hi,4,1,f);
        fread(&off,8,1,f); fread(&cs,8,1,f); fread(&ds,8,1,f);
        fread(&att,8,1,f); fread(&ck,8,1,f);
        entries[i] = { (uint64_t(hi)<<32)|lo, cs };
    }
    fclose(f);
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b){
        return a.hash < b.hash || (a.hash == b.hash && a.cs < b.cs);
    });
    // MD5 of packed (hash, cs) pairs
    std::vector<uint8_t> buf(entries.size() * 16);
    for (size_t i = 0; i < entries.size(); i++) {
        memcpy(buf.data() + i*16, &entries[i].hash, 8);
        memcpy(buf.data() + i*16 + 8, &entries[i].cs, 8);
    }
    return md5_hex(buf.data(), buf.size()).substr(0, 16);
}

// ============================================================================
// Mod scanning (hash matching + mdf2 texture discovery)
// ============================================================================

// upgraded_tex: maps old-suffix hash -> current-suffix pak path (for relocation)
static void scan_mod_pak(PakReader& mod_pak, const VanillaIndex& inv,
                         std::unordered_map<std::string, ModFileRef>& known,
                         std::vector<uint64_t>& unknown,
                         std::unordered_map<uint64_t, std::string>& upgraded_tex) {
    std::unordered_set<uint64_t> unknown_set;
    for (auto& [h, e] : mod_pak.entries()) {
        auto it = inv.hash_to_paths.find(h);
        if (it != inv.hash_to_paths.end()) {
            for (auto& path : it->second)
                known[path] = {&mod_pak, h};
        } else {
            unknown_set.insert(h);
        }
    }
    std::string tex_ver;
    { auto it = inv.suffixes.find("tex"); if (it != inv.suffixes.end()) tex_ver = it->second; }
    int upgraded = 0;

    // Try old tex suffixes for remaining unknowns against vanilla inventory
    if (!unknown_set.empty()) {
        // Build map: for each vanilla .tex path, compute old-suffix hashes
        for (auto& [vh, vpaths] : inv.hash_to_paths) {
            for (auto& vp : vpaths) {
                if (!str_ends_with(str_lower(vp), (".tex." + tex_ver).c_str())) continue;
                // vp = "natives/stm/.../foo.tex.241101895"
                // stem = "natives/stm/.../foo.tex"
                std::string stem = vp.substr(0, vp.size() - tex_ver.size() - 1);
                for (int oi = 0; oi < N_OLD_TEX; oi++) {
                    std::string old_path = stem + "." + OLD_TEX_SUFFIXES[oi];
                    uint64_t oh = pak_path_hash(std::string_view(old_path));
                    if (unknown_set.count(oh)) {
                        // Found! Register under the CURRENT-suffix path
                        known[vp] = {&mod_pak, oh};
                        upgraded_tex[oh] = vp; // remember for renaming
                        unknown_set.erase(oh);
                        upgraded++;
                    }
                }
            }
        }
    }

    // Discover textures via mdf2 parsing
    int discovered = 0;
    for (auto& [path, mf] : std::unordered_map<std::string,ModFileRef>(known)) {
        if (!str_ends_with(str_lower(path), ".mdf2.31")) continue;
        auto data = mf.read_data();
        auto tex_paths = scan_mdf2_textures(data.data(), data.size());
        for (auto& tp : tex_paths) {
            std::string p = tp;
            for (auto& c : p) if (c == '\\') c = '/';
            std::string lp = str_lower(p);
            std::string pak_p = "natives/stm/" + lp + "." + tex_ver;
            uint64_t th = pak_path_hash(std::string_view(pak_p));
            if (unknown_set.count(th)) {
                known[pak_p] = {&mod_pak, th};
                unknown_set.erase(th);
                discovered++;
            }
            // Also try old suffixes for mdf2-discovered textures
            for (int oi = 0; oi < N_OLD_TEX; oi++) {
                std::string old_pak_p = "natives/stm/" + lp + "." + OLD_TEX_SUFFIXES[oi];
                uint64_t oh = pak_path_hash(std::string_view(old_pak_p));
                if (unknown_set.count(oh)) {
                    known[pak_p] = {&mod_pak, oh};
                    upgraded_tex[oh] = pak_p;
                    unknown_set.erase(oh);
                    discovered++;
                    upgraded++;
                }
            }
        }
    }
    if (discovered) printf("    discovered %d custom textures via mdf2\n", discovered);
    if (upgraded)   printf("    %d old-suffix textures upgraded\n", upgraded);

    // Resolve streaming twins of known textures
    int streaming_resolved = 0;
    size_t bp = strlen(BASE_PFX);
    for (auto& [path, mf] : std::unordered_map<std::string, ModFileRef>(known)) {
        if (str_lower(path).find(".tex.") == std::string::npos) continue;
        if (path.compare(0, bp, BASE_PFX) != 0) continue;
        std::string sp = std::string(STREAM_PFX) + path.substr(bp);
        uint64_t sh = pak_path_hash(std::string_view(sp));
        if (unknown_set.count(sh)) {
            known[sp] = {&mod_pak, sh};
            unknown_set.erase(sh);
            streaming_resolved++;
        }
    }
    if (streaming_resolved) printf("    %d streaming textures resolved\n", streaming_resolved);

    unknown.assign(unknown_set.begin(), unknown_set.end());
}

// ============================================================================
// Mod costume detection
// ============================================================================

struct ModCostume {
    int fighter;
    std::string fighter_dir;
    int original_costume_no;
    std::string original_folder;
    std::unordered_map<std::string, ModFileRef> files_in_folder;
    std::unordered_map<std::string, ModFileRef> files_shared;
    std::unordered_map<std::string, ModFileRef> files_other;
};

static std::vector<ModCostume> detect_mod_costumes(
    const std::unordered_map<std::string, ModFileRef>& mod_files,
    const VanillaIndex& inv) {

    std::map<std::string, ModCostume> groups; // "fd\tfolder" -> MC
    std::unordered_map<std::string, ModFileRef> other_files;
    // Collect 000/ files separately, keyed by fighter_dir
    std::unordered_map<std::string, std::unordered_map<std::string, ModFileRef>> shared_by_fd;

    for (auto& [pak_path, mf] : mod_files) {
        std::string fd, folder;
        if (!parse_model_folder(pak_path, fd, folder)) {
            other_files[pak_path] = mf; continue;
        }
        if (folder == "000") {
            shared_by_fd[fd][pak_path] = mf; continue;
        }
        std::string key = fd + "\t" + folder;
        if (groups.find(key) == groups.end()) {
            std::string lk = fd + "\t" + folder;
            auto it = inv.folder_to_costume.find(lk);
            if (it == inv.folder_to_costume.end()) continue;
            auto& ci = inv.costumes[it->second];
            ModCostume mc;
            mc.fighter = ci.fighter;
            mc.fighter_dir = ci.fighter_dir;
            mc.original_costume_no = ci.costume_no;
            mc.original_folder = folder;
            groups[key] = mc;
        }
        groups[key].files_in_folder[pak_path] = mf;
    }
    // Attach shared/other to groups
    for (auto& [k, mc] : groups) {
        // 000/ files for this fighter
        auto sit = shared_by_fd.find(mc.fighter_dir);
        if (sit != shared_by_fd.end())
            for (auto& [p, mf] : sit->second)
                mc.files_shared[p] = mf;
        // Extra-model files: specific by fighter+folder prefix
        std::string prefix = mc.fighter_dir + "_" + mc.original_folder + "_";
        for (auto& [p, mf] : other_files) {
            auto sl = p.rfind('/');
            std::string fn = (sl != std::string::npos) ? p.substr(sl+1) : p;
            if (fn.find(prefix) != std::string::npos)
                mc.files_other[p] = mf;
        }
    }
    // Reject partial mods: require at least one body mesh (part 01)
    std::vector<ModCostume> result;
    for (auto& [k, mc] : groups) {
        bool has_body = false;
        for (auto& [p, mf] : mc.files_in_folder) {
            if (p.find("/" + mc.original_folder + "/01/") != std::string::npos
                && str_ends_with(str_lower(p), ".mesh.230110883")) {
                has_body = true; break;
            }
        }
        if (has_body) {
            result.push_back(std::move(mc));
        } else {
            printf("  partial mod ignored: %s/%s (no body mesh in part 01, %zu files)\n",
                   mc.fighter_dir.c_str(), mc.original_folder.c_str(),
                   mc.files_in_folder.size());
        }
    }
    return result;
}

// ============================================================================
// Slot assignment
// ============================================================================

struct SlotInfo {
    std::string mod_id, fighter_dir, original_folder, new_folder, scene_name;
    std::string outfit_name;   // "Outfit I"... (BrewedVFX: shown in the slot log line)
    int fighter=0, original_costume_no=0, new_costume_no=0;
    int record_id=0, manage_id=0;
    const ModCostume* mod_costume = nullptr;
};

static std::vector<SlotInfo> assign_slots(
    const std::vector<std::pair<std::string, std::vector<ModCostume>>>& mods,
    const VanillaIndex& inv, Registry& reg, const StaticMeta& meta) {

    // Build set of mod_ids that are STILL PRESENT in this run
    std::unordered_set<std::string> present_mod_ids;
    for (auto& [mid, cs] : mods) present_mod_ids.insert(mid);

    // Build existing assignments from registry (only for mods still present)
    std::unordered_map<std::string, const RegSlot*> existing;
    for (auto& s : reg.slots) {
        if (!present_mod_ids.count(s.mod_id)) continue; // mod removed -> slot freed
        std::string key = s.mod_id + "\t" + std::to_string(s.fighter) + "\t"
                        + std::to_string(s.original_costume_no);
        existing[key] = &s;
    }

    // Per-fighter: sets of costume_no and folder values taken by PRESENT mods
    std::unordered_map<int, std::set<int>> taken_cno, taken_folder;
    for (auto& [key, rs] : existing) {
        taken_cno[rs->fighter].insert(rs->new_costume_no);
        taken_folder[rs->fighter].insert(atoi(rs->new_folder.c_str()));
    }

    // Smallest-free-slot: find smallest value in [lo..hi] not in 'taken'
    auto smallest_free = [](const std::set<int>& taken, int lo, int hi) -> int {
        for (int v = lo; v <= hi; v++)
            if (!taken.count(v)) return v;
        return -1;
    };

    std::vector<SlotInfo> slots;
    std::vector<RegSlot> new_reg;

    for (auto& [mod_id, costumes] : mods) {
        for (auto& mc : costumes) {
            SlotInfo si;
            si.mod_id = mod_id;
            si.fighter = mc.fighter;
            si.fighter_dir = mc.fighter_dir;
            si.original_costume_no = mc.original_costume_no;
            si.original_folder = mc.original_folder;
            si.mod_costume = &mc;

            std::string key = mod_id + "\t" + std::to_string(mc.fighter) + "\t"
                            + std::to_string(mc.original_costume_no);
            auto eit = existing.find(key);
            if (eit != existing.end()) {
                auto& es = *eit->second;
                si.new_costume_no = es.new_costume_no;
                si.new_folder = es.new_folder;
                si.scene_name = es.scene_name;
            } else {
                int cno = smallest_free(taken_cno[mc.fighter],
                                        meta.slot_min, meta.slot_max);
                if (cno < 0) continue; // out of slots
                int fld_min = inv.max_folder(mc.fighter) + 1;
                int fld = smallest_free(taken_folder[mc.fighter], fld_min, fld_min + 200);
                if (fld < 0) continue;
                si.new_costume_no = cno;
                taken_cno[mc.fighter].insert(cno);
                taken_folder[mc.fighter].insert(fld);
                char buf[32]; sprintf(buf, "%03d", fld);
                si.new_folder = buf;
                sprintf(buf, "%sv%02d", mc.fighter_dir.c_str(), cno);
                si.scene_name = buf;
            }
            auto* sr = meta.find(mc.fighter, si.new_costume_no);
            if (sr) {
                si.record_id = sr->record_id;
                si.manage_id = sr->manage_id;
            }
            slots.push_back(si);
            new_reg.push_back({si.mod_id, si.fighter_dir, si.original_folder,
                               si.new_folder, si.scene_name,
                               si.fighter, si.original_costume_no, si.new_costume_no});
        }
    }
    reg.slots = new_reg;
    return slots;
}

// ============================================================================
// add_slot_files (minimal mode)
// ============================================================================

// Original paths of the mod files a slot relocated. A mod-only file that no slot relocated (for
// instance a texture a DriveTech mod keeps in the C1 folder) must still be written where it is,
// even when another mod's slot relocates that same folder.
static std::unordered_set<std::string> g_relocated_mod_paths;
// Mod names by mod_id, for the log (the modinfo name, else the mod's folder or archive)
static std::unordered_map<std::string, std::string> g_mod_names;
static std::string mod_label(const std::string& id) {
    auto it = g_mod_names.find(id);
    return it == g_mod_names.end() ? id : it->second;
}

static void add_slot_files(PakWriter& writer,
                           std::unordered_set<std::string>& written_paths,
                           const SlotInfo& slot,
                           const VanillaIndex& inv,
                           PakReader& base_pak,
                           const std::unordered_map<std::string, ModFileRef>& all_mod) {
    auto* mc = slot.mod_costume;
    if (!mc) return;
    const std::string& fd = slot.fighter_dir;
    const std::string& old_f = slot.original_folder;
    const std::string& new_f = slot.new_folder;
    int count = 0, patched = 0;

    // Dir-only relocation helper (shared across phases)
    auto relocate_dir_only = [&](const std::string& p) -> std::string {
        std::string old_dir = fd + "/" + old_f + "/";
        std::string new_dir = fd + "/" + new_f + "/";
        std::string r = p;
        auto pos = r.find(old_dir);
        if (pos != std::string::npos) r.replace(pos, old_dir.size(), new_dir);
        return r;
    };

    // Textures the mod ships, named the way material files reference them: "<part>/<file>.tex"
    // relative to <fighter>/<folder>/, lowercase.
    auto tex_keys_for = [&](const std::unordered_map<std::string, ModFileRef>& files,
                            const std::string& folder) {
        std::unordered_set<std::string> keys;
        std::string base = std::string(BASE_PFX) + "model/esf/" + fd + "/" + folder + "/";
        for (auto& [p2, mf2] : files) {
            std::string low = str_lower(p2);
            if (low.compare(0, base.size(), base) != 0) continue;   // product files only
            auto dot = low.find(".tex.", base.size());
            if (dot == std::string::npos) continue;
            keys.insert(low.substr(base.size(), dot + 4 - base.size()));
        }
        return keys;
    };
    std::unordered_set<std::string> mod_tex_keys = tex_keys_for(mc->files_in_folder, old_f);
    // Shared 000/ textures shipped by the mod: phase 1c relocates them into the new folder
    std::unordered_set<std::string> shared_tex_keys = tex_keys_for(mc->files_shared, "000");
    // Points a material file at the relocated textures of the costume folder and of 000/
    auto patch_material = [&](std::vector<uint8_t>& d) -> size_t {
        size_t n = patch_mdf2_minimal(d, fd, old_f, new_f, mod_tex_keys);
        if (!shared_tex_keys.empty()) n += patch_mdf2_minimal(d, fd, "000", new_f, shared_tex_keys);
        return n;
    };

    // Relocate mod files
    int streaming_count = 0;
    std::vector<std::pair<std::string,std::string>> tex_pairs;
    for (auto& [path, mf] : mc->files_in_folder) {
        std::string low = str_lower(path);
        std::string fn_low = low;
        { auto sl = fn_low.rfind('/'); if (sl != std::string::npos) fn_low = fn_low.substr(sl+1); }
        bool is_chain_file = (fn_low.find("_chain.chain.") != std::string::npos
                           || fn_low.find("_chain.user.") != std::string::npos
                           || fn_low.find("_havok.") != std::string::npos
                           || fn_low.find("havokcloth") != std::string::npos);
        // Streaming textures: derive relocation from base path
        bool is_streaming = (low.compare(0, strlen(STREAM_PFX), STREAM_PFX) == 0);
        std::string new_path;
        if (is_streaming) {
            std::string base_p = std::string(BASE_PFX) + path.substr(strlen(STREAM_PFX));
            std::string base_new = is_chain_file ? relocate_dir_only(base_p)
                                                 : relocate_path(base_p, fd, old_f, new_f);
            new_path = std::string(STREAM_PFX) + base_new.substr(strlen(BASE_PFX));
        } else {
            new_path = is_chain_file ? relocate_dir_only(path)
                                     : relocate_path(path, fd, old_f, new_f);
        }

        if (str_ends_with(low, ".mdf2.31")) {
            auto data = mf.read_data();
            auto vdata = std::vector<uint8_t>(data.begin(), data.end());
            size_t n = patch_material(vdata);
            if (n > 0) patched++;
            uint64_t h = pak_path_hash(std::string_view(new_path));
            writer.add_uncompressed(h, std::move(vdata));
        } else if (needs_content_patch(path)) {
            auto data = mf.read_data();
            uint64_t h = pak_path_hash(std::string_view(new_path));
            writer.add_uncompressed(h, std::move(data));
        } else {
            std::vector<uint8_t> raw; int64_t att, ds;
            mf.read_raw(raw, att, ds);
            uint64_t h = pak_path_hash(std::string_view(new_path));
            writer.add_raw(h, std::move(raw), att, ds);
        }
        written_paths.insert(new_path);
        g_relocated_mod_paths.insert(path);
        count++;
        if (low.find(".tex.") != std::string::npos && !is_streaming)
            tex_pairs.push_back({path, new_path});
    }
    // Streaming twins for relocated textures
    for (auto& [orig, np] : tex_pairs)
        streaming_count += write_streaming_twin(writer, written_paths, orig, np, all_mod, base_pak);

    std::vector<std::string> relocated_vanilla_mdf2;   // vanilla materials relocated by phase 1b
    // ---- Phase 1b: vanilla mdf2 relocation for the parts the mod gives no material ----
    // Decided per part (01 body, 02 hair, 30 weapon...): a part whose mesh the mod ships without a
    // material gets the vanilla material of that part, bound to the mod's textures. When the mod ships
    // no material at all, every vanilla material of the folder is relocated, as before. Deciding for
    // the whole folder left the body of "outfit + hair add-on" bundles without any material.
    {
        std::string part_base = std::string(BASE_PFX) + "model/esf/" + fd + "/" + old_f + "/";
        auto part_of = [&](const std::string& p) -> std::string {
            std::string low = str_lower(p);
            if (low.compare(0, part_base.size(), part_base) != 0) return {};
            auto sl = low.find('/', part_base.size());
            return (sl == std::string::npos) ? std::string() : low.substr(part_base.size(), sl - part_base.size());
        };
        std::set<std::string> parts_mesh, parts_mdf2;
        for (auto& [p, mf] : mc->files_in_folder) {
            std::string part = part_of(p);
            if (part.empty()) continue;
            std::string low = str_lower(p);
            if (str_ends_with(low, ".mdf2.31")) parts_mdf2.insert(part);
            else if (low.find(".mesh.") != std::string::npos) parts_mesh.insert(part);
        }
        bool has_mod_mdf2 = !parts_mdf2.empty();
        auto part_needs_vanilla = [&](const std::string& part) {
            if (parts_mdf2.count(part)) return false;
            return !has_mod_mdf2 || parts_mesh.count(part) > 0;
        };
        bool any_part_needs = !has_mod_mdf2;
        for (auto& part : parts_mesh) if (part_needs_vanilla(part)) any_part_needs = true;

        if (any_part_needs) {
            // Find vanilla mdf2 files for this costume from inventory
            auto* ci = inv.get_costume(slot.fighter, slot.original_costume_no);
            if (ci) {
                std::string mprefix = inv.model_prefix(fd, old_f);
                // Scan the vanilla inventory FILES for mdf2s in this costume's folder
                for (auto& [vh, vpaths] : inv.hash_to_paths) {
                    for (auto& vp : vpaths) {
                        if (vp.size() < mprefix.size()) continue;
                        if (vp.compare(0, mprefix.size(), mprefix) != 0) continue;
                        if (!str_ends_with(str_lower(vp), ".mdf2.31")) continue;
                        if (!part_needs_vanilla(part_of(vp))) continue;
                        // Found a vanilla mdf2 in this costume's folder
                        std::string new_path = relocate_path(vp, fd, old_f, new_f);
                        if (written_paths.count(new_path)) continue;
                        auto* ve = base_pak.find(vh);
                        if (!ve) continue;
                        auto mdf_data = base_pak.read(*ve);
                        // Point the vanilla material at the textures the mod ships (by path:
                        // folder mods have no pak hash, so matching by hash never fired)
                        patch_material(mdf_data);
                        uint64_t nh = pak_path_hash(std::string_view(new_path));
                        writer.add_uncompressed(nh, std::move(mdf_data));
                        written_paths.insert(new_path);
                        relocated_vanilla_mdf2.push_back(vp);
                        count++; patched++;
                    }
                }
            }
            // Parts the index does not list (weapons, props: the costume scene does not name them,
            // a model_parts file does). Derive the vanilla material from the mod's mesh name.
            for (auto& [p, mf] : mc->files_in_folder) {
                std::string low = str_lower(p);
                auto mpos = low.find(".mesh.");
                if (mpos == std::string::npos) continue;
                std::string part = part_of(p);
                if (part.empty() || !part_needs_vanilla(part)) continue;
                std::string vp = low.substr(0, mpos) + "_v00.mdf2.31";
                std::string new_path = relocate_path(vp, fd, old_f, new_f);
                if (written_paths.count(new_path)) continue;
                auto* ve = base_pak.find(pak_path_hash(std::string_view(vp)));
                if (!ve) continue;
                auto mdf_data = base_pak.read(*ve);
                patch_material(mdf_data);
                writer.add_uncompressed(pak_path_hash(std::string_view(new_path)), std::move(mdf_data));
                written_paths.insert(new_path);
                relocated_vanilla_mdf2.push_back(vp);
                count++; patched++;
            }
        }
    }

    // ---- Phase 2: CCVD + CMD + chain (dir-only relocation) ----
    std::string base_model = "natives/stm/product/model/esf/" + fd + "/" + old_f + "/";
    const auto& amf = all_mod;

    // 2a. CCVD: read, patch dir segment, write; keep patched data for 2b scan
    std::vector<uint8_t> ccvd_patched; // kept alive for CMD discovery
    {
        std::string ccvd_orig = base_model + fd + "_" + old_f + "_ccvd.user.2";
        std::string ccvd_target = relocate_dir_only(ccvd_orig);
        if (!written_paths.count(ccvd_target)) {
            std::vector<uint8_t> ccvd_data;
            auto mfit = mc->files_in_folder.find(ccvd_orig);
            if (mfit != mc->files_in_folder.end())
                ccvd_data = mfit->second.read_data();
            else if (amf.count(ccvd_orig)) {
                ccvd_data = amf.at(ccvd_orig).read_data();
            } else {
                uint64_t oh = pak_path_hash(std::string_view(ccvd_orig));
                auto* oe = base_pak.find(oh);
                if (oe) ccvd_data = base_pak.read(*oe);
            }
            if (!ccvd_data.empty()) {
                // Patch dir segment in CCVD content (UTF-16LE only)
                std::string old_ds = fd + "/" + old_f + "/";
                std::string new_ds = fd + "/" + new_f + "/";
                std::vector<uint8_t> old_d16, new_d16;
                for (char c : old_ds) { old_d16.push_back((uint8_t)c); old_d16.push_back(0); }
                for (char c : new_ds) { new_d16.push_back((uint8_t)c); new_d16.push_back(0); }
                for (size_t i = 0; i + old_d16.size() <= ccvd_data.size(); ) {
                    if (memcmp(ccvd_data.data()+i, old_d16.data(), old_d16.size()) == 0) {
                        memcpy(ccvd_data.data()+i, new_d16.data(), new_d16.size());
                        i += old_d16.size();
                    } else i += 2;
                }
                ccvd_patched = ccvd_data; // keep a copy for CMD discovery
                uint64_t th = pak_path_hash(std::string_view(ccvd_target));
                writer.add_uncompressed(th, std::move(ccvd_data));
                written_paths.insert(ccvd_target);
                count++; patched++;
            }
        }
    }

    // 2b. CMD files: discover from CCVD content, relocate dir-only
    {
        std::set<std::string> cmd_names;
        if (!ccvd_patched.empty()) {
            // Scan patched CCVD for .user refs under the new folder (UTF-16LE)
            std::string new_dir_s = fd + "/" + new_f + "/";
            std::vector<uint8_t> pat16;
            for (char c : new_dir_s) { pat16.push_back((uint8_t)c); pat16.push_back(0); }
            for (size_t ci = 0; ci + pat16.size() <= ccvd_patched.size(); ) {
                if (memcmp(ccvd_patched.data() + ci, pat16.data(), pat16.size()) != 0) {
                    ci += 2; continue;
                }
                // Read forward to null terminator
                size_t ce = ci;
                while (ce + 1 < ccvd_patched.size()) {
                    if (ccvd_patched[ce] == 0 && ccvd_patched[ce+1] == 0) break;
                    ce += 2;
                }
                // Decode UTF-16LE string
                std::string ref;
                for (size_t j = ci; j < ce; j += 2) {
                    uint16_t ch = ccvd_patched[j] | (uint16_t(ccvd_patched[j+1]) << 8);
                    if (ch == 0) break;
                    ref += (char)(ch & 0xFF);
                }
                // Extract filename, check .user
                auto sl = ref.rfind('/');
                std::string fn = (sl != std::string::npos) ? ref.substr(sl+1) : ref;
                while (!fn.empty() && fn.back() == '\0') fn.pop_back();
                fn = str_lower(fn);
                if (fn.size() > 5 && fn.substr(fn.size()-5) == ".user") {
                    cmd_names.insert(fn + ".2"); // add version suffix
                }
                ci += pat16.size();
            }
        }
        // Fallback: hardcoded set if CCVD parsing found nothing
        if (cmd_names.empty()) {
            for (int i = 0; i <= 10; i++) {
                char buf[64]; sprintf(buf, "%s_%s_cmd_%03d.user.2", fd.c_str(), old_f.c_str(), i);
                cmd_names.insert(buf);
            }
            for (int i = 1; i <= 7; i++) {
                char buf[64]; sprintf(buf, "%s_%s_cmd_dx_%03d.user.2", fd.c_str(), old_f.c_str(), i);
                cmd_names.insert(buf);
            }
        }
        for (auto& cn : cmd_names) {
            std::string cmd_orig = base_model + cn;
            std::string cmd_target = relocate_dir_only(cmd_orig);
            if (written_paths.count(cmd_target)) continue;
            uint64_t th = pak_path_hash(std::string_view(cmd_target));
            auto mfit = mc->files_in_folder.find(cmd_orig);
            if (mfit != mc->files_in_folder.end()) {
                std::vector<uint8_t> raw; int64_t att, ds;
                mfit->second.read_raw(raw, att, ds);
                writer.add_raw(th, std::move(raw), att, ds);
                written_paths.insert(cmd_target); count++;
            } else if (amf.count(cmd_orig)) {
                std::vector<uint8_t> raw; int64_t att, ds;
                amf.at(cmd_orig).read_raw(raw, att, ds);
                writer.add_raw(th, std::move(raw), att, ds);
                written_paths.insert(cmd_target); count++;
            } else {
                uint64_t oh = pak_path_hash(std::string_view(cmd_orig));
                auto* oe = base_pak.find(oh);
                if (oe) {
                    auto raw = base_pak.read_raw(*oe);
                    writer.add_raw(th, std::move(raw), oe->attributes, oe->decompressed_size);
                    written_paths.insert(cmd_target); count++;
                }
            }
        }
    }

    // 2c. chain.52: from mod or vanilla (renamed prefix in filename)
    {
        std::string chain_orig = base_model + fd + "_" + old_f + "_01_chain.chain.52";
        // Dir-only relocation for chain.chain (keep filename prefix)
        std::string chain_target = chain_orig;
        { auto p = chain_target.find(fd + "/" + old_f + "/");
          if (p != std::string::npos) chain_target.replace(p, (fd + "/" + old_f + "/").size(),
                                                           fd + "/" + new_f + "/"); }
        if (!written_paths.count(chain_target)) {
            uint64_t th = pak_path_hash(std::string_view(chain_target));
            auto mfit = mc->files_in_folder.find(chain_orig);
            if (mfit != mc->files_in_folder.end()) {
                auto data2 = mfit->second.read_data();
                writer.add_uncompressed(th, std::move(data2));
                written_paths.insert(chain_target); count++;
            } else {
                uint64_t oh = pak_path_hash(std::string_view(chain_orig));
                auto* oe = base_pak.find(oh);
                if (oe) {
                    auto raw = base_pak.read_raw(*oe);
                    writer.add_raw(th, std::move(raw), oe->attributes, oe->decompressed_size);
                    written_paths.insert(chain_target); count++;
                }
            }
        }
    }

    // 2d. chain.user: for parts with mod chain.chain, relocate vanilla chain.user
    std::set<std::string> mod_chain_parts;
    {
        for (auto& [path, mf] : mc->files_in_folder) {
            std::string fl = str_lower(path);
            auto fsl = fl.rfind('/');
            std::string fn = (fsl != std::string::npos) ? fl.substr(fsl+1) : fl;
            if (fn.find("_chain.chain.") != std::string::npos ||
                fn.find("_havok.") != std::string::npos) {
                std::string mfd, mfolder;
                if (parse_model_folder(path, mfd, mfolder)) {
                    std::string rest = path.substr(path.find("/" + mfolder + "/") + mfolder.size() + 2);
                    auto rsl = rest.find('/');
                    if (rsl != std::string::npos)
                        mod_chain_parts.insert(rest.substr(0, rsl));
                }
            }
        }
        for (auto& part : mod_chain_parts) {
            std::string cu_orig = base_model + fd + "_" + old_f + "_" + part + "_chain.user.2";
            std::string cu_target = cu_orig;
            { auto p = cu_target.find(fd + "/" + old_f + "/");
              if (p != std::string::npos) cu_target.replace(p, (fd + "/" + old_f + "/").size(),
                                                            fd + "/" + new_f + "/"); }
            if (written_paths.count(cu_target)) continue;
            std::vector<uint8_t> cu_data;
            auto mfit = mc->files_in_folder.find(cu_orig);
            if (mfit != mc->files_in_folder.end())
                cu_data = mfit->second.read_data();
            else {
                uint64_t oh = pak_path_hash(std::string_view(cu_orig));
                auto* oe = base_pak.find(oh);
                if (oe) cu_data = base_pak.read(*oe);
            }
            if (cu_data.empty()) continue;
            // Patch internal dir ref: fd/old_f/part/ -> fd/new_f/part/
            std::string old_ds = fd + "/" + old_f + "/" + part + "/";
            std::string new_ds = fd + "/" + new_f + "/" + part + "/";
            std::vector<uint8_t> old_d16, new_d16;
            for (char c : old_ds) { old_d16.push_back((uint8_t)c); old_d16.push_back(0); }
            for (char c : new_ds) { new_d16.push_back((uint8_t)c); new_d16.push_back(0); }
            for (size_t ci = 0; ci + old_d16.size() <= cu_data.size(); ) {
                if (memcmp(cu_data.data()+ci, old_d16.data(), old_d16.size()) == 0) {
                    memcpy(cu_data.data()+ci, new_d16.data(), new_d16.size());
                    ci += old_d16.size();
                } else ci += 2;
            }
            uint64_t th = pak_path_hash(std::string_view(cu_target));
            writer.add_uncompressed(th, std::move(cu_data));
            written_paths.insert(cu_target);
            count++; patched++;
        }
    }

    // ---- Phase 1c: Relocate mod-provided 000/ shared files ----
    std::set<std::string> shared_parts;
    for (auto& [path, mf] : mc->files_shared) {
        std::string sfd, sfolder;
        if (!parse_model_folder(path, sfd, sfolder) || sfolder != "000") continue;
        // Extract part number from path after "000/"
        auto prefix_end = path.find("/000/");
        if (prefix_end == std::string::npos) continue;
        std::string rest = path.substr(prefix_end + 5); // after "000/"
        auto slash = rest.find('/');
        if (slash == std::string::npos) continue;
        std::string part = rest.substr(0, slash);
        std::string new_path = relocate_path(path, fd, "000", new_f);
        if (written_paths.count(new_path)) continue;
        g_relocated_mod_paths.insert(path);
        shared_parts.insert(part);
        std::string low = str_lower(path);
        if (str_ends_with(low, ".mdf2.31")) {
            auto data = mf.read_data();
            auto vdata = std::vector<uint8_t>(data.begin(), data.end());
            size_t n = patch_material(vdata);
            if (n > 0) patched++;
            uint64_t h = pak_path_hash(std::string_view(new_path));
            writer.add_uncompressed(h, std::move(vdata));
        } else if (needs_content_patch(path)) {
            auto data = mf.read_data();
            uint64_t h = pak_path_hash(std::string_view(new_path));
            writer.add_uncompressed(h, std::move(data));
        } else {
            std::vector<uint8_t> raw; int64_t att, ds;
            mf.read_raw(raw, att, ds);
            uint64_t h = pak_path_hash(std::string_view(new_path));
            writer.add_raw(h, std::move(raw), att, ds);
        }
        written_paths.insert(new_path);
        count++;
    }
    if (!shared_parts.empty()) {
        // Streaming twins for shared textures
        for (auto& [sp, smf] : mc->files_shared) {
            if (str_lower(sp).find(".tex.") == std::string::npos || sp.find("/000/") == std::string::npos)
                continue;
            std::string sh_new = relocate_path(sp, fd, "000", new_f);
            streaming_count += write_streaming_twin(writer, written_paths, sp, sh_new, all_mod, base_pak);
        }
        printf("      relocated shared files from 000/ parts\n");
    }

    // ---- Phase 3: External files matching this costume's prefix ----
    {
        std::string ext_prefix = fd + "_" + old_f + "_";
        int ext_count = 0;
        for (auto& [path, mf] : mc->files_other) {
            auto sl = path.rfind('/');
            std::string fn = (sl != std::string::npos) ? path.substr(sl+1) : path;
            if (fn.find(ext_prefix) == std::string::npos) continue;
            if (path.find("/product/model/esf/") != std::string::npos) continue;
            std::string new_fn = fn;
            auto pos = new_fn.find(ext_prefix);
            if (pos != std::string::npos)
                new_fn.replace(pos, ext_prefix.size(), fd + "_" + new_f + "_");
            std::string new_path = (sl != std::string::npos)
                                 ? path.substr(0, sl+1) + new_fn : new_fn;
            if (written_paths.count(new_path)) continue;
            std::vector<uint8_t> raw; int64_t att, ds;
            mf.read_raw(raw, att, ds);
            uint64_t h = pak_path_hash(std::string_view(new_path));
            writer.add_raw(h, std::move(raw), att, ds);
            written_paths.insert(new_path);
            count++; ext_count++;
        }
        if (ext_count)
            printf("      %d extra-model files relocated\n", ext_count);
    }

    // ---- Phase 3b: model parts (weapons, props) ----
    // The costume scene does not name weapon meshes itself: it references
    // battle_ud/<fd>vNN_model_parts.user, which lists them (Lily's clubs: 001/30 and 001/31).
    // When the slot relocated such a part, the slot gets its own copy in battle<new>/ (same length
    // as battle_ud/, so the scene can be patched in place) pointing at the relocated meshes and
    // materials, and the scene is redirected to that copy below.
    std::string mp_old_s, mp_new_s;
    {
        auto* oc = inv.get_costume(slot.fighter, slot.original_costume_no);
        const PakEntry* se0 = (oc && !oc->scene.empty())
            ? base_pak.find(pak_path_hash(std::string_view(oc->scene))) : nullptr;
        if (se0) {
            auto sd = base_pak.read(*se0);
            std::string needle = fd + "/battle_ud/" + fd + "v";
            std::vector<uint8_t> n16;
            for (char c : needle) { n16.push_back((uint8_t)c); n16.push_back(0); }
            std::string mp_rel;
            for (size_t i = 0; i + n16.size() <= sd.size() && mp_rel.empty(); i += 2) {
                bool eq = true;
                for (size_t k = 0; k < n16.size(); k += 2)
                    if (tolower(sd[i + k]) != n16[k] || sd[i + k + 1] != 0) { eq = false; break; }
                if (!eq) continue;
                size_t s0 = i;
                while (s0 >= 2 && !(sd[s0 - 2] == 0 && sd[s0 - 1] == 0)) s0 -= 2;
                std::string full;
                for (size_t j = s0; j + 1 < sd.size(); j += 2) {
                    uint16_t c = sd[j] | (uint16_t(sd[j + 1]) << 8);
                    if (!c) break;
                    full += (char)tolower(c & 0xFF);
                }
                if (full.find("_model_parts.user") != std::string::npos) mp_rel = full;
            }
            while (!mp_rel.empty() && (mp_rel[0] == '@' || mp_rel[0] == ' ')) mp_rel.erase(0, 1);
            std::string user_ver = "2";
            { auto it = inv.suffixes.find("user"); if (it != inv.suffixes.end()) user_ver = it->second; }
            const PakEntry* me = mp_rel.empty() ? nullptr
                : base_pak.find(pak_path_hash(std::string_view("natives/stm/" + mp_rel + "." + user_ver)));
            if (me) {
                auto mdata = base_pak.read(*me);
                std::vector<uint8_t> vd(mdata.begin(), mdata.end());
                // Meshes and materials this slot relocated, as "<part>/<file>.mesh|.mdf2"
                auto part_keys = [&](const std::string& folder,
                                     const std::unordered_map<std::string, ModFileRef>& files,
                                     bool with_vanilla) {
                    std::unordered_set<std::string> keys;
                    std::string base = std::string(BASE_PFX) + "model/esf/" + fd + "/" + folder + "/";
                    auto add = [&](const std::string& pth) {
                        std::string low = str_lower(pth);
                        if (low.compare(0, base.size(), base) != 0) return;
                        std::string rest = low.substr(base.size());
                        for (const char* ext : {".mesh.", ".mdf2."}) {
                            auto d = rest.find(ext);
                            if (d != std::string::npos) { keys.insert(rest.substr(0, d + strlen(ext) - 1)); return; }
                        }
                    };
                    for (auto& [pth, mf] : files) add(pth);
                    if (with_vanilla) for (auto& vp : relocated_vanilla_mdf2) add(vp);
                    return keys;
                };
                size_t n = patch_mdf2_minimal(vd, fd, old_f, new_f, part_keys(old_f, mc->files_in_folder, true));
                if (!shared_parts.empty())
                    n += patch_mdf2_minimal(vd, fd, "000", new_f, part_keys("000", mc->files_shared, false));
                if (n > 0 && new_f.size() == 3) {
                    std::string dst_rel = mp_rel;
                    auto bpos = dst_rel.find("/battle_ud/");
                    dst_rel.replace(bpos, 11, "/battle" + new_f + "/");
                    std::string dst_pak = "natives/stm/" + dst_rel + "." + user_ver;
                    writer.add_uncompressed(pak_path_hash(std::string_view(dst_pak)), std::move(vd));
                    written_paths.insert(dst_pak);
                    count++; patched++;
                    mp_old_s = fd + "/battle_ud/" + fd + "v";
                    mp_new_s = fd + "/battle" + new_f + "/" + fd + "v";
                    printf("      model parts: %zu references repointed in %s\n", n, dst_rel.c_str());
                }
            }
        }
    }

    // Costume scene: targeted patching
    auto* orig_costume = inv.get_costume(slot.fighter, slot.original_costume_no);
    if (orig_costume && !orig_costume->scene.empty()) {
        uint64_t sh = pak_path_hash(std::string_view(orig_costume->scene));
        auto* se = base_pak.find(sh);
        if (se) {
            auto scene_data = base_pak.read(*se);
            auto vdata = std::vector<uint8_t>(scene_data.begin(), scene_data.end());
            patch_scene_minimal(vdata, fd, old_f, new_f,
                                &written_paths, &inv.suffixes);
            // Second pass: patch 000/ references for parts we relocated
            if (!shared_parts.empty())
                patch_scene_minimal(vdata, fd, "000", new_f,
                                    &written_paths, &inv.suffixes);
            // Third pass: weather/havok chain filename-prefix rename
            if (!mod_chain_parts.empty()) {
                std::string old_wpfx = fd + "_" + old_f + "_";
                std::string new_wpfx = fd + "_" + new_f + "_";
                std::vector<uint8_t> ow16, nw16;
                for (char c : old_wpfx) { ow16.push_back((uint8_t)c); ow16.push_back(0); }
                for (char c : new_wpfx) { nw16.push_back((uint8_t)c); nw16.push_back(0); }
                for (size_t wi = 0; wi + ow16.size() <= vdata.size(); ) {
                    if (memcmp(vdata.data()+wi, ow16.data(), ow16.size()) != 0) {
                        wi += 2; continue;
                    }
                    // Read full string to check if it's a chain ref
                    size_t we = wi;
                    while (we + 1 < vdata.size() && !(vdata[we]==0 && vdata[we+1]==0))
                        we += 2;
                    std::string ws;
                    for (size_t wj = wi; wj < we; wj += 2) {
                        uint16_t wc = vdata[wj] | (uint16_t(vdata[wj+1]) << 8);
                        if (wc == 0) break;
                        ws += (char)tolower(wc & 0xFF);
                    }
                    if (ws.find("chain.chain") != std::string::npos ||
                        ws.find("havok") != std::string::npos) {
                        memcpy(vdata.data()+wi, nw16.data(), nw16.size());
                        patched++;
                    }
                    wi += ow16.size();
                }
            }
            // Fourth pass: point the scene at the slot's model_parts copy. Like CCVD, only from the
            // second occurrence on: the first one sits in the scene's userdata table.
            if (!mp_old_s.empty()) {
                std::vector<uint8_t> o16, n16;
                for (char c : mp_old_s) { o16.push_back((uint8_t)c); o16.push_back(0); }
                for (char c : mp_new_s) { n16.push_back((uint8_t)c); n16.push_back(0); }
                int occ = 0, done = 0;
                for (size_t wi = 0; wi + o16.size() <= vdata.size(); wi += 2) {
                    bool eq = true;
                    for (size_t k = 0; k < o16.size(); k += 2)
                        if (tolower(vdata[wi + k]) != o16[k] || vdata[wi + k + 1] != 0) { eq = false; break; }
                    if (!eq) continue;
                    if (++occ >= 2) { memcpy(vdata.data() + wi, n16.data(), n16.size()); done++; patched++; }
                    wi += o16.size() - 2;
                }
                printf("      model parts: scene redirected (%d of %d references)\n", done, occ);
            }
            std::string new_scene = "natives/stm/product/charparam/esf/"
                                  + fd + "/" + slot.scene_name + ".scn.20";
            uint64_t nh = pak_path_hash(std::string_view(new_scene));
            writer.add_uncompressed(nh, std::move(vdata));
            written_paths.insert(new_scene);
            count++; patched++;
        }
    }

    if (streaming_count)
        printf("      %d streaming textures relocated\n", streaming_count);
    printf("    slot %s/%s (v%02d, %s): %d files, %d patched\n",
           fd.c_str(), new_f.c_str(), slot.new_costume_no,
           slot.outfit_name.c_str(), count, patched);
}

// ============================================================================
// Restorations
// ============================================================================

static void add_restorations(PakWriter& writer,
                             std::unordered_set<std::string>& written_paths,
                             const std::vector<SlotInfo>& slots,
                             const VanillaIndex& inv,
                             PakReader& base_pak,
                             const std::unordered_map<std::string, ModFileRef>& all_mod) {
    std::unordered_set<std::string> relocated_prefixes;
    for (auto& sl : slots)
        relocated_prefixes.insert(inv.model_prefix(sl.fighter_dir, sl.original_folder));

    int restored = 0, added = 0, streaming_restored = 0;
    size_t bp = strlen(BASE_PFX);
    for (auto& [pak_path, mf] : all_mod) {
        if (written_paths.count(pak_path)) continue;
        // Skip streaming/ paths (handled as twins below)
        if (pak_path.compare(0, strlen(STREAM_PFX), STREAM_PFX) == 0) continue;
        bool in_relocated = false;
        for (auto& pfx : relocated_prefixes)
            if (pak_path.size() >= pfx.size() && pak_path.compare(0, pfx.size(), pfx) == 0)
                { in_relocated = true; break; }
        uint64_t h = pak_path_hash(std::string_view(pak_path));
        auto* be = base_pak.find(h);
        if (be) {
            auto raw = base_pak.read_raw(*be);
            writer.add_raw(h, std::move(raw), be->attributes, be->decompressed_size);
            written_paths.insert(pak_path);
            restored++;
            // Also restore streaming twin
            if (str_lower(pak_path).find(".tex.") != std::string::npos
                && pak_path.compare(0, bp, BASE_PFX) == 0) {
                std::string sp = std::string(STREAM_PFX) + pak_path.substr(bp);
                if (!written_paths.count(sp)) {
                    uint64_t sh = pak_path_hash(std::string_view(sp));
                    auto* se = base_pak.find(sh);
                    if (se) {
                        auto sr = base_pak.read_raw(*se);
                        writer.add_raw(sh, std::move(sr), se->attributes, se->decompressed_size);
                        written_paths.insert(sp);
                        streaming_restored++;
                    }
                }
            }
        } else if (pak_path.find("/model/esf/") != std::string::npos
                   && !g_relocated_mod_paths.count(pak_path)) {
            // Not in the game and not relocated by any slot: another slot may still reference it
            // at this path (in_relocated alone dropped files a slot of another mod needed).
            (void)in_relocated;
            auto data = mf.read_data();
            writer.add_uncompressed(h, std::move(data));
            written_paths.insert(pak_path);
            added++;
            // ...with its high-resolution twin when the mod ships one
            if (str_lower(pak_path).find(".tex.") != std::string::npos
                && pak_path.compare(0, bp, BASE_PFX) == 0) {
                std::string sp = std::string(STREAM_PFX) + pak_path.substr(bp);
                auto tw = all_mod.find(sp);
                if (tw != all_mod.end() && !written_paths.count(sp)) {
                    auto sd = tw->second.read_data();
                    writer.add_uncompressed(pak_path_hash(std::string_view(sp)), std::move(sd));
                    written_paths.insert(sp);
                }
            }
        }
    }
    printf("  restored %d vanilla files, added %d mod-only files", restored, added);
    if (streaming_restored) printf(", %d streaming", streaming_restored);
    printf("\n");
}

// ============================================================================
// Slot check
// ============================================================================
// A costume the game cannot load does not fail: its loading never completes and every costume shown
// after it stays empty (Juri, 27/09: textures missing; Vegeta, 27/09: a head mesh whose materials its
// material file does not define). Before the pak is written, every slot scene is read back the way the
// game reads it: each mesh must be readable and each of its materials defined by the material file the
// scene pairs it with (true of all 362 vanilla parts), and every file the slot references must exist:
// the scene's own references and the textures of its material files. A failing part is rebuilt from
// the mod's shared 000/ version of the part, else from the original costume's part; a missing file is
// taken from the mod, else from the original costume. A slot that still fails is left out of the game
// instead of being shipped, and the log says why.

namespace slotcheck {

// Printable UTF-16LE strings of a resource, in file order
static std::vector<std::string> utf16_strings(const std::vector<uint8_t>& d, size_t min_len = 6) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i + 1 < d.size(); i += 2) {
        uint16_t c = uint16_t(d[i] | (d[i + 1] << 8));
        if (c >= 0x20 && c < 0x7F) { cur += char(c); continue; }
        if (cur.size() >= min_len) out.push_back(cur);
        cur.clear();
    }
    if (cur.size() >= min_len) out.push_back(cur);
    return out;
}

// Materials a mesh uses (SF6 mesh header: name table + material index table)
static bool mesh_materials(const std::vector<uint8_t>& b, std::vector<std::string>& out) {
    if (b.size() < 152 || memcmp(b.data(), "MESH", 4) != 0) return false;
    auto r16 = [&](size_t o) { int16_t v; memcpy(&v, b.data() + o, 2); return v; };
    auto r64 = [&](size_t o) { int64_t v; memcpy(&v, b.data() + o, 8); return v; };
    const int16_t name_count = r16(20);
    const int64_t lods = r64(32), mat_idx = r64(112), name_offs = r64(144);
    if (name_count <= 0 || lods <= 0 || mat_idx <= 0 || name_offs <= 0) return false;
    if (size_t(lods) + 2 > b.size() || size_t(name_offs) + 8ull * name_count > b.size()) return false;
    const uint8_t mcount = b[size_t(lods) + 1];
    if (size_t(mat_idx) + 2ull * mcount > b.size()) return false;
    std::vector<std::string> names;
    for (int i = 0; i < name_count; i++) {
        int64_t o = r64(size_t(name_offs) + 8ull * i);
        if (o <= 0 || size_t(o) >= b.size()) return false;
        std::string s;
        for (size_t k = size_t(o); k < b.size() && b[k]; k++) s += char(b[k]);
        names.push_back(s);
    }
    for (int k = 0; k < mcount; k++) {
        int16_t idx = r16(size_t(mat_idx) + 2ull * k);
        if (idx < 0 || idx >= name_count) return false;
        out.push_back(names[idx]);
    }
    return true;
}

// Materials a material file defines (.mdf2.31: 100-byte material headers, name offset first)
static bool mdf2_materials(const std::vector<uint8_t>& b, std::vector<std::string>& out) {
    if (b.size() < 16 || memcmp(b.data(), "MDF\0", 4) != 0) return false;
    int16_t count; memcpy(&count, b.data() + 6, 2);
    const size_t H = 100;
    if (count < 0 || 16 + H * size_t(count) > b.size()) return false;
    for (int i = 0; i < count; i++) {
        int64_t o; memcpy(&o, b.data() + 16 + H * i, 8);
        if (o <= 0 || size_t(o) >= b.size()) return false;
        std::string s;
        for (size_t k = size_t(o); k + 1 < b.size(); k += 2) {
            uint16_t c = uint16_t(b[k] | (b[k + 1] << 8));
            if (!c) break;
            s += char(c);
        }
        out.push_back(s);
    }
    return true;
}

// What the game will find at a path: our pak first, then the game's paks by priority
struct GameFiles {
    PakWriter* writer = nullptr;
    std::vector<PakReader*> paks;
    bool exists(const std::string& p) const {
        uint64_t h = pak_path_hash(std::string_view(p));
        if (writer->find(h)) return true;
        for (auto* pk : paks) if (pk->find(h)) return true;
        return false;
    }
    std::vector<uint8_t> load(const std::string& p) const {
        uint64_t h = pak_path_hash(std::string_view(p));
        if (writer->find(h)) return writer->get(h);
        for (auto* pk : paks) if (auto* e = pk->find(h)) return pk->read(*e);
        return {};
    }
};

// "Product/Model/..../x.mesh" -> "natives/stm/product/model/..../x.mesh.230110883", or empty
static std::string resource_path(std::string ref, const std::unordered_map<std::string, std::string>& suffixes) {
    while (!ref.empty() && (ref[0] == '@' || ref[0] == '/')) ref.erase(0, 1);
    std::string low = str_lower(ref);
    if (low.compare(0, 8, "product/") != 0) return {};
    auto dot = low.rfind('.');
    auto sl = low.rfind('/');
    if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return {};
    auto it = suffixes.find(low.substr(dot + 1));
    if (it == suffixes.end() || it->second.empty()) return {};
    return "natives/stm/" + low + "." + it->second;
}

struct Ctx {
    GameFiles gf;
    const std::unordered_map<std::string, std::string>* suffixes = nullptr;
    const std::unordered_map<std::string, ModFileRef>* all_mod = nullptr;
};

// Why a mesh and its material file cannot be loaded together, or empty
static std::string pair_problem(const Ctx& c, const std::vector<uint8_t>& mesh, const std::vector<uint8_t>& mdf) {
    if (mesh.empty()) return "mesh missing";
    if (mdf.empty()) return "material file missing";
    std::vector<std::string> mm, dm;
    if (!mesh_materials(mesh, mm)) return "mesh not readable";
    if (!mdf2_materials(mdf, dm)) return "material file not readable";
    std::unordered_set<std::string> have(dm.begin(), dm.end());
    std::string missing;
    int n_missing = 0;
    for (auto& m : mm)
        if (!have.count(m)) { if (n_missing++ < 3) missing += (missing.empty() ? "" : ", ") + m; }
    if (n_missing) return std::to_string(n_missing) + " materials of the mesh not in its material file (" + missing + ")";
    for (auto& s : utf16_strings(mdf)) {
        if (!str_ends_with(str_lower(s), ".tex")) continue;
        std::string p = resource_path(s, *c.suffixes);
        if (!p.empty() && !c.gf.exists(p)) return "texture missing: " + s;
        if (!p.empty())
            if (auto* t = c.gf.writer->peek(pak_path_hash(std::string_view(p)))) {
                std::vector<uint8_t> head(t->begin(), t->begin() + std::min<size_t>(t->size(), 4096));
                if (texture_mip_check(head, false, nullptr)) return "texture with an inconsistent mip table: " + s;
            }
    }
    return {};
}

// Missing textures of a material file: taken from the mod when it ships them (a file of the mod the
// relocation left out), else, for a texture of the slot, from the original costume. Returns how many.
static int restore_missing_textures(Ctx& c, const std::vector<uint8_t>& mdf, const SlotInfo& slot) {
    int n = 0;
    const std::string sd = "/" + slot.fighter_dir + "/" + slot.new_folder + "/";
    const std::string od = "/" + slot.fighter_dir + "/" + slot.original_folder + "/";
    const std::string ss = slot.fighter_dir + "_" + slot.new_folder + "_";
    const std::string os = slot.fighter_dir + "_" + slot.original_folder + "_";
    for (auto& s : utf16_strings(mdf)) {
        if (!str_ends_with(str_lower(s), ".tex")) continue;
        std::string p = resource_path(s, *c.suffixes);
        if (p.empty() || c.gf.exists(p)) continue;
        std::vector<uint8_t> data;
        auto it = c.all_mod->find(p);
        if (it != c.all_mod->end()) data = it->second.read_data();
        else if (p.find(sd) != std::string::npos) {
            std::string v = p;
            v.replace(v.find(sd), sd.size(), od);
            auto k = v.find(ss);
            if (k != std::string::npos) v.replace(k, ss.size(), os);
            data = c.gf.load(v);
            if (!data.empty()) {
                std::string sv = "natives/stm/streaming/" + v.substr(12), sp = "natives/stm/streaming/" + p.substr(12);
                auto twin = c.gf.load(sv);
                if (!twin.empty() && !c.gf.exists(sp))
                    c.gf.writer->add_uncompressed(pak_path_hash(std::string_view(sp)), std::move(twin));
            }
        }
        if (data.empty()) continue;
        c.gf.writer->add_uncompressed(pak_path_hash(std::string_view(p)), std::move(data));
        n++;
    }
    return n;
}

static void put_file(PakWriter& w, const std::string& p, std::vector<uint8_t> data) {
    uint64_t h = pak_path_hash(std::string_view(p));
    if (w.find(h)) w.replace(h, std::move(data));
    else w.add_uncompressed(h, std::move(data));
}

// Finds a file of a mod by path, case-insensitively
static const ModFileRef* mod_file(const std::unordered_map<std::string, ModFileRef>& files, const std::string& low_path) {
    auto it = files.find(low_path);
    if (it != files.end()) return &it->second;
    for (auto& [p, mf] : files) if (str_lower(p) == low_path) return &mf;
    return nullptr;
}

// Checks one slot, repairing what can be repaired. Returns an empty string when the slot is loadable.
static std::string check_slot(Ctx& c, const SlotInfo& slot) {
    const std::string& fd = slot.fighter_dir;
    const std::string& old_f = slot.original_folder;
    const std::string& new_f = slot.new_folder;
    const std::string scene_p = "natives/stm/product/charparam/esf/" + fd + "/" + slot.scene_name + ".scn.20";
    auto scene = c.gf.writer->get(pak_path_hash(std::string_view(scene_p)));
    if (scene.empty()) return "slot scene missing";
    const std::string slot_dir = "/" + fd + "/" + new_f + "/", slot_stem = fd + "_" + new_f + "_",
                      slot_ud = "/battle" + new_f + "/";
    auto is_slot = [&](const std::string& p) {
        return p.find(slot_dir) != std::string::npos || p.find(slot_stem) != std::string::npos
            || p.find(slot_ud) != std::string::npos;
    };
    const std::string mesh_suf = c.suffixes->count("mesh") ? c.suffixes->at("mesh") : "230110883";
    const std::string mdf_suf = c.suffixes->count("mdf2") ? c.suffixes->at("mdf2") : "31";
    auto model_path = [&](const std::string& folder, const std::string& part, const std::string& tail) {
        return std::string(BASE_PFX) + "model/esf/" + fd + "/" + folder + "/" + part + "/"
             + fd + "_" + folder + "_" + part + tail;
    };

    // References of the scene, in order
    std::vector<std::string> refs;
    for (auto& s : utf16_strings(scene)) {
        std::string p = resource_path(s, *c.suffixes);
        if (!p.empty()) refs.push_back(p);
    }

    // 1. Files of the slot that the scene references: they must exist
    for (auto& p : refs) {
        if (!is_slot(p) || c.gf.exists(p)) continue;
        std::string v = p;
        auto k = v.find(slot_dir);
        if (k != std::string::npos) v.replace(k, slot_dir.size(), "/" + fd + "/" + old_f + "/");
        k = v.find(slot_stem);
        if (k != std::string::npos) v.replace(k, slot_stem.size(), fd + "_" + old_f + "_");
        auto data = (v != p) ? c.gf.load(v) : std::vector<uint8_t>();
        if (data.empty()) return "file missing: " + p;
        put_file(*c.gf.writer, p, std::move(data));
        printf("      check: missing %s taken from the original costume\n", p.c_str());
    }

    // 2. Each mesh with the material file that follows it
    for (size_t i = 0; i < refs.size(); i++) {
        const std::string& mesh_p = refs[i];
        if (!str_ends_with(mesh_p, (".mesh." + mesh_suf).c_str())) continue;
        std::string mdf_p;
        for (size_t j = i + 1; j < refs.size(); j++)
            if (str_ends_with(refs[j], (".mdf2." + mdf_suf).c_str())) { mdf_p = refs[j]; break; }
        if (mdf_p.empty() || (!is_slot(mesh_p) && !is_slot(mdf_p))) continue;
        std::string why = pair_problem(c, c.gf.load(mesh_p), c.gf.load(mdf_p));
        if (why.empty()) continue;
        if (why.compare(0, 16, "texture missing:") == 0) {
            int n = restore_missing_textures(c, c.gf.load(mdf_p), slot);
            std::string again = pair_problem(c, c.gf.load(mesh_p), c.gf.load(mdf_p));
            if (again.empty()) {
                printf("      check: %d missing textures of %s restored\n", n, mdf_p.c_str());
                continue;
            }
            why = again;
        }

        // Part of the slot: "<fd>/<new_f>/<part>/..."
        auto sd = mesh_p.find(slot_dir);
        if (sd == std::string::npos) return mesh_p + ": " + why;
        std::string part = mesh_p.substr(sd + slot_dir.size());
        part = part.substr(0, part.find('/'));
        const bool mdf_is_slot = is_slot(mdf_p);

        // Candidates: the mod's shared 000/ part, then the original costume's part
        struct Cand { const char* label; std::vector<uint8_t> mesh, mdf; };
        std::vector<Cand> cands;
        if (auto* mc = slot.mod_costume) {
            if (auto* mm = mod_file(mc->files_shared, model_path("000", part, ".mesh." + mesh_suf))) {
                Cand k{"the mod's shared 000/ part", mm->read_data(), {}};
                auto* md = mod_file(mc->files_shared, model_path("000", part, "_v00.mdf2." + mdf_suf));
                k.mdf = md ? md->read_data() : c.gf.load(model_path("000", part, "_v00.mdf2." + mdf_suf));
                // The mod's shared textures were relocated into the slot folder (phase 1c)
                std::unordered_set<std::string> keys;
                const std::string base = std::string(BASE_PFX) + "model/esf/" + fd + "/000/";
                for (auto& [pth, mf] : mc->files_shared) {
                    std::string low = str_lower(pth);
                    auto dot = low.find(".tex.");
                    if (low.compare(0, base.size(), base) == 0 && dot != std::string::npos)
                        keys.insert(low.substr(base.size(), dot + 4 - base.size()));
                }
                if (!keys.empty() && !k.mdf.empty()) patch_mdf2_minimal(k.mdf, fd, "000", new_f, keys);
                cands.push_back(std::move(k));
            }
        }
        cands.push_back({"the original costume's part", c.gf.load(model_path(old_f, part, ".mesh." + mesh_suf)),
                         c.gf.load(model_path(old_f, part, "_v00.mdf2." + mdf_suf))});
        bool fixed = false;
        for (auto& k : cands) {
            if (!mdf_is_slot) k.mdf = c.gf.load(mdf_p);   // the scene keeps its material file
            if (!pair_problem(c, k.mesh, k.mdf).empty()) continue;
            put_file(*c.gf.writer, mesh_p, std::move(k.mesh));
            if (mdf_is_slot) put_file(*c.gf.writer, mdf_p, std::move(k.mdf));
            printf("      check: part %s (%s) rebuilt from %s\n", part.c_str(), why.c_str(), k.label);
            fixed = true;
            break;
        }
        if (!fixed) return "part " + part + ": " + why;
    }
    return {};
}

}  // namespace slotcheck


// ============================================================================
// Costume mods folder scanning
// ============================================================================

static const char* CHARACTER_NAMES[] = {
    "Ryu", "Luke", "Kimberly", "Chun-Li", "Manon", "Zangief", "JP",
    "Dhalsim", "Cammy", "Ken", "Dee Jay", "Lily", "A.K.I", "Rashid",  // pas de point final : Windows le retire du nom de dossier
    "Blanka", "Juri", "Marisa", "Guile", "Ed", "E. Honda", "Jamie",
    "Akuma", "Sagat", "M. Bison", "Terry", "Mai", "Elena", "C. Viper",
    "Alex", "Ingrid", "Yasmine",
};
static const int N_CHARACTERS = 31;

static void ensure_costume_mods_dirs(const std::string& game_dir) {
    std::string base = game_dir + "\\reframework\\costume_mods";
    // Use wide API for directory creation (names with dots/spaces)
    auto mkdirW = [](const std::string& p) {
        std::wstring wp;
        for (char c : p) wp += (wchar_t)(unsigned char)c;
        CreateDirectoryW(wp.c_str(), nullptr);
    };
    mkdirW(base);
    for (int i = 0; i < N_CHARACTERS; i++) {
        mkdirW(base + "\\" + CHARACTER_NAMES[i]);
    }
}

struct FolderModInfo {
    std::string rel_path;  // e.g. "Ryu/Vagrant"
    std::string abs_path;  // full disk path
};

// Find first 'natives' directory within max_depth levels
static std::wstring find_natives_dir(const std::wstring& folder, int max_depth = 3) {
    if (max_depth <= 0) return {};
    WIN32_FIND_DATAW fd;
    std::wstring pat = folder + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    // First pass: look for 'natives' directly
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        std::wstring name(fd.cFileName);
        std::wstring low(name);
        for (auto& c : low) c = towlower(c);
        if (low == L"natives") { FindClose(h); return folder + L"\\" + name; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // Second pass: recurse into subdirs
    if (max_depth > 1) {
        h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return {};
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 ||
                (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            auto r = find_natives_dir(folder + L"\\" + fd.cFileName, max_depth - 1);
            if (!r.empty()) { FindClose(h); return r; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return {};
}

// Recursive directory walk (wide API, \\?\ support)
static void walk_dir_w(const std::wstring& dir,
                       std::vector<std::pair<std::wstring, std::wstring>>& files) {
    // files: (full_path, relative_from_dir)
    WIN32_FIND_DATAW fd;
    std::wstring pat = dir + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            walk_dir_w(full, files);
        else
            files.push_back({full, std::wstring(fd.cFileName)});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Scan a natives-based folder mod. Returns {pak_path: ModFileRef}.
// Non-costume files (outside model/esf) are ignored. Old tex suffixes upgraded.
static std::unordered_map<std::string, ModFileRef> scan_folder_mod(
    const std::string& mod_dir, const VanillaIndex& inv, int& ignored_count, int& upgraded_count) {
    ignored_count = 0; upgraded_count = 0;
    std::unordered_map<std::string, ModFileRef> result;

    std::string tex_ver;
    { auto it = inv.suffixes.find("tex"); if (it != inv.suffixes.end()) tex_ver = it->second; }

    // Walk the directory tree
    std::vector<std::pair<std::wstring, std::wstring>> files_found;
    // Convert mod_dir to wide with \\?\ for long paths
    std::wstring mod_dir_w;
    if (mod_dir.size() > 2 && mod_dir[1] == ':') {
        mod_dir_w = L"\\\\?\\";
        for (char c : mod_dir) mod_dir_w += (wchar_t)(unsigned char)c;
    } else {
        for (char c : mod_dir) mod_dir_w += (wchar_t)(unsigned char)c;
    }

    // Recursive walk collecting all files with their full paths
    struct WalkEntry { std::string full_path; std::string rel_path; };
    std::vector<WalkEntry> all_files;

    std::function<void(const std::wstring&, const std::string&)> walk_rec;
    walk_rec = [&](const std::wstring& dir, const std::string& rel_prefix) {
        WIN32_FIND_DATAW fd;
        std::wstring pat = dir + L"\\*";
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 ||
                (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            std::string name = w2a(fd.cFileName);
            std::string rel = rel_prefix.empty() ? name : rel_prefix + "/" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                walk_rec(dir + L"\\" + fd.cFileName, rel);
            } else {
                // Convert full path back to narrow for ModFileRef
                std::string full_narrow = mod_dir + "\\" + rel;
                for (auto& c : full_narrow) if (c == '/') c = '\\';
                all_files.push_back({full_narrow, rel});
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    // Walk only this unit's natives tree: sibling folders can be other options of a bundle.
    std::wstring natives_name;
    {
        WIN32_FIND_DATAW fdn;
        HANDLE hn = FindFirstFileW((mod_dir_w + L"\\*").c_str(), &fdn);
        if (hn != INVALID_HANDLE_VALUE) {
            do {
                if (!(fdn.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::wstring n(fdn.cFileName), l(n);
                for (auto& c : l) c = towlower(c);
                if (l == L"natives") { natives_name = n; break; }
            } while (FindNextFileW(hn, &fdn));
            FindClose(hn);
        }
    }
    if (natives_name.empty()) walk_rec(mod_dir_w, "");
    else walk_rec(mod_dir_w + L"\\" + natives_name, w2a(natives_name.c_str()));

    // First pass: collect all pak_paths + full_paths, upgrade tex
    struct PakItem { std::string pak_path; std::string full_path; };
    std::vector<PakItem> pak_items;
    for (auto& [full_path, rel] : all_files) {
        std::string low_rel = str_lower(rel);
        for (auto& c : low_rel) if (c == '\\') c = '/';
        auto idx = low_rel.find("natives/stm/");
        if (idx == std::string::npos) continue;
        std::string pak_path = low_rel.substr(idx);
        for (int oi = 0; oi < N_OLD_TEX; oi++) {
            std::string old_suf = std::string(".tex.") + OLD_TEX_SUFFIXES[oi];
            if (str_ends_with(pak_path, old_suf.c_str())) {
                pak_path = pak_path.substr(0, pak_path.size() - strlen(OLD_TEX_SUFFIXES[oi]))
                         + tex_ver;
                upgraded_count++;
                break;
            }
        }
        pak_items.push_back({pak_path, full_path});
    }

    // Find costume folder prefixes from model/esf files (e.g. "esf010_002_")
    std::set<std::string> costume_prefixes;
    for (auto& item : pak_items) {
        std::string pfd, pfolder;
        if (parse_model_folder(item.pak_path, pfd, pfolder) && pfolder != "000")
            costume_prefixes.insert(pfd + "_" + pfolder + "_");
    }

    // Second pass: classify
    int extra_count = 0;
    for (auto& item : pak_items) {
        if (item.pak_path.find("/product/model/esf/") != std::string::npos
            || item.pak_path.find("/streaming/product/model/esf/") != std::string::npos) {
            ModFileRef mf; mf.disk_path = item.full_path;
            result[item.pak_path] = mf;
        } else {
            auto sl = item.pak_path.rfind('/');
            std::string fn = (sl != std::string::npos) ? item.pak_path.substr(sl+1) : item.pak_path;
            bool matched = false;
            for (auto& pfx : costume_prefixes) {
                if (fn.find(pfx) != std::string::npos) { matched = true; break; }
            }
            if (matched) {
                ModFileRef mf; mf.disk_path = item.full_path;
                result[item.pak_path] = mf;
                extra_count++;
            } else {
                ignored_count++;
            }
        }
    }
    if (extra_count)
        printf("    %d extra-model files attached by name\n", extra_count);
    return result;
}

// Content-based identity for a folder mod (matches identify_mod_pak logic)
static std::string identify_folder_mod(
    const std::unordered_map<std::string, ModFileRef>& mod_files) {
    struct E { uint64_t hash; int64_t cs; };
    std::vector<E> entries;
    for (auto& [path, mf] : mod_files) {
        uint64_t h = pak_path_hash(std::string_view(path));
        uint64_t sz = 0; uint64_t mt = 0;
        if (!mf.disk_path.empty()) file_stat_long(mf.disk_path, sz, mt);
        entries.push_back({h, (int64_t)sz});
    }
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.cs < b.cs);
    });
    std::vector<uint8_t> buf(entries.size() * 16);
    for (size_t i = 0; i < entries.size(); i++) {
        memcpy(buf.data() + i*16, &entries[i].hash, 8);
        memcpy(buf.data() + i*16 + 8, &entries[i].cs, 8);
    }
    return md5_hex(buf.data(), buf.size()).substr(0, 16);
}

// Process one mod folder (pak or natives). Returns true if mod found.
// ============================================================================
// Archives dropped in costume_mods (.zip / .7z / .rar)
// ============================================================================
// An archive is extracted once into costume_mods\.cache\<key>\ and then handled exactly like a
// folder. The key comes from the archive's name, size and date, so the extraction is reused until
// the archive changes; cache folders whose archive is gone are deleted at the next regeneration.
// Only what the loader can use is extracted: natives trees, paks and nested archives.

static std::string g_archive_cache_root;
static std::set<std::string> g_archive_cache_used;

static bool is_archive_name(const std::string& name) {
    auto l = str_lower(name);
    return str_ends_with(l, ".zip") || str_ends_with(l, ".7z") || str_ends_with(l, ".rar");
}

static bool archive_keep_entry(const std::wstring& p) {
    std::wstring l(p);
    for (auto& c : l) c = towlower(c);
    if (l.rfind(L"natives/", 0) == 0 || l.find(L"/natives/") != std::wstring::npos) return true;
    // Fluffy's modinfo.ini carries the option names and the add-on links
    if (l == L"modinfo.ini" || (l.size() > 12 && l.compare(l.size() - 12, 12, L"/modinfo.ini") == 0))
        return true;
    static const wchar_t* exts[] = { L".pak", L".zip", L".7z", L".rar" };
    for (auto e : exts) {
        size_t n = wcslen(e);
        if (l.size() >= n && l.compare(l.size() - n, n, e) == 0) return true;
    }
    return false;
}

static std::string cache_safe_name(const std::string& n) {
    std::string s;
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
               || c == '.' || c == '-' || c == '_';
        s += ok ? c : '_';
        if (s.size() >= 24) break;
    }
    return s;
}

static uint32_t fnv1a32(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

static std::wstring a2w_path(const std::string& s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? size_t(n - 1) : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static void delete_tree_w(const std::wstring& dir_in) {
    std::wstring dir = (dir_in.size() > 2 && dir_in[1] == L':') ? L"\\\\?\\" + dir_in : dir_in;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 ||
                (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            std::wstring full = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_tree_w(full);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Returns the folder holding the archive's content, or "" when the archive cannot be used.
static std::string ensure_archive_extracted(const std::string& archive_path) {
    uint64_t sz = 0, mt = 0;
    if (!file_stat_long(archive_path, sz, mt)) return {};
    auto sl = archive_path.rfind('\\');
    std::string fname = (sl != std::string::npos) ? archive_path.substr(sl + 1) : archive_path;
    char idbuf[64], key[96];
    snprintf(idbuf, sizeof(idbuf), ":%llu:%llu", (unsigned long long)sz, (unsigned long long)mt);
    // "v2": bump when the extraction filter changes, so older cache folders are not reused
    snprintf(key, sizeof(key), "%s_%08x", cache_safe_name(fname).c_str(),
             fnv1a32(std::string("v2:") + fname + idbuf));
    g_archive_cache_used.insert(key);
    std::string final_dir = g_archive_cache_root + "\\" + key;
    if (GetFileAttributesA((final_dir + "\\.complete").c_str()) != INVALID_FILE_ATTRIBUTES) {
        printf("    archive: %s (already extracted)\n", fname.c_str());
        return final_dir;
    }
    if (GetFileAttributesA(g_archive_cache_root.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryA(g_archive_cache_root.c_str(), nullptr);
        SetFileAttributesA(g_archive_cache_root.c_str(), FILE_ATTRIBUTE_HIDDEN);
    }
    std::wstring arc_w = a2w_path(archive_path);
    std::wstring tmp_w = a2w_path(final_dir + ".partial");
    std::wstring fin_w = a2w_path(final_dir);
    delete_tree_w(tmp_w);
    delete_tree_w(fin_w);
    ArchiveKind kind = archive_sniff(arc_w);
    if (kind == ArchiveKind::None) {
        printf("    archive: %s is not a zip, 7z or rar file, ignored\n", fname.c_str());
        return {};
    }
    auto t0 = std::chrono::high_resolution_clock::now();
    ArchiveResult r = archive_extract(arc_w, tmp_w, archive_keep_entry);
    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
    if (!r.ok) {
        printf("    archive: %s CANNOT be used (%s): %s\n",
               fname.c_str(), archive_kind_name(kind), r.error.c_str());
        delete_tree_w(tmp_w);
        return {};
    }
    HANDLE hm = CreateFileW((tmp_w + L"\\.complete").c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hm != INVALID_HANDLE_VALUE) CloseHandle(hm);
    if (!MoveFileExW(tmp_w.c_str(), fin_w.c_str(), 0)) {
        printf("    archive: %s extracted but its cache folder could not be finalised (error %lu)\n",
               fname.c_str(), (unsigned long)GetLastError());
        delete_tree_w(tmp_w);
        return {};
    }
    printf("    archive: %s (%s) extracted in %.0f ms: %d files kept (%.1f MB), %d skipped\n",
           fname.c_str(), archive_kind_name(kind), ms, r.files_written,
           r.bytes_written / 1048576.0, r.files_skipped);
    return final_dir;
}

static void purge_stale_archive_cache() {
    if (g_archive_cache_root.empty()) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_archive_cache_root + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> stale;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        if (!g_archive_cache_used.count(n)) stale.push_back(n);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (auto& n : stale) {
        delete_tree_w(a2w_path(g_archive_cache_root + "\\" + n));
        printf("  archive cache: removed %s (archive no longer installed)\n", n.c_str());
    }
}

// ============================================================================
// Mod units: one costume source = one natives tree or one .pak
// ============================================================================
// A folder or an archive can hold several options (a bundle). Each natives tree and each .pak is
// handled as its own costume, so every option becomes its own slot; partial options (hair only,
// an accessory) are then turned away by the usual partial-costume rule.

struct ModUnit {
    std::string dir;   // folder holding the natives tree, or the .pak
    std::string pak;   // .pak file name, empty for a natives tree
    std::string rel;   // path shown in the log
};

struct ScanOutputs {
    const VanillaIndex& inv;
    std::vector<std::pair<std::string, std::vector<ModCostume>>>& mods_out;
    std::unordered_map<std::string, ModFileRef>& all_mod_out;
    std::vector<std::unique_ptr<PakReader>>& pak_readers_out;
    std::vector<FolderModInfo>& infos_out;
    std::unordered_map<uint64_t, std::string>& upgraded_tex_out;
};

// Finds every costume under `dir`. Archives met on the way are extracted and searched too, with
// two levels of nesting at most; folders are searched four levels deep.
static void collect_mod_units(const std::string& dir, const std::string& rel,
                              int depth, int nest, std::vector<ModUnit>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> subdirs, paks, archives;
    bool has_natives = false;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (str_lower(name) == "natives") has_natives = true;
            else if (name[0] != '.') subdirs.push_back(name);
        } else if (str_ends_with(str_lower(name), ".pak")) {
            paks.push_back(name);
        } else if (is_archive_name(name)) {
            archives.push_back(name);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(subdirs.begin(), subdirs.end());
    std::sort(paks.begin(), paks.end());
    std::sort(archives.begin(), archives.end());

    if (has_natives) out.push_back({dir, "", rel});
    for (auto& p : paks) out.push_back({dir, p, rel + "/" + p});
    if (nest < 2) {
        for (auto& a : archives) {
            std::string ex = ensure_archive_extracted(dir + "\\" + a);
            if (!ex.empty()) collect_mod_units(ex, rel + "/" + a, 0, nest + 1, out);
        }
    }
    if (depth < 4)
        for (auto& sd : subdirs)
            collect_mod_units(dir + "\\" + sd, rel + "/" + sd, depth + 1, nest, out);
}

// ============================================================================
// Units are loaded first, then combined: add-on options (Fluffy "addonfor", or parts of the same
// bundle) are merged onto the option they complete, the way Fluffy would install them together.
// ============================================================================

struct UnitFiles {
    ModUnit u;
    int source = 0;                      // units coming from the same folder or archive share it
    std::string name, addonfor, bundle;  // from modinfo.ini (Fluffy), may be empty
    std::string mod_id, info_path;
    std::unordered_map<std::string, ModFileRef> files;
    std::vector<ModCostume> costumes;    // empty: partial option, candidate add-on
    std::set<std::string> targets;       // costume folders touched, e.g. "esf001/001"
};
static std::vector<UnitFiles> g_units;
static int g_unit_source = 0;

static std::string trim_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

static bool eq_ci(const std::string& a, const std::string& b) {
    return str_lower(trim_ws(a)) == str_lower(trim_ws(b));
}

static uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

static void read_modinfo(const std::string& dir, UnitFiles& uf) {
    FILE* f = fopen((dir + "\\modinfo.ini").c_str(), "rb");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string l(line);
        if (l.size() >= 3 && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB
            && (unsigned char)l[2] == 0xBF) l.erase(0, 3);
        auto eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = str_lower(trim_ws(l.substr(0, eq)));
        std::string v = trim_ws(l.substr(eq + 1));
        if (k == "name") uf.name = v;
        else if (k == "addonfor") uf.addonfor = v;
        else if (k == "nameasbundle") uf.bundle = v;
    }
    fclose(f);
}

static void print_costumes(const std::vector<ModCostume>& costumes) {
    for (auto& mc : costumes)
        printf("    -> %s costume %d (folder %s): %zu files\n",
               mc.fighter_dir.c_str(), mc.original_costume_no,
               mc.original_folder.c_str(), mc.files_in_folder.size());
}

static bool load_unit(const ModUnit& u, int source, ScanOutputs& o) {
    const VanillaIndex& inv = o.inv;
    UnitFiles uf;
    uf.u = u;
    uf.source = source;
    read_modinfo(u.dir, uf);

    if (!u.pak.empty()) {
        std::string pak_path = u.dir + "\\" + u.pak;
        printf("    pak: %s\n", u.pak.c_str());
        auto reader = std::make_unique<PakReader>();
        if (!reader->open(pak_path.c_str())) {
            printf("    WARN: cannot open %s, skipping\n", u.pak.c_str());
            return false;
        }
        printf("    %zu entries\n", reader->entry_count());
        uf.mod_id = identify_mod_pak(pak_path.c_str());
        std::vector<uint64_t> unknown;
        std::unordered_map<uint64_t, std::string> upgraded_tex;
        scan_mod_pak(*reader, inv, uf.files, unknown, upgraded_tex);
        o.upgraded_tex_out.insert(upgraded_tex.begin(), upgraded_tex.end());
        printf("    resolved: %zu paths, unattributed: %zu\n", uf.files.size(), unknown.size());
        o.pak_readers_out.push_back(std::move(reader));   // its files stay readable until the end
        uf.info_path = pak_path;
    } else {
        int ignored = 0, upgraded = 0;
        uf.files = scan_folder_mod(u.dir, inv, ignored, upgraded);
        if (ignored)
            printf("    %d non-costume files ignored "
                   "(folder mods cannot provide global files)\n", ignored);
        printf("    %zu costume files\n", uf.files.size());
        if (uf.files.empty()) {
            printf("    WARN: no costume files found, skipping\n");
            return false;
        }
        uf.mod_id = identify_folder_mod(uf.files);
        uf.info_path = u.dir;
    }
    printf("    mod_id: %s\n", uf.mod_id.c_str());
    g_mod_names[uf.mod_id] = uf.name.empty() ? uf.info_path : uf.name;
    if (!uf.name.empty()) {
        std::string extra = uf.addonfor.empty() ? std::string() : "  (add-on for " + uf.addonfor + ")";
        printf("    name: %s%s\n", uf.name.c_str(), extra.c_str());
    }
    uf.costumes = detect_mod_costumes(uf.files, inv);
    print_costumes(uf.costumes);
    for (auto& [p, mf] : uf.files) {
        std::string fd, folder;
        if (parse_model_folder(p, fd, folder) && folder != "000") uf.targets.insert(fd + "/" + folder);
    }
    g_units.push_back(std::move(uf));
    return true;
}

static bool process_units(const std::vector<ModUnit>& units, const std::string& rel_path,
                          ScanOutputs& o) {
    if (units.empty()) {
        printf("    no natives folder, .pak or usable archive found, skipping\n");
        return false;
    }
    if (units.size() > 1) printf("    %zu options in this mod\n", units.size());
    int source = ++g_unit_source;
    bool any = false;
    for (auto& u : units) {
        if (units.size() > 1 || u.rel != rel_path) printf("  option: %s\n", u.rel.c_str());
        if (load_unit(u, source, o)) any = true;
    }
    return any;
}

static bool scan_one_mod_folder(const std::string& mod_dir, const std::string& rel_path,
                                ScanOutputs& o) {
    std::vector<ModUnit> units;
    collect_mod_units(mod_dir, rel_path, 0, 0, units);
    return process_units(units, rel_path, o);
}

static bool shares_target(const UnitFiles& a, const UnitFiles& b) {
    for (auto& t : a.targets) if (b.targets.count(t)) return true;
    return false;
}

// Turns the loaded units into costumes. Complete options are used as they are. A partial option
// is matched to the option it completes, most explicit rule first: its Fluffy "addonfor" name,
// then the same Fluffy bundle, then the same folder or archive; in every case it must touch the
// same costume folder (except for the explicit "addonfor" name). Each base then gets one variant
// per add-on, plus one with all its add-ons when it has several.
static void finalize_units(ScanOutputs& o) {
    for (auto& uf : g_units) {
        if (uf.costumes.empty()) continue;
        o.mods_out.push_back({uf.mod_id, uf.costumes});
        o.all_mod_out.insert(uf.files.begin(), uf.files.end());
        o.infos_out.push_back({uf.u.rel, uf.info_path});
    }

    std::map<size_t, std::vector<size_t>> addons_of;
    for (size_t i = 0; i < g_units.size(); ++i) {
        const auto& a = g_units[i];
        if (!a.costumes.empty() || a.files.empty()) continue;
        std::vector<size_t> bases;
        for (int rule = 0; rule < 3 && bases.empty(); ++rule) {
            for (size_t j = 0; j < g_units.size(); ++j) {
                const auto& b = g_units[j];
                if (b.costumes.empty()) continue;
                bool hit = false;
                if (rule == 0) hit = !a.addonfor.empty() && eq_ci(a.addonfor, b.name);
                if (rule == 1) hit = !a.bundle.empty() && eq_ci(a.bundle, b.bundle) && shares_target(a, b);
                if (rule == 2) hit = a.source == b.source && shares_target(a, b);
                if (hit) bases.push_back(j);
            }
        }
        if (bases.empty()) {
            printf("\n  add-on %s: no costume it can complete was found, ignored\n", a.u.rel.c_str());
            continue;
        }
        for (auto j : bases) addons_of[j].push_back(i);
    }

    for (auto& [j, adds] : addons_of) {
        const auto& b = g_units[j];
        std::vector<std::vector<size_t>> variants;
        for (auto a : adds) variants.push_back({a});
        if (adds.size() >= 2) variants.push_back(adds);
        for (auto& v : variants) {
            auto merged = b.files;
            std::string id_src = b.mod_id;
            std::string label = b.name.empty() ? b.u.rel : b.name;
            for (auto a : v) {
                const auto& ad = g_units[a];
                for (auto& [pth, mf] : ad.files) merged[pth] = mf;
                id_src += "+" + ad.mod_id;
                label += " + " + (ad.name.empty() ? ad.u.rel : ad.name);
            }
            char idb[24];
            snprintf(idb, sizeof(idb), "%016llx", (unsigned long long)fnv1a64(id_src));
            printf("\n  variant: %s\n", label.c_str());
            auto costumes = detect_mod_costumes(merged, o.inv);
            print_costumes(costumes);
            if (costumes.empty()) continue;
            printf("    mod_id: %s\n", idb);
            o.mods_out.push_back({idb, std::move(costumes)});
            for (auto a : v) o.all_mod_out.insert(g_units[a].files.begin(), g_units[a].files.end());
            o.infos_out.push_back({b.u.rel + " (with add-ons)", b.info_path});
        }
    }
    g_units.clear();
}

// Scan <game_dir>/reframework/costume_mods/<Character>/<CostumeName>/
// Also handles mods placed directly in <Character>/ (no sub-folder).
// Creates the directory structure if missing.
static void scan_costume_mods(
    const std::string& game_dir,
    const VanillaIndex& inv,
    std::vector<std::pair<std::string, std::vector<ModCostume>>>& mods_out,
    std::unordered_map<std::string, ModFileRef>& all_mod_out,
    std::vector<std::unique_ptr<PakReader>>& pak_readers_out,
    std::vector<FolderModInfo>& infos_out,
    std::unordered_map<uint64_t, std::string>& upgraded_tex_out) {

    ensure_costume_mods_dirs(game_dir);

    std::string base_dir = game_dir + "\\reframework\\costume_mods";
    if (GetFileAttributesA(base_dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    g_archive_cache_root = base_dir + "\\.cache";
    g_archive_cache_used.clear();
    g_units.clear();
    g_unit_source = 0;

    ScanOutputs o{ inv, mods_out, all_mod_out, pak_readers_out, infos_out, upgraded_tex_out };

    printf("\nScanning costume_mods in %s...\n", base_dir.c_str());

    WIN32_FIND_DATAA fd_char;
    HANDLE hc = FindFirstFileA((base_dir + "\\*").c_str(), &fd_char);
    if (hc == INVALID_HANDLE_VALUE) return;

    // Character folders, plus archives dropped straight into costume_mods
    std::vector<std::pair<std::string, std::string>> char_dirs; // (name, abs_path)
    std::vector<std::string> root_archives;
    do {
        if (fd_char.cFileName[0] == '.') continue;
        std::string name = fd_char.cFileName;
        if (fd_char.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            char_dirs.push_back({name, base_dir + "\\" + name});
        else if (is_archive_name(name))
            root_archives.push_back(name);
    } while (FindNextFileA(hc, &fd_char));
    FindClose(hc);
    std::sort(char_dirs.begin(), char_dirs.end());
    std::sort(root_archives.begin(), root_archives.end());

    for (auto& a : root_archives) {
        printf("\n  archive: %s\n", a.c_str());
        std::vector<ModUnit> units;
        std::string ex = ensure_archive_extracted(base_dir + "\\" + a);
        if (!ex.empty()) collect_mod_units(ex, a, 0, 1, units);
        process_units(units, a, o);
    }

    for (auto& [char_name, char_dir] : char_dirs) {
        WIN32_FIND_DATAA fd_cos;
        HANDLE hco = FindFirstFileA((char_dir + "\\*").c_str(), &fd_cos);
        if (hco == INVALID_HANDLE_VALUE) continue;

        std::vector<std::pair<std::string, std::string>> sub_dirs;
        std::vector<std::string> archives, direct_paks;
        bool has_direct_natives = false;
        do {
            if (fd_cos.cFileName[0] == '.') continue;
            std::string name = fd_cos.cFileName;
            if (fd_cos.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (str_lower(name) == "natives")
                    has_direct_natives = true;
                else
                    sub_dirs.push_back({name, char_dir + "\\" + name});
            } else if (str_ends_with(str_lower(name), ".pak")) {
                direct_paks.push_back(name);
            } else if (is_archive_name(name)) {
                archives.push_back(name);
            }
        } while (FindNextFileA(hco, &fd_cos));
        FindClose(hco);
        std::sort(sub_dirs.begin(), sub_dirs.end());
        std::sort(archives.begin(), archives.end());
        std::sort(direct_paks.begin(), direct_paks.end());

        // Costume folders (unpacked by the user, or Fluffy-style folders)
        for (auto& [cos_name, cos_dir] : sub_dirs) {
            std::string rel = char_name + "/" + cos_name;
            printf("\n  folder: %s\n", rel.c_str());
            scan_one_mod_folder(cos_dir, rel, o);
        }

        // Archives left as downloaded (.zip / .7z / .rar)
        for (auto& a : archives) {
            std::string rel = char_name + "/" + a;
            printf("\n  archive: %s\n", rel.c_str());
            std::vector<ModUnit> units;
            std::string ex = ensure_archive_extracted(char_dir + "\\" + a);
            if (!ex.empty()) collect_mod_units(ex, rel, 0, 1, units);
            process_units(units, rel, o);
        }

        // A mod unpacked straight into the character folder (no costume sub-folder)
        if (has_direct_natives || !direct_paks.empty()) {
            printf("\n  folder: %s (mod directly in character dir)\n", char_name.c_str());
            std::vector<ModUnit> units;
            if (has_direct_natives) units.push_back({char_dir, "", char_name});
            for (auto& p : direct_paks) units.push_back({char_dir, p, char_name + "/" + p});
            process_units(units, char_name, o);
        }
    }

    finalize_units(o);
    purge_stale_archive_cache();
}

// ============================================================================
// Fingerprint (paks + costume_mods folders)
// ============================================================================

static std::string compute_fingerprint(const std::vector<PakInfo>& mod_paks,
                                       const std::vector<FolderModInfo>& folder_infos = {}) {
    std::string fp = std::string("loader=") + LOADER_BUILD_ID;
    for (auto& pi : mod_paks) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(pi.path.c_str(), GetFileExInfoStandard, &fa)) continue;
        uint64_t sz = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        uint64_t mt = (uint64_t(fa.ftLastWriteTime.dwHighDateTime) << 32)
                    | fa.ftLastWriteTime.dwLowDateTime;
        auto sl = pi.path.rfind('\\');
        std::string name = (sl != std::string::npos) ? pi.path.substr(sl+1) : pi.path;
        if (!fp.empty()) fp += ";";
        char buf[256]; sprintf(buf, "%s:%llu:%llu", name.c_str(),
                               (unsigned long long)sz, (unsigned long long)mt);
        fp += buf;
    }
    // Include costume_mods folder files in fingerprint
    for (auto& fi : folder_infos) {
        // A loose file (archive or pak dropped in a character folder): its own size and date
        {
            WIN32_FILE_ATTRIBUTE_DATA fa0;
            if (GetFileAttributesExA(fi.abs_path.c_str(), GetFileExInfoStandard, &fa0)
                && !(fa0.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                uint64_t sz0 = (uint64_t(fa0.nFileSizeHigh) << 32) | fa0.nFileSizeLow;
                uint64_t mt0 = (uint64_t(fa0.ftLastWriteTime.dwHighDateTime) << 32)
                             | fa0.ftLastWriteTime.dwLowDateTime;
                char b0[640];
                snprintf(b0, sizeof(b0), ";%s:%llu:%llu", fi.rel_path.c_str(),
                         (unsigned long long)sz0, (unsigned long long)mt0);
                fp += b0;
                continue;
            }
        }
        // Walk all files in abs_path, include relative path + size + mtime
        std::function<void(const std::string&, const std::string&)> walk;
        walk = [&](const std::string& dir, const std::string& prefix) {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            std::vector<std::string> entries;
            do {
                if (fd.cFileName[0] == '.') continue;
                entries.push_back(fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            std::sort(entries.begin(), entries.end());
            for (auto& e : entries) {
                std::string full = dir + "\\" + e;
                std::string rel = prefix.empty() ? fi.rel_path + "/" + e
                                                 : prefix + "/" + e;
                WIN32_FILE_ATTRIBUTE_DATA fa2;
                if (!GetFileAttributesExA(full.c_str(), GetFileExInfoStandard, &fa2)) continue;
                if (fa2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    walk(full, rel);
                } else {
                    uint64_t sz = (uint64_t(fa2.nFileSizeHigh) << 32) | fa2.nFileSizeLow;
                    uint64_t mt = (uint64_t(fa2.ftLastWriteTime.dwHighDateTime) << 32)
                                | fa2.ftLastWriteTime.dwLowDateTime;
                    if (!fp.empty()) fp += ";";
                    char buf[512]; sprintf(buf, "%s:%llu:%llu", rel.c_str(),
                                           (unsigned long long)sz, (unsigned long long)mt);
                    fp += buf;
                }
            }
        };
        walk(fi.abs_path, "");
    }
    return fp;
}

// ============================================================================
// costume_loader_run  (entry point for exe and DLL)
// ============================================================================

int costume_loader_run(const wchar_t* game_dir_w, const wchar_t* base_pak_override) {
    auto t0 = std::chrono::high_resolution_clock::now();

    std::string game_dir = w2a(game_dir_w);
    if (game_dir.empty()) return 1;

    // Derive paths
    std::string data_dir = game_dir + "\\reframework\\data\\SF6_Costumes_Data\\loader";
    std::string tsv_path = data_dir + "\\vanilla_costume_index.tsv";
    std::string static_dir = data_dir + "\\static";
    std::string registry_dir = game_dir + "\\reframework\\data\\SF6_Costumes_Data";
    std::string pak_dir = game_dir;
    std::string out_dir = game_dir;

    // Silent exit if loader data dir doesn't exist (MatchScout-only install)
    if (GetFileAttributesA(data_dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        return 0;

    log_open(game_dir);
    logf("=== SF6 Costume Slot Loader ===\n");

    // Base pak path
    std::string base_path;
    if (base_pak_override && base_pak_override[0]) {
        base_path = w2a(base_pak_override);
    } else {
        // Check env var
        char env_buf[MAX_PATH];
        DWORD env_n = GetEnvironmentVariableA("SF6_COSTUME_LOADER_BASE_PAK", env_buf, MAX_PATH);
        if (env_n > 0 && env_n < MAX_PATH) base_path = env_buf;
        else base_path = game_dir + "\\re_chunk_000.pak";
    }

    // 1. Load vanilla index
    logf("Loading vanilla index...\n");
    VanillaIndex inv;
    if (!inv.load(tsv_path.c_str())) {
        logf("ERROR: cannot load %s\n", tsv_path.c_str());
        log_close(); return 1;
    }
    logf("  %zu costumes, %zu hashes\n", inv.costumes.size(), inv.all_hashes.size());

    // 2. Load static meta
    std::string meta_path = static_dir + "\\static_meta.json";
    StaticMeta meta;
    meta.load(meta_path.c_str());

    // 3. Classify patch paks
    logf("Scanning patch paks...\n");
    std::vector<PakInfo> our_paks, mod_paks;
    classify_patch_paks(pak_dir, our_paks, mod_paks);

    // 3a. Check if costume_mods folder exists (quick existence check)
    std::string costume_mods_dir = game_dir + "\\reframework\\costume_mods";
    bool has_costume_mods = (GetFileAttributesA(costume_mods_dir.c_str()) != INVALID_FILE_ATTRIBUTES);

    // 3b. No mod paks AND no costume_mods -> clean up and exit
    if (mod_paks.empty() && !has_costume_mods) {
        logf("  no mod paks found\n");
        // Delete our old paks
        for (auto& op : our_paks) {
            DeleteFileA(op.path.c_str());
            logf("  deleted %s\n", op.path.c_str());
        }
        // Clear possession in registry
        std::string reg_path = registry_dir + "\\registry.json";
        auto registry = load_registry(reg_path.c_str());
        registry.slots.clear();
        registry.possession.clear();
        registry.fingerprint.clear();
        save_registry(reg_path.c_str(), registry);
        auto t1 = std::chrono::high_resolution_clock::now();
        logf("Done (no mods) in %.1f ms\n", std::chrono::duration<double,std::milli>(t1-t0).count());
        log_close(); return 0;
    }

    // 3c. Pre-scan costume_mods for fingerprint (lightweight: only dir listing)
    std::vector<FolderModInfo> folder_infos;
    // We need to know about costume_mods for the fingerprint, but the full scan
    // requires the VanillaIndex (already loaded). We build folder_infos now.
    if (has_costume_mods) {
        WIN32_FIND_DATAA fd_c;
        HANDLE hc = FindFirstFileA((costume_mods_dir + "\\*").c_str(), &fd_c);
        if (hc != INVALID_HANDLE_VALUE) {
            do {
                if (fd_c.cFileName[0] == '.') continue;
                std::string cd = costume_mods_dir + "\\" + fd_c.cFileName;
                if (!(fd_c.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    // archive dropped straight into costume_mods
                    folder_infos.push_back({ std::string(fd_c.cFileName), cd });
                    continue;
                }
                WIN32_FIND_DATAA fd_co;
                HANDLE hco = FindFirstFileA((cd + "\\*").c_str(), &fd_co);
                if (hco == INVALID_HANDLE_VALUE) continue;
                do {
                    if (fd_co.cFileName[0] == '.') continue;   // folders and loose files (archives, paks)
                    folder_infos.push_back({
                        std::string(fd_c.cFileName) + "/" + fd_co.cFileName,
                        cd + "\\" + fd_co.cFileName
                    });
                } while (FindNextFileA(hco, &fd_co));
                FindClose(hco);
            } while (FindNextFileA(hc, &fd_c));
            FindClose(hc);
        }
        std::sort(folder_infos.begin(), folder_infos.end(),
                  [](auto& a, auto& b){ return a.rel_path < b.rel_path; });
    }

    // 3d. Fingerprint check for early exit
    std::string new_fp = compute_fingerprint(mod_paks, folder_infos);
    int max_mod_num = 0;
    for (auto& pi : mod_paks) max_mod_num = std::max(max_mod_num, pi.num);
    int target_num = max_mod_num + 1;
    char target_name[128];
    sprintf(target_name, "re_chunk_000.pak.patch_%03d.pak", target_num);
    std::string target_path = out_dir + "\\" + target_name;

    {
        std::string reg_path = registry_dir + "\\registry.json";
        auto reg_check = load_registry(reg_path.c_str());
        if (reg_check.fingerprint == new_fp && !new_fp.empty()) {
            // Check our pak exists
            if (GetFileAttributesA(target_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                auto t1 = std::chrono::high_resolution_clock::now();
                logf("Up to date (fingerprint match, %s exists) in %.1f ms\n",
                     target_name, std::chrono::duration<double,std::milli>(t1-t0).count());
                log_close(); return 0;
            }
        }
    }

    // 4. Open base pak (only if we need to regenerate)
    logf("Loading base pak...\n");
    PakReader base_pak;
    if (!base_pak.open(base_path.c_str())) {
        logf("ERROR: cannot open %s\n", base_path.c_str());
        log_close(); return 1;
    }
    logf("  %zu entries\n", base_pak.entry_count());
    load_current_crcs(base_pak);

    // 5. Scan each mod pak
    std::vector<std::pair<std::string, std::vector<ModCostume>>> mods_with_costumes;
    std::unordered_map<std::string, ModFileRef> all_mod_files;
    std::unordered_map<uint64_t, std::string> all_upgraded_tex; // old hash -> current path
    std::vector<std::unique_ptr<PakReader>> mod_pak_readers;
    std::vector<std::pair<int, PakReader*>> game_patch_readers;   // patch paks the game loads

    for (auto& pi : mod_paks) {
        printf("\n  patch_%03d: %s\n", pi.num, pi.path.c_str());
        auto reader = std::make_unique<PakReader>();
        if (!reader->open(pi.path.c_str())) continue;
        printf("    %zu entries\n", reader->entry_count());

        std::string mod_id = identify_mod_pak(pi.path.c_str());
        printf("    mod_id: %s\n", mod_id.c_str());

        std::unordered_map<std::string, ModFileRef> known;
        std::vector<uint64_t> unknown;
        std::unordered_map<uint64_t, std::string> upgraded_tex;
        scan_mod_pak(*reader, inv, known, unknown, upgraded_tex);
        all_upgraded_tex.insert(upgraded_tex.begin(), upgraded_tex.end());
        printf("    resolved: %zu paths, unattributed: %zu\n", known.size(), unknown.size());

        auto costumes = detect_mod_costumes(known, inv);
        for (auto& mc : costumes)
            printf("    -> %s costume %d (folder %s): %zu files\n",
                   mc.fighter_dir.c_str(), mc.original_costume_no,
                   mc.original_folder.c_str(), mc.files_in_folder.size());

        mods_with_costumes.push_back({mod_id, std::move(costumes)});
        all_mod_files.insert(known.begin(), known.end());
        game_patch_readers.push_back({pi.num, reader.get()});
        mod_pak_readers.push_back(std::move(reader));
    }

    // 5b. Scan costume_mods folders (merged after paks, alphabetical order)
    if (has_costume_mods) {
        scan_costume_mods(game_dir, inv, mods_with_costumes, all_mod_files,
                          mod_pak_readers, folder_infos, all_upgraded_tex);
    }

    if (mods_with_costumes.empty() || !std::any_of(mods_with_costumes.begin(),
            mods_with_costumes.end(), [](auto& p){ return !p.second.empty(); })) {
        logf("No costume mods detected -- cleaning up\n");
        for (auto& op : our_paks) {
            if (!delete_with_retry(op.path)) {
                logf("ERROR: %s is in use by another Street Fighter 6 process, not removed. "
                     "Close every Street Fighter 6 and launch the game again.\n", op.path.c_str());
                log_close(); return 1;
            }
            logf("  deleted %s\n", op.path.c_str());
        }
        std::string reg_path = registry_dir + "\\registry.json";
        auto registry = load_registry(reg_path.c_str());
        registry.slots.clear(); registry.possession.clear(); registry.fingerprint.clear();
        save_registry(reg_path.c_str(), registry);
        log_close(); return 0;
    }

    // 6. Load registry and assign slots
    std::string reg_path = registry_dir + "\\registry.json";
    auto registry = load_registry(reg_path.c_str());
    printf("\nAssigning slots...\n");
    auto slots = assign_slots(mods_with_costumes, inv, registry, meta);
    if (slots.empty()) { logf("No slots to create.\n"); log_close(); return 0; }

    // Build possession for registry
    registry.possession.clear();
    std::unordered_map<int, int> fighter_outfit_counter;
    for (auto& sl : slots) {
        int idx = fighter_outfit_counter[sl.fighter]++;
        std::string roman = int_to_roman(idx + 1);
        Possession pos;
        pos.fighter = sl.fighter;
        pos.fighter_dir = sl.fighter_dir;
        pos.costume_no = sl.new_costume_no;
        pos.record_id = sl.record_id;
        pos.manage_id = sl.manage_id;
        pos.name = std::string("Outfit ") + roman;
        sl.outfit_name = pos.name;
        registry.possession.push_back(pos);
        printf("  slot: %s/v%02d -> folder %s, record %d, \"%s\"\n",
               sl.fighter_dir.c_str(), sl.new_costume_no,
               sl.new_folder.c_str(), sl.record_id, pos.name.c_str());
    }

    // 7. Build output pak
    printf("\nAssembling output pak (minimal)...\n");
    PakWriter writer;
    std::unordered_set<std::string> written_paths;

    g_relocated_mod_paths.clear();
    for (auto& sl : slots)
        add_slot_files(writer, written_paths, sl, inv, base_pak, all_mod_files);

    // Restorations
    add_restorations(writer, written_paths, slots, inv, base_pak, all_mod_files);

    // Slot check: every slot must be loadable as the game will load it, else it is left out
    {
        std::vector<std::unique_ptr<PakReader>> dlc_readers;
        {
            WIN32_FIND_DATAA fdta;
            std::string dlc_dir = game_dir + "\\dlc\\";
            HANDLE fh = FindFirstFileA((dlc_dir + "*.pak").c_str(), &fdta);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    auto r = std::make_unique<PakReader>();
                    if (r->open((dlc_dir + fdta.cFileName).c_str())) dlc_readers.push_back(std::move(r));
                } while (FindNextFileA(fh, &fdta));
                FindClose(fh);
            }
        }
        slotcheck::Ctx ctx;
        ctx.gf.writer = &writer;
        std::sort(game_patch_readers.begin(), game_patch_readers.end(),
                  [](auto& a, auto& b) { return a.first > b.first; });
        for (auto& [num, r] : game_patch_readers) ctx.gf.paks.push_back(r);
        for (auto& r : dlc_readers) ctx.gf.paks.push_back(r.get());
        ctx.gf.paks.push_back(&base_pak);
        ctx.suffixes = &inv.suffixes;
        ctx.all_mod = &all_mod_files;
        printf("\nChecking slots...\n");
        std::vector<SlotInfo> kept;
        int left_out = 0;
        for (auto& sl : slots) {
            std::string why = slotcheck::check_slot(ctx, sl);
            if (why.empty()) { kept.push_back(sl); continue; }
            left_out++;
            printf("  WARN: slot %s/v%02d (%s, from %s) left out, the game could not load it: %s\n",
                   sl.fighter_dir.c_str(), sl.new_costume_no, sl.outfit_name.c_str(),
                   mod_label(sl.mod_id).c_str(), why.c_str());
        }
        printf("  %zu slots checked, %d left out\n", slots.size(), left_out);
        if (left_out) {
            std::vector<Possession> pos;
            for (auto& p : registry.possession)
                for (auto& sl : kept)
                    if (sl.fighter_dir == p.fighter_dir && sl.new_costume_no == p.costume_no) { pos.push_back(p); break; }
            registry.possession = std::move(pos);
            slots = std::move(kept);
        }
    }

    // Static structural files (generic list from static_meta.json, fallback to hardcoded)
    struct SFile { std::string local; std::string pak; };
    std::vector<SFile> sfiles;
    if (!meta.files.empty()) {
        for (auto& mf : meta.files)
            sfiles.push_back({mf.local, mf.pak_path});
    } else {
        // Fallback: 3 hardcoded names (pre-files-section compat)
        sfiles.push_back({"esf.scn.20", ESF_ROOT});
        sfiles.push_back({"fightercostumeuserdata.user.2", COSTUME_TBL});
        sfiles.push_back({"fightercostumemessage.msg.21", COSTUME_MSG});
    }
    for (auto& sf : sfiles) {
        std::string fp = static_dir + "\\" + sf.local;
        auto data = read_file_bin(fp.c_str());
        if (data.empty()) {
            printf("  WARN: static file not found: %s\n", fp.c_str());
            continue;
        }
        // The costume table only lists the slots in use: a character the player does not own shows
        // every record of the table, and a reserved slot has no scene to load.
        if (str_ends_with(sf.pak, "fightercostumeuserdata.user.2")) {
            std::unordered_set<uint32_t> keep;
            for (auto& sl : slots) keep.insert((uint32_t)sl.record_id);
            size_t kept = 0, removed = 0;
            if (trim_costume_table(data, keep, &kept, &removed))
                printf("  costume table: %zu slot records kept, %zu unused ones left out\n", kept, removed);
            else
                printf("  WARN: costume table layout not recognised, shipped with every reserved record\n");
        }
        size_t dsz = data.size();
        uint64_t h = pak_path_hash(std::string_view(sf.pak));
        writer.add_uncompressed(h, std::move(data));
        written_paths.insert(sf.pak);
        printf("  static: %s (%zu bytes)\n", sf.local.c_str(), dsz);
    }


    // Mod files made before a game update
    if (!g_crc_upgraded_files.empty())
        printf("  %zu mod files brought to the current type signatures (colour files made before a game update)\n",
               g_crc_upgraded_files.size());
    if (!g_tex_repaired_files.empty())
        printf("  %zu mod textures with a wrong row pitch in their mip table, repaired\n", g_tex_repaired_files.size());
    for (auto& f : g_tex_bad_files)
        printf("  WARN: mod texture with an inconsistent mip table: %s\n", f.c_str());
    if (!g_layout_upgraded_files.empty())
        printf("  %zu colour files converted from the 2023 layout (disabled fur block added to each garment)\n",
               g_layout_upgraded_files.size());
    for (auto& [t, files] : g_crc_stale_files) {
        const char* lbl = type_label(t);
        printf("  WARN: %zu mod files use an older layout of %s%s%08X, left as they are (their colours may not load):\n",
               files.size(), lbl ? lbl : "type ", lbl ? " " : "", t);
        for (auto& f : files) printf("        %s\n", f.c_str());
    }

    // Marker
    {
        std::vector<uint8_t> marker(20);
        memcpy(marker.data(), "SF6_CostumeSlots v1\n", 20);
        uint64_t mh = pak_path_hash(std::string_view(MARKER_PATH));
        writer.add_uncompressed(mh, std::move(marker));
        written_paths.insert(MARKER_PATH);
    }

    // 8. Delete old our-paks. A Street Fighter 6 that was just closed often keeps running for a while
    // with the pak open, which can then be neither deleted nor rewritten (27/09: the pak stayed the old
    // one while the log said "wrote", and the saved fingerprint kept it for good). Up to ~10 s of
    // waiting covers a normal exit; past that nothing is changed, the old pak and the old registry
    // still match, and the next launch tries again.
    for (auto& op : our_paks) {
        if (!delete_with_retry(op.path)) {
            logf("ERROR: %s is in use by another Street Fighter 6 process, costumes not updated. "
                 "Close every Street Fighter 6 (see the Task Manager) and launch the game again.\n",
                 op.path.c_str());
            log_close(); return 1;
        }
        logf("  removed old pak: %s\n", op.path.c_str());
    }

    // 9. Write output pak; the registry is saved only once it is on disk
    if (!writer.write(target_path.c_str())) {
        DeleteFileA(target_path.c_str());
        logf("ERROR: could not write %s (disk full or access denied), costumes not updated.\n",
             target_path.c_str());
        log_close(); return 1;
    }
    logf("  wrote %s (%zu entries)\n", target_path.c_str(), writer.entry_count());
    if (writer.dedup_count())
        logf("  %zu entries share data with an identical one (%.1f MB not written twice)\n",
             writer.dedup_count(), writer.dedup_bytes() / 1048576.0);

    // 10. Save registry with fingerprint
    registry.fingerprint = new_fp;
    save_registry(reg_path.c_str(), registry);
    logf("  registry saved\n");

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    logf("Done in %.1f ms\n\n", ms);
    log_close();
    return 0;
}
