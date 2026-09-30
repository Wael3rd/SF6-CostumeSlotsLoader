// stagepak: builds a test pak holding stage mods at relocated paths, and the redirect rules
// that serve them in place of the vanilla files.
//
//   stagepak <out.pak> <out_rules.txt> <SF6_STM.list> <variant_id> <mod.pak> [<variant_id> <mod.pak> ...]
//
// Each file of a mod, natives/stm/<rest>, is stored as natives/stm/_stageslots/<id>/<rest>.
// Paths are resolved from the list (KPKA only stores hashes); unresolved entries are reported
// and left out. The rules file has one "<vanilla path>=<relocated path>" line per file; only
// the files of the first variant get a rule (the watcher serves one variant at a time).

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "pak.hpp"

static const char* MARKER_PATH = "natives/stm/sf6_stage_slots.marker";

static std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

int main(int argc, char** argv) {
    if (argc < 6 || (argc - 4) % 2 != 0) {
        fprintf(stderr, "usage: stagepak <out.pak> <out_rules.txt> <SF6_STM.list> <id> <mod.pak> [<id> <mod.pak> ...]\n");
        return 2;
    }
    std::unordered_map<uint64_t, std::string> names;
    {
        std::ifstream in(argv[3]);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            names[pak_path_hash(std::string_view(line))] = lower(line);
        }
        printf("list: %zu paths\n", names.size());
    }

    PakWriter writer;
    FILE* rules = fopen(argv[2], "wb");
    if (!rules) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    const std::string prefix = "natives/stm/";

    for (int a = 4; a + 1 < argc; a += 2) {
        std::string id = argv[a];
        PakReader rd;
        if (!rd.open(argv[a + 1])) { fprintf(stderr, "cannot open %s\n", argv[a + 1]); return 1; }
        int ok = 0, unknown = 0;
        for (auto& [h, e] : rd.entries()) {
            auto it = names.find(h);
            if (it == names.end() || it->second.compare(0, prefix.size(), prefix) != 0) {
                printf("  [%s] unresolved hash %016llx (%lld bytes)\n", id.c_str(), (unsigned long long)h,
                       (long long)e.decompressed_size);
                ++unknown;
                continue;
            }
            std::string moved = prefix + "_stageslots/" + id + "/" + it->second.substr(prefix.size());
            writer.add_raw(pak_path_hash(std::string_view(moved)), rd.read_raw(e), e.attributes, e.decompressed_size);
            if (a == 4) fprintf(rules, "%s=%s\n", it->second.c_str(), moved.c_str());
            printf("  [%s] %s\n", id.c_str(), it->second.c_str());
            ++ok;
        }
        printf("variant %s: %d files, %d unresolved (%s)\n", id.c_str(), ok, unknown, argv[a + 1]);
    }
    fprintf(rules, "trace=_stageslots\n");
    fclose(rules);

    std::vector<uint8_t> marker(21);
    memcpy(marker.data(), "SF6_StageSlots test\n", 20);
    writer.add_uncompressed(pak_path_hash(std::string_view(MARKER_PATH)), std::move(marker));
    if (!writer.write(argv[1])) { fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
    printf("wrote %s (%zu entries)\n", argv[1], writer.entry_count());
    return 0;
}
