// stagetest: runs the stage loader pass on a folder laid out like the game's
// (reframework\stage_mods, reframework\data\SF6_StageSlots_Data\loader\stage_paths.txt),
// without the game. Usage: stagetest <fake game dir>
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../loader/stage_loader.hpp"
#include "../loader/stage_log.hpp"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { fprintf(stderr, "usage: stagetest <fake game dir>\n"); return 2; }
    slog_open(argv[1]);
    std::vector<StageVariant> vars;
    stage_loader_run(argv[1], vars);
    for (auto& v : vars)
        printf("%s  stage %u (%s)  \"%s\"  %zu files%s\n", v.key.c_str(), v.stage_id, v.ess.c_str(), v.name.c_str(),
               v.redirects.size(), v.has_preview ? "  preview" : "");
    return 0;
}
