#include "patch.hpp"
#include <string>
#include <cstring>
#include <cctype>

// ============================================================================
// Helpers
// ============================================================================

// Simple in-place search-and-replace for equal-length byte patterns.
static size_t replace_all(std::vector<uint8_t>& data,
                          const uint8_t* pat, const uint8_t* rep, size_t len) {
    if (len == 0 || data.size() < len) return 0;
    size_t count = 0;
    size_t limit = data.size() - len;
    for (size_t i = 0; i <= limit; ) {
        if (std::memcmp(data.data() + i, pat, len) == 0) {
            std::memcpy(data.data() + i, rep, len);
            i += len;
            ++count;
        } else {
            ++i;
        }
    }
    return count;
}

// Encode an ASCII string as UTF-16LE bytes (each char -> 2 bytes).
static std::vector<uint8_t> to_utf16le(const std::string& s) {
    std::vector<uint8_t> v;
    v.reserve(s.size() * 2);
    for (char c : s) {
        v.push_back(static_cast<uint8_t>(c));
        v.push_back(0);
    }
    return v;
}

static std::string to_upper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(
        std::toupper(static_cast<unsigned char>(c)));
    return r;
}

// ============================================================================
// patch_paths
// ============================================================================

size_t patch_paths(std::vector<uint8_t>& data,
                   std::string_view fighter_dir,
                   std::string_view old_folder,
                   std::string_view new_folder) {
    // Build the two replacement pairs
    std::string old_slash = std::string(fighter_dir) + "/" +
                            std::string(old_folder) + "/";
    std::string new_slash = std::string(fighter_dir) + "/" +
                            std::string(new_folder) + "/";

    std::string old_under = std::string(fighter_dir) + "_" +
                            std::string(old_folder) + "_";
    std::string new_under = std::string(fighter_dir) + "_" +
                            std::string(new_folder) + "_";

    struct Pair { std::string old_s, new_s; };
    Pair pairs[] = { {old_slash, new_slash}, {old_under, new_under} };

    size_t total = 0;

    for (auto& [os, ns] : pairs) {
        // -- UTF-16LE, original case --
        auto o16 = to_utf16le(os);
        auto n16 = to_utf16le(ns);
        total += replace_all(data, o16.data(), n16.data(), o16.size());

        // -- UTF-8, original case (ASCII = UTF-8) --
        total += replace_all(data,
                             reinterpret_cast<const uint8_t*>(os.data()),
                             reinterpret_cast<const uint8_t*>(ns.data()),
                             os.size());

        // -- Uppercase variants --
        std::string os_up = to_upper(os);
        std::string ns_up = to_upper(ns);

        auto o16u = to_utf16le(os_up);
        auto n16u = to_utf16le(ns_up);
        total += replace_all(data, o16u.data(), n16u.data(), o16u.size());

        total += replace_all(data,
                             reinterpret_cast<const uint8_t*>(os_up.data()),
                             reinterpret_cast<const uint8_t*>(ns_up.data()),
                             os_up.size());
    }
    return total;
}

// ============================================================================
// patch_scene_minimal  (UTF-16LE scan, mesh/mdf2/CCVD only)
// ============================================================================

static std::string to_lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// Read a null-terminated UTF-16LE string from buf starting at 'start'.
// Returns end position (exclusive, at the null terminator start).
static std::string read_u16_string(const uint8_t* buf, size_t len,
                                   size_t start, size_t* out_end) {
    std::string s;
    size_t i = start;
    while (i + 1 < len) {
        uint16_t c = buf[i] | (uint16_t(buf[i+1]) << 8);
        if (c == 0) { *out_end = i; return s; }
        s += static_cast<char>(c & 0xFF);
        i += 2;
    }
    *out_end = i;
    return s;
}

// Walk backward from pos (UTF-16LE aligned) to find string start
static size_t find_u16_string_start(const uint8_t* buf, size_t pos) {
    size_t s = pos;
    while (s >= 2) {
        if (buf[s - 2] == 0 && buf[s - 1] == 0) break;
        s -= 2;
    }
    return s;
}

static bool ends_with(const std::string& s, const char* suffix) {
    size_t sl = std::strlen(suffix);
    return s.size() >= sl && s.compare(s.size() - sl, sl, suffix) == 0;
}

size_t patch_scene_minimal(std::vector<uint8_t>& data,
                           std::string_view fighter_dir,
                           std::string_view old_folder,
                           std::string_view new_folder,
                           const std::unordered_set<std::string>* relocated_paths,
                           const std::unordered_map<std::string, std::string>* suffixes) {
    auto old_dir_b = to_utf16le(std::string(fighter_dir) + "/" +
                                std::string(old_folder) + "/");
    auto new_dir_b = to_utf16le(std::string(fighter_dir) + "/" +
                                std::string(new_folder) + "/");
    auto old_pfx_b = to_utf16le(std::string(fighter_dir) + "_" +
                                std::string(old_folder) + "_");
    auto new_pfx_b = to_utf16le(std::string(fighter_dir) + "_" +
                                std::string(new_folder) + "_");

    std::string old_dir_s = std::string(fighter_dir) + "/" +
                            std::string(old_folder) + "/";
    std::string new_dir_s = std::string(fighter_dir) + "/" +
                            std::string(new_folder) + "/";
    std::string old_pfx_s = std::string(fighter_dir) + "_" +
                            std::string(old_folder) + "_";
    std::string new_pfx_s = std::string(fighter_dir) + "_" +
                            std::string(new_folder) + "_";

    size_t total = 0;
    size_t dlen = old_dir_b.size();
    size_t plen = old_pfx_b.size();
    // Track per-string occurrence count for CCVD rule
    std::unordered_map<std::string, int> seen_count;

    for (size_t i = 0; i + dlen <= data.size(); ) {
        if (std::memcmp(data.data() + i, old_dir_b.data(), dlen) != 0) {
            i += 2; continue;
        }
        // Found old_dir pattern. Read full enclosing null-terminated UTF-16LE string.
        size_t str_start = find_u16_string_start(data.data(), i);
        size_t str_end;
        std::string full_str = read_u16_string(data.data(), data.size(),
                                                str_start, &str_end);
        std::string low = to_lower(full_str);

        bool is_mesh_mdf2 = ends_with(low, ".mesh") || ends_with(low, ".mdf2");
        bool is_ccvd = low.find("ccvd.user") != std::string::npos;
        bool is_chain_user = low.find("chain.user") != std::string::npos
                          || low.find("havok.user") != std::string::npos;
        bool is_userdata = is_ccvd || is_chain_user;
        bool is_visual = is_mesh_mdf2 || is_userdata;

        if (!is_visual) { i += dlen; continue; }

        // Existence check against relocated set
        if (relocated_paths && suffixes) {
            std::string wb = low;
            { auto p = wb.find(old_dir_s);
              if (p != std::string::npos) wb.replace(p, old_dir_s.size(), new_dir_s); }
            // For CCVD/chain.user: dir-only (filename prefix is semantic)
            if (!is_userdata) {
                auto p = wb.find(old_pfx_s);
                if (p != std::string::npos) wb.replace(p, old_pfx_s.size(), new_pfx_s);
            }
            // Strip leading '@'
            while (!wb.empty() && wb[0] == '@') wb.erase(0, 1);
            while (!wb.empty() && wb[0] == ' ') wb.erase(0, 1);
            // Get extension and build candidate pak path
            std::string bn = wb;
            { auto sl = bn.rfind('/'); if (sl != std::string::npos) bn = bn.substr(sl+1); }
            auto dot = bn.rfind('.');
            std::string ext = (dot != std::string::npos) ? bn.substr(dot+1) : "";
            std::string cand;
            auto sit = suffixes->find(ext);
            if (sit != suffixes->end()) {
                cand = "natives/stm/" + wb + "." + sit->second;
            }
            if (!cand.empty() && relocated_paths->find(cand) == relocated_paths->end()) {
                i += dlen; continue;
            }
        }

        // CCVD occurrence tracking
        std::string norm = low;
        while (!norm.empty() && norm.back() == '\0') norm.pop_back();
        seen_count[norm]++;
        if (is_userdata && seen_count[norm] == 1) {
            i += dlen; continue; // Skip first CCVD occurrence
        }

        // Patch: replace dir segment
        std::memcpy(data.data() + i, new_dir_b.data(), dlen);
        total++;
        // For mesh/mdf2: also replace filename prefix.
        // For CCVD/chain.user: dir-only (the filename prefix is semantic).
        if (!is_userdata) {
            for (size_t j = i; j + plen <= str_end + 40 && j + plen <= data.size(); j += 2) {
                if (std::memcmp(data.data() + j, old_pfx_b.data(), plen) == 0) {
                    std::memcpy(data.data() + j, new_pfx_b.data(), plen);
                    total++;
                    break;
                }
            }
        }
        i += dlen;
    }
    return total;
}

// ============================================================================
// patch_mdf2_minimal  (UTF-16LE scan, mod textures only)
// ============================================================================

size_t patch_mdf2_minimal(std::vector<uint8_t>& data,
                          std::string_view fighter_dir,
                          std::string_view old_folder,
                          std::string_view new_folder,
                          const std::unordered_set<std::string>& mod_tex_stems) {
    auto old_dir = to_utf16le(std::string(fighter_dir) + "/" +
                              std::string(old_folder) + "/");
    auto new_dir = to_utf16le(std::string(fighter_dir) + "/" +
                              std::string(new_folder) + "/");
    size_t dlen = old_dir.size();
    size_t total = 0;

    for (size_t i = 0; i + dlen <= data.size(); ) {
        if (std::memcmp(data.data() + i, old_dir.data(), dlen) != 0) {
            i += 2; continue;
        }
        // Read the rest of the string after old_dir to get the filename
        size_t str_end;
        std::string rest = read_u16_string(data.data(), data.size(),
                                            i + dlen, &str_end);
        // Extract filename (after last /)
        std::string fn = rest;
        auto slash = fn.rfind('/');
        if (slash != std::string::npos) fn = fn.substr(slash + 1);
        // Strip null
        while (!fn.empty() && fn.back() == '\0') fn.pop_back();
        std::string fn_low = to_lower(fn);

        if (mod_tex_stems.count(fn_low)) {
            std::memcpy(data.data() + i, new_dir.data(), dlen);
            total++;
        }
        i += dlen;
    }
    return total;
}
