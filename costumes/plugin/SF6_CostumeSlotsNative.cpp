// SF6_CostumeSlotsNative — partie "en match" du systeme d'alias de costume.
// Porte depuis SF6_CostumeSlots.lua : activation du dossier du slot, recopie du
// manifeste visuel (SettingData.overwrite), permutation des couleurs CCVD.
// Fonctionne meme quand REFramework coupe les scripts Lua en match en ligne.
// Le Lua garde les menus, la sauvegarde et la possession.
//
// Compile contre les en-tetes API v1.5.8 (minor=10) pour compatibilite.
// Build : /EHa (regle 17 du projet), /std:c++20, /O2, /W4.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <reframework/API.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace costume_slots {

using API  = reframework::API;
using Obj  = API::ManagedObject;
using Ret  = reframework::InvokeRet;

// ── constantes ──────────────────────────────────────────────────────────────────
constexpr int  BASE_COS = 4;   // DriveTech = costume 4 pour tous les persos
constexpr char kState[]    = "reframework/data/SF6_CostumeSlots_data/state.json";
constexpr char kRegistry[] = "reframework/data/SF6_Costumes_Data/registry.json";
constexpr char kPresence[] = "reframework/data/SF6_CostumeSlots_data/native_present.json";
constexpr char kLog[]      = "reframework/data/SF6_CostumeSlots_data/native_log.txt";

// ── API accessor ────────────────────────────────────────────────────────────────
static inline API& reapi() { return *API::get(); }

// ── logging ─────────────────────────────────────────────────────────────────────
static void log_info(const char* fmt, ...) {
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    reapi().log_info("[CostumeSlotsNative] %s", buf);
}
static void log_error(const char* fmt, ...) {
    char buf[512]; va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    reapi().log_error("[CostumeSlotsNative] %s", buf);
}

// journal fichier (append, horodate, max ~1 Mo)
static void log_file(const char* fmt, ...) {
    FILE* f = fopen(kLog, "ab");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) > 1024 * 1024) { fclose(f); return; }
    time_t now = time(nullptr);
    struct tm t; localtime_s(&t, &now);
    fprintf(f, "%02d:%02d:%02d ", t.tm_hour, t.tm_min, t.tm_sec);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f);
    fclose(f);
}

// ── fichiers ────────────────────────────────────────────────────────────────────
static std::string read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); return {}; }
    fseek(f, 0, SEEK_SET);
    std::string buf(static_cast<size_t>(sz), '\0');
    fread(buf.data(), 1, static_cast<size_t>(sz), f);
    fclose(f);
    return buf;
}

static FILETIME get_mtime(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return d.ftLastWriteTime;
    return {};
}
static bool ft_eq(FILETIME a, FILETIME b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

// ── System.String → UTF-8 (layout SF6 : size @0x10, wchar_t[] @0x14) ─────────
static std::string managed_str(Obj* s) {
    if (!s) return {};
    auto sz = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(s) + 0x10);
    if (sz <= 0 || sz > 4096) return {};
    auto* data = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(s) + 0x14);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, data, sz, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, sz, out.data(), bytes, nullptr, nullptr);
    return out;
}

// ── helpers champ / methode (patron ll_common) ──────────────────────────────────
static API::Field* find_field_walk(API::TypeDefinition* td, const char* name) {
    for (auto* t = td; t; t = t->get_parent_type())
        if (auto* f = t->find_field(name)) return f;
    return nullptr;
}
static Obj* read_field_obj(Obj* o, const char* name) {
    if (!o) return nullptr;
    auto* f = find_field_walk(o->get_type_definition(), name);
    if (!f) return nullptr;
    void* raw = f->get_data_raw(o, false);
    return raw ? *reinterpret_cast<Obj**>(raw) : nullptr;
}
static bool read_field_i32(Obj* o, const char* name, int32_t& out) {
    if (!o) return false;
    auto* f = find_field_walk(o->get_type_definition(), name);
    if (!f) return false;
    void* raw = f->get_data_raw(o, false);
    if (!raw) return false;
    memcpy(&out, raw, sizeof out);
    return true;
}
static bool write_field_obj(Obj* o, const char* name, Obj* val) {
    if (!o) return false;
    auto* f = find_field_walk(o->get_type_definition(), name);
    if (!f) return false;
    void* raw = f->get_data_raw(o, false);
    if (!raw) return false;
    *reinterpret_cast<Obj**>(raw) = val;
    return true;
}
static void* arg_i32(int32_t v) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(v)));
}
static void* arg_bool(bool v) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(v ? 1u : 0u));
}
// methode par nom + nb de params (fallback scan si find_method exact echoue)
static API::Method* find_method_compat(API::TypeDefinition* td, const char* name, int nparams = -1) {
    if (!td) return nullptr;
    if (auto* m = td->find_method(name)) {
        if (nparams < 0 || m->get_num_params() == static_cast<uint32_t>(nparams)) return m;
    }
    // scan par nom nu (avant la parenthese) + nb de params
    std::string bare = name;
    auto paren = bare.find('(');
    if (paren != std::string::npos) bare.resize(paren);
    for (auto* t = td; t; t = t->get_parent_type()) {
        for (auto* m : t->get_methods()) {
            if (!m || !m->get_name()) continue;
            if (bare != m->get_name()) continue;
            if (nparams >= 0 && m->get_num_params() != static_cast<uint32_t>(nparams)) continue;
            return m;
        }
    }
    return nullptr;
}

// invoke helpers (emettent un log en cas d'exception, ne propagent pas)
static Ret safe_invoke(API::Method* m, Obj* o, std::vector<void*> args = {}) {
    if (!m) { Ret r; r.exception_thrown = true; return r; }
    return m->invoke(o, args);
}
static Obj* invoke_obj(API::Method* m, Obj* o, std::vector<void*> args = {}) {
    auto r = safe_invoke(m, o, std::move(args));
    return (!r.exception_thrown && r.ptr) ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
}
static int invoke_count(Obj* list) {
    if (!list) return 0;
    auto* m = find_method_compat(list->get_type_definition(), "get_Count", 0);
    auto r = safe_invoke(m, list);
    return r.exception_thrown ? 0 : static_cast<int>(r.dword);
}
static Obj* invoke_item(Obj* list, int i) {
    if (!list) return nullptr;
    auto* m = find_method_compat(list->get_type_definition(), "get_Item", 1);
    return invoke_obj(m, list, { arg_i32(i) });
}

// ── JSON parsing (formats controles, ecrits par notre Lua / loader) ─────────────

struct FighterIntent { int slot = 0; int color = -1; };
struct SlotEntry     { int fighter = 0; int costume_no = 0; };

static std::unordered_map<int,FighterIntent> parse_state(const std::string& js) {
    std::unordered_map<int,FighterIntent> out;
    auto fp = js.find("\"fighters\"");
    if (fp == std::string::npos) return out;
    auto brace = js.find('{', fp + 10);
    if (brace == std::string::npos) return out;

    size_t pos = brace + 1;
    for (;;) {
        // sauter espaces / virgules
        while (pos < js.size() && (js[pos]==' '||js[pos]=='\t'||js[pos]=='\n'||js[pos]=='\r'||js[pos]==',')) ++pos;
        if (pos >= js.size() || js[pos] == '}') break;
        if (js[pos] != '"') { ++pos; continue; }

        // cle = fighter id
        ++pos;
        auto ke = js.find('"', pos);
        if (ke == std::string::npos) break;
        int fid = atoi(js.c_str() + pos);
        pos = ke + 1;

        // valeur = sous-objet { ... }
        auto os2 = js.find('{', pos);
        if (os2 == std::string::npos) break;
        auto oe = js.find('}', os2);
        if (oe == std::string::npos) break;
        std::string sub = js.substr(os2, oe - os2 + 1);

        FighterIntent fi;
        auto sl = sub.find("\"slot\"");
        if (sl != std::string::npos) {
            auto sc = sub.find(':', sl + 6);
            if (sc != std::string::npos) {
                size_t v = sc + 1;
                while (v < sub.size() && sub[v] == ' ') ++v;
                fi.slot = (v < sub.size() && sub[v] == 'f') ? 0 : atoi(sub.c_str() + v);
            }
        }
        auto cl = sub.find("\"color\"");
        if (cl != std::string::npos) {
            auto cc = sub.find(':', cl + 7);
            if (cc != std::string::npos) {
                size_t v = cc + 1;
                while (v < sub.size() && sub[v] == ' ') ++v;
                if (v < sub.size() && sub[v] != 'n') fi.color = atoi(sub.c_str() + v);
            }
        }
        if (fid > 0) out[fid] = fi;
        pos = oe + 1;
    }
    return out;
}

static std::vector<SlotEntry> parse_registry(const std::string& js) {
    std::vector<SlotEntry> out;
    auto pp = js.find("\"possession\"");
    if (pp == std::string::npos) return out;
    auto arr = js.find('[', pp);
    if (arr == std::string::npos) return out;

    size_t pos = arr + 1;
    for (;;) {
        auto os2 = js.find('{', pos);
        if (os2 == std::string::npos) break;
        auto ae = js.find(']', pos);
        if (ae != std::string::npos && ae < os2) break;   // fin du tableau
        auto oe = js.find('}', os2);
        if (oe == std::string::npos) break;
        std::string sub = js.substr(os2, oe - os2 + 1);

        SlotEntry e;
        auto fp2 = sub.find("\"fighter\"");
        if (fp2 != std::string::npos) {
            auto fc = sub.find(':', fp2 + 9);
            if (fc != std::string::npos) { size_t v = fc+1; while (v<sub.size()&&sub[v]==' ') ++v; e.fighter = atoi(sub.c_str()+v); }
        }
        auto cp = sub.find("\"costume_no\"");
        if (cp != std::string::npos) {
            auto cc = sub.find(':', cp + 12);
            if (cc != std::string::npos) { size_t v = cc+1; while (v<sub.size()&&sub[v]==' ') ++v; e.costume_no = atoi(sub.c_str()+v); }
        }
        if (e.fighter > 0 && e.costume_no > 0) out.push_back(e);
        pos = oe + 1;
    }
    return out;
}

// ── etat ────────────────────────────────────────────────────────────────────────
struct FighterRT {
    int slot = 0, color = -1;
    uintptr_t swapped_addr = 0;          // adresse du dernier holder DST reecrit
    struct { uintptr_t addr; int a, b; } csw = {};
    bool has_csw = false;
    int  last_mount = -1000;
    Obj* src_folder = nullptr;            // cache de recherche, valide par try/catch
    std::string last_sig;
};

struct State {
    // registre : fighter → costume_no → suffixe / chemin complet
    std::unordered_map<int, std::unordered_map<int,std::string>> slots;   // suffixe pour match
    std::unordered_map<int, std::unordered_map<int,std::string>> paths;   // chemin complet
    std::vector<int>         fids;
    std::unordered_set<int>  fset;
    std::unordered_map<int, FighterRT> rt;

    // DST = chemin complet du dossier v04 dans la scene
    std::unordered_map<int, std::string> dst;
    bool dst_done = false;

    // couleurs de base de DriveTech par perso
    std::unordered_map<int, std::unordered_set<int>> base_colors;
    std::unordered_map<int, int> base_color_min;
    bool bc_done = false;

    // surveillance de fichiers
    FILETIME mt_state = {}, mt_reg = {};
    time_t   last_fcheck = 0;
    time_t   last_presence = 0;

    uint32_t frame = 0;
    bool select_gate = false;

    // cache types / methodes
    bool tcached = false, tok = false;
    API::TypeDefinition *td_folder = nullptr, *td_scene = nullptr, *td_sm = nullptr;
    API::Method *m_cur_scene = nullptr, *m_first_folder = nullptr;
    API::Method *m_f_child = nullptr, *m_f_next = nullptr, *m_f_path = nullptr;
    API::Method *m_f_active = nullptr, *m_f_activate = nullptr;
    API::Method *m_find_comps = nullptr, *m_get_go = nullptr, *m_go_folder = nullptr;
    Obj* holder_type = nullptr;  // System.Type

    // methodes resolues tardivement
    API::Method *m_get_master = nullptr, *m_get_cos_colors = nullptr;
    API::Method *m_overwrite  = nullptr;
    bool bc_meth_done = false, ow_meth_done = false;
};
static State g;

// ── cache de types ──────────────────────────────────────────────────────────────
static bool cache_types() {
    if (g.tcached) return g.tok;
    g.tcached = true;
    auto* tdb = reapi().tdb();

    g.td_sm     = tdb->find_type("via.SceneManager");
    g.td_scene  = tdb->find_type("via.Scene");
    g.td_folder = tdb->find_type("via.Folder");
    auto* td_c  = tdb->find_type("via.Component");
    auto* td_go = tdb->find_type("via.GameObject");
    if (!g.td_sm || !g.td_scene || !g.td_folder || !td_c || !td_go) {
        log_error("type lookup failed"); return false;
    }

    g.m_cur_scene    = find_method_compat(g.td_sm,     "get_CurrentScene", 0);
    g.m_first_folder = find_method_compat(g.td_scene,  "get_FirstFolder",  0);
    g.m_find_comps   = find_method_compat(g.td_scene,  "findComponents(System.Type)", 1);
    g.m_f_child      = find_method_compat(g.td_folder, "get_Child",  0);
    g.m_f_next       = find_method_compat(g.td_folder, "get_Next",   0);
    g.m_f_path       = find_method_compat(g.td_folder, "get_Path",   0);
    g.m_f_active     = find_method_compat(g.td_folder, "get_Active", 0);
    g.m_f_activate   = find_method_compat(g.td_folder, "activate",   0);
    g.m_get_go       = find_method_compat(td_c,        "get_GameObject", 0);
    g.m_go_folder    = find_method_compat(td_go,       "get_FolderSelf",  0);

    if (!g.m_cur_scene || !g.m_first_folder || !g.m_find_comps ||
        !g.m_f_child || !g.m_f_next || !g.m_f_path || !g.m_f_active ||
        !g.m_f_activate || !g.m_get_go || !g.m_go_folder) {
        log_error("method lookup failed"); return false;
    }

    g.holder_type = reapi().typeof("app.battle.assets.FighterVisualHolder");
    if (!g.holder_type) { log_error("typeof FighterVisualHolder failed"); return false; }

    g.tok = true;
    log_info("types et methodes en cache");
    return true;
}

// ── acces scene / dossiers ──────────────────────────────────────────────────────
static Obj* get_scene() {
    auto* sm = reapi().get_native_singleton("via.SceneManager");
    return sm ? invoke_obj(g.m_cur_scene, reinterpret_cast<Obj*>(sm)) : nullptr;
}
static std::string fld_path(Obj* f) {
    auto* s = invoke_obj(g.m_f_path, f);
    return s ? managed_str(s) : std::string{};
}
static Obj* fld_child(Obj* f) { return invoke_obj(g.m_f_child, f); }
static Obj* fld_next(Obj* f)  { return invoke_obj(g.m_f_next, f); }
static bool fld_active(Obj* f) {
    auto r = safe_invoke(g.m_f_active, f);
    return !r.exception_thrown && r.byte != 0;
}

// ── correspondance par suffixe ──────────────────────────────────────────────────
static bool ends_with(const std::string& s, const std::string& sfx) {
    return sfx.size() <= s.size() && s.compare(s.size()-sfx.size(), sfx.size(), sfx) == 0;
}
static std::string extract_suffix(const std::string& p) {
    auto i = p.find("content/esf/");
    return (i != std::string::npos) ? p.substr(i) : p;
}
static bool same_slot_path(const std::string& actual, const std::string& wanted) {
    if (actual == wanted) return true;
    auto sfx = extract_suffix(wanted);
    return !sfx.empty() && ends_with(actual, sfx);
}

// ── find_dst_all : dossier v04 par perso ────────────────────────────────────────
struct DstQ { int fid; std::string sfx; };
static void walk_dst(Obj* f, int depth, std::vector<DstQ>& q) {
    if (!f || q.empty() || depth > 8) return;
    auto p = fld_path(f);
    if (!p.empty()) {
        for (int i = static_cast<int>(q.size())-1; i >= 0; --i) {
            if (ends_with(p, q[static_cast<size_t>(i)].sfx)) {
                g.dst[q[static_cast<size_t>(i)].fid] = p;
                log_file("[F%d] DST = %s", q[static_cast<size_t>(i)].fid, p.c_str());
                q.erase(q.begin() + i);
            }
        }
    }
    walk_dst(fld_child(f), depth+1, q);
    walk_dst(fld_next(f),  depth,   q);
}
static void find_dst_all() {
    if (g.dst_done) return;
    auto* sc = get_scene();
    if (!sc) return;
    std::vector<DstQ> q;
    for (int fid : g.fids) {
        if (g.dst.count(fid)) continue;
        char buf[64]; snprintf(buf, sizeof buf, "fighter%03d/esf%03dv%02d", fid, fid, BASE_COS);
        q.push_back({fid, buf});
    }
    if (q.empty()) { g.dst_done = true; return; }
    auto* root = invoke_obj(g.m_first_folder, sc);
    if (root) walk_dst(root, 0, q);
    g.dst_done = q.empty();
}

// ── find_folder : retrouver un dossier de slot par suffixe ──────────────────────
static Obj* g_ff_result;
static void walk_ff(Obj* f, const std::string& sfx, int depth) {
    if (!f || g_ff_result || depth > 8) return;
    auto p = fld_path(f);
    if (!p.empty() && ends_with(p, sfx)) { g_ff_result = f; return; }
    walk_ff(fld_child(f), sfx, depth+1);
    walk_ff(fld_next(f),  sfx, depth);
}
static Obj* find_folder(const std::string& wanted) {
    auto* sc = get_scene();
    if (!sc) return nullptr;
    auto sfx = extract_suffix(wanted);
    if (sfx.empty()) return nullptr;
    g_ff_result = nullptr;
    auto* root = invoke_obj(g.m_first_folder, sc);
    if (root) walk_ff(root, sfx, 0);
    return g_ff_result;
}

// ── select_screen_active ────────────────────────────────────────────────────────
static bool select_screen_active() {
    auto* fm = reapi().get_managed_singleton("app.UIFlowManager");
    if (!fm) return false;
    auto* hs = read_field_obj(fm, "_Handles");
    if (!hs) return false;
    int n = invoke_count(hs);
    for (int i = 0; i < n; ++i) {
        auto* h = invoke_item(hs, i);
        auto* prm = h ? read_field_obj(h, "<Param>k__BackingField") : nullptr;
        if (!prm) continue;
        auto* td = prm->get_type_definition();
        if (!td) continue;
        auto nm = td->get_full_name();
        if (nm.find("UIFlowUI105") != std::string::npos || nm.find("SelectFighter") != std::string::npos)
            return true;
    }
    return false;
}

// ── BASE_COLORS ─────────────────────────────────────────────────────────────────
static void read_base_colors() {
    if (g.bc_done) return;
    auto* tm = reapi().get_managed_singleton("app.TableDataManager");
    auto* im = reapi().get_managed_singleton("app.InventoryManager");
    if (!im || !tm) return;
    auto* ci = read_field_obj(im, "_Costume");
    if (!ci) return;

    if (!g.bc_meth_done) {
        g.bc_meth_done = true;
        g.m_get_master     = find_method_compat(ci->get_type_definition(), "GetMasterDataRecord", 1);
        g.m_get_cos_colors = find_method_compat(tm->get_type_definition(), "GetFighterCostumeColors", 2);
    }
    if (!g.m_get_master || !g.m_get_cos_colors) return;

    for (int rid = 1; rid <= 250; ++rid) {
        auto rr = safe_invoke(g.m_get_master, ci, { arg_i32(rid) });
        if (rr.exception_thrown || !rr.ptr) continue;
        auto* rec = reinterpret_cast<Obj*>(rr.ptr);
        int32_t fid = 0, cn = 0;
        if (!read_field_i32(rec, "fighterId", fid) || !read_field_i32(rec, "costumeNo", cn)) continue;
        if (cn != BASE_COS || !g.fset.count(fid) || g.base_colors.count(fid)) continue;

        auto cr = safe_invoke(g.m_get_cos_colors, tm, { arg_i32(rid), arg_bool(false) });
        if (cr.exception_thrown || !cr.ptr) continue;
        auto* cls = reinterpret_cast<Obj*>(cr.ptr);
        int cc = invoke_count(cls);
        std::unordered_set<int> cols;
        int min_c = 999;
        for (int j = 0; j < cc; ++j) {
            auto* cr2 = invoke_item(cls, j);
            int32_t c = 0;
            if (cr2 && read_field_i32(cr2, "colorNo", c)) { cols.insert(c); if (c < min_c) min_c = c; }
        }
        g.base_colors[fid] = cols;
        g.base_color_min[fid] = (min_c < 999) ? min_c : 0;
        log_file("[F%d] BASE_COLORS (%d couleurs, base=%d)", fid, cc, g.base_color_min[fid]);
    }
    bool all = true;
    for (int fid : g.fids) if (!g.base_colors.count(fid)) { all = false; break; }
    g.bc_done = all;
}

// ── holder_folder_path ──────────────────────────────────────────────────────────
static std::string holder_folder_path(Obj* comp) {
    auto* go = invoke_obj(g.m_get_go, comp);
    if (!go) return {};
    auto* fld = invoke_obj(g.m_go_folder, go);
    return fld ? fld_path(fld) : std::string{};
}

// ── dst_holder_present ──────────────────────────────────────────────────────────
static bool dst_holder_present(int fid) {
    auto di = g.dst.find(fid);
    if (di == g.dst.end()) return false;
    auto* sc = get_scene();
    if (!sc) return false;
    auto* comps = invoke_obj(g.m_find_comps, sc, { reinterpret_cast<void*>(g.holder_type) });
    if (!comps) return false;
    int n = invoke_count(comps);
    for (int i = 0; i < n; ++i) {
        auto* c = invoke_item(comps, i);
        if (c && holder_folder_path(c) == di->second) return true;
    }
    return false;
}

// ── keep_mounted ────────────────────────────────────────────────────────────────
static void keep_mounted(int fid) {
    auto& fr = g.rt[fid];
    if (fr.slot <= 0) return;
    auto pi = g.paths.find(fid);
    if (pi == g.paths.end()) return;
    auto ci = pi->second.find(fr.slot);
    if (ci == pi->second.end()) return;
    const std::string& src_path = ci->second;

    if (!fr.src_folder) fr.src_folder = find_folder(src_path);
    if (!fr.src_folder) return;

    bool active = false;
    try { active = fld_active(fr.src_folder); }
    catch (...) { fr.src_folder = nullptr; return; }

    if (!active && (static_cast<int>(g.frame) - fr.last_mount) > 120 && dst_holder_present(fid)) {
        fr.last_mount = static_cast<int>(g.frame);
        try { safe_invoke(g.m_f_activate, fr.src_folder); }
        catch (...) { fr.src_folder = nullptr; return; }
        log_file("[F%d] montage de v%d (DriveTech monte par le jeu)", fid, fr.slot);
    }
}

// ── CCVD color swap ─────────────────────────────────────────────────────────────
static bool ccvd_swap(Obj* ccvd, int a, int b) {
    auto* colors = read_field_obj(ccvd, "Colors");
    if (!colors) return false;
    int n = invoke_count(colors);
    Obj *ea = nullptr, *eb = nullptr;
    for (int i = 0; i < n; ++i) {
        auto* it = invoke_item(colors, i);
        int32_t id = -1;
        if (it && read_field_i32(it, "ColorId", id)) {
            if (id == a) ea = it; else if (id == b) eb = it;
        }
    }
    if (!ea || !eb) return false;
    auto* da = read_field_obj(ea, "Data");
    auto* db = read_field_obj(eb, "Data");
    if (!da || !db) return false;
    write_field_obj(ea, "Data", db);
    write_field_obj(eb, "Data", da);
    return true;
}

static void apply_color(int fid, Obj* d_set) {
    auto& fr = g.rt[fid];
    if (fr.color < 0 || fr.has_csw) return;
    auto bci = g.base_colors.find(fid);
    if (bci != g.base_colors.end() && bci->second.count(fr.color)) return;
    int base_c = g.base_color_min.count(fid) ? g.base_color_min[fid] : 0;
    auto* ccvd = read_field_obj(d_set, "costumeColorVariation");
    if (!ccvd) { log_file("[F%d] couleur : pas de costumeColorVariation", fid); return; }
    if (ccvd_swap(ccvd, base_c, fr.color)) {
        fr.csw = { reinterpret_cast<uintptr_t>(ccvd), base_c, fr.color };
        fr.has_csw = true;
        log_file("[F%d] couleur %d placee sous la couleur %d", fid, fr.color, base_c);
    } else {
        log_file("[F%d] couleur : entrees %d/%d introuvables", fid, base_c, fr.color);
    }
}

static void restore_color(int fid, Obj* src_h) {
    auto& fr = g.rt[fid];
    if (!fr.has_csw) return;
    auto* s_set = src_h ? read_field_obj(src_h, "_Setting") : nullptr;
    auto* ccvd  = s_set ? read_field_obj(s_set, "costumeColorVariation") : nullptr;
    if (ccvd && reinterpret_cast<uintptr_t>(ccvd) == fr.csw.addr) {
        bool ok = ccvd_swap(ccvd, fr.csw.a, fr.csw.b);
        log_file("[F%d] couleur : remise en place = %s", fid, ok ? "true" : "false");
    } else {
        log_file("[F%d] couleur : CCVD source different/absent", fid);
    }
    fr.has_csw = false;
}

// ── swap_manifest ───────────────────────────────────────────────────────────────
static void swap_manifest(int fid) {
    auto di = g.dst.find(fid);
    if (di == g.dst.end()) return;
    auto* sc = get_scene();
    if (!sc) return;
    auto* comps = invoke_obj(g.m_find_comps, sc, { reinterpret_cast<void*>(g.holder_type) });
    if (!comps) return;
    int n = invoke_count(comps);

    auto& fr = g.rt[fid];
    auto pi = g.paths.find(fid);
    if (pi == g.paths.end()) return;
    auto ci = pi->second.find(fr.slot);
    if (ci == pi->second.end()) return;
    const std::string& src_path = ci->second;

    Obj *src_h = nullptr, *dst_h = nullptr;
    for (int i = 0; i < n; ++i) {
        auto* c = invoke_item(comps, i);
        if (!c) continue;
        auto p = holder_folder_path(c);
        if (p.empty()) continue;
        if (same_slot_path(p, src_path))          src_h = c;
        else if (p == di->second && !dst_h)       dst_h = c;   // premier v04 seulement
    }

    // diagnostic
    char sig[64]; snprintf(sig, sizeof sig, "src=%s dst=%s", src_h?"y":"-", dst_h?"y":"-");
    if (fr.last_sig != sig) {
        log_file("[F%d] holders %s (n=%d, v%d)", fid, sig, n, fr.slot);
        fr.last_sig = sig;
    }

    if (dst_h && src_h) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(dst_h);
        if (fr.swapped_addr != addr) {
            auto* s_set = read_field_obj(src_h, "_Setting");
            auto* d_set = read_field_obj(dst_h, "_Setting");
            if (s_set && d_set) {
                // methode overwrite (resolue une seule fois)
                if (!g.ow_meth_done) {
                    g.ow_meth_done = true;
                    g.m_overwrite = find_method_compat(d_set->get_type_definition(), "overwrite", 1);
                }
                if (g.m_overwrite) {
                    auto ow = safe_invoke(g.m_overwrite, d_set, { reinterpret_cast<void*>(s_set) });
                    bool ok = !ow.exception_thrown;
                    log_file("[F%d] manifeste v%d -> v%d = %s", fid, fr.slot, BASE_COS, ok?"true":"false");
                    fr.swapped_addr = addr;
                    if (ok) apply_color(fid, d_set);
                } else {
                    log_file("[F%d] methode overwrite introuvable", fid);
                }
            }
        }
    } else if (!dst_h) {
        if (fr.swapped_addr && fr.has_csw) restore_color(fid, src_h);
        fr.swapped_addr = 0;
    }
}

// ── rechargement des fichiers ───────────────────────────────────────────────────
static void reload_state() {
    auto js = read_file(kState);
    if (js.empty()) return;
    auto parsed = parse_state(js);
    for (auto& [fid, fi] : parsed) {
        if (!g.fset.count(fid)) continue;
        auto& fr = g.rt[fid];
        if (fr.slot != fi.slot || fr.color != fi.color) {
            fr.slot = fi.slot; fr.color = fi.color;
            fr.src_folder = nullptr; fr.swapped_addr = 0;
            log_file("[F%d] state: slot=%d color=%d", fid, fr.slot, fr.color);
        }
    }
    for (int fid : g.fids) {
        if (!parsed.count(fid)) {
            auto& fr = g.rt[fid];
            if (fr.slot) { fr.slot = 0; fr.color = -1; fr.src_folder = nullptr; fr.swapped_addr = 0; }
        }
    }
}

static void reload_registry() {
    auto js = read_file(kRegistry);
    if (js.empty()) return;
    auto entries = parse_registry(js);
    g.slots.clear(); g.paths.clear(); g.fids.clear(); g.fset.clear();
    for (auto& e : entries) {
        char path[128], sfx[128];
        snprintf(path, sizeof path, "product/content/esf/fighter%03d/esf%03dv%02d", e.fighter, e.fighter, e.costume_no);
        snprintf(sfx,  sizeof sfx,  "content/esf/fighter%03d/esf%03dv%02d",         e.fighter, e.fighter, e.costume_no);
        g.paths[e.fighter][e.costume_no] = path;
        g.slots[e.fighter][e.costume_no] = sfx;
        if (!g.fset.count(e.fighter)) { g.fset.insert(e.fighter); g.fids.push_back(e.fighter); }
    }
    g.dst_done = false; g.bc_done = false;
    for (int fid : g.fids) if (!g.rt.count(fid)) g.rt[fid] = {};
    log_file("registry: %d slots, %d persos", static_cast<int>(entries.size()), static_cast<int>(g.fids.size()));
}

// ── presence ────────────────────────────────────────────────────────────────────
static void write_presence() {
    time_t now = time(nullptr);
    if (now - g.last_presence < 2) return;
    g.last_presence = now;
    FILE* f = fopen(kPresence, "wb");
    if (!f) return;
    fprintf(f, "{\"ts\":%lld,\"pid\":%lu}", static_cast<long long>(now), static_cast<unsigned long>(GetCurrentProcessId()));
    fclose(f);
}

// ── surveillance de fichiers ────────────────────────────────────────────────────
static void check_files() {
    time_t now = time(nullptr);
    if (now == g.last_fcheck) return;
    g.last_fcheck = now;
    auto ft = get_mtime(kState);
    if ((ft.dwLowDateTime||ft.dwHighDateTime) && !ft_eq(ft, g.mt_state)) { g.mt_state = ft; reload_state(); }
    ft = get_mtime(kRegistry);
    if ((ft.dwLowDateTime||ft.dwHighDateTime) && !ft_eq(ft, g.mt_reg))   { g.mt_reg   = ft; reload_registry(); }
}

// ── tick principal ──────────────────────────────────────────────────────────────
static void tick_impl() {
    ++g.frame;
    if (!cache_types()) return;

    check_files();
    write_presence();
    if (g.fids.empty()) return;

    if (!g.dst_done && g.frame > 60  && g.frame % 30 == 0) find_dst_all();
    if (!g.bc_done  && g.frame > 80  && g.frame % 30 == 0) read_base_colors();
    if (g.frame % 6 == 0) g.select_gate = select_screen_active();

    if (!g.select_gate) {
        for (int fid : g.fids) {
            auto& fr = g.rt[fid];
            if (fr.slot > 0) {
                keep_mounted(fid);
                if (g.frame % 3 == 0) swap_manifest(fid);
            }
        }
    }

    if (g.frame % 600 == 0) {
        std::string parts;
        for (int fid : g.fids) {
            if (!parts.empty()) parts += ',';
            char b[32]; snprintf(b, sizeof b, "F%d=%d", fid, g.rt[fid].slot); parts += b;
        }
        log_file("heartbeat f%u %s", g.frame, parts.c_str());
    }
}

static void on_late_update() {
    try { tick_impl(); }
    catch (const std::exception& e) { log_error("exception: %s", e.what()); log_file("EXCEPTION: %s", e.what()); }
    catch (...) { log_error("exception inconnue"); log_file("EXCEPTION inconnue"); }
}

} // namespace costume_slots

// ── exports plugin ──────────────────────────────────────────────────────────────
extern "C" __declspec(dllexport)
void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    v->major = 1;
    v->minor = 10;    // API v1.5.8 (minor=10 au tag v1.5.8)
    v->patch = 0;
    v->game_name = nullptr;
}

extern "C" __declspec(dllexport)
bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions) return false;
    try { reframework::API::initialize(param); } catch (...) { return false; }
    if (!param->functions->on_pre_application_entry) return false;

    param->functions->on_pre_application_entry("LateUpdateBehavior", costume_slots::on_late_update);

    costume_slots::reload_registry();
    costume_slots::reload_state();

    costume_slots::log_info("charge (%d persos, %d slots)",
        static_cast<int>(costume_slots::g.fids.size()),
        static_cast<int>(costume_slots::g.paths.size()));
    costume_slots::log_file("plugin charge (%d persos)", static_cast<int>(costume_slots::g.fids.size()));
    return true;
}
