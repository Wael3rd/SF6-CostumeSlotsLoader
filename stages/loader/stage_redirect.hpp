#pragma once
#include <vector>

#include "stage_loader.hpp"

// Stage slots, runtime part: serves the selected variant of a stage in place of its files.
//
// The engine turns every file path into a pak hash through one function (path_to_hash). A
// detour on it swaps the hash of a vanilla file the selected variant replaces for the hash its
// copy has in our pak (stage_loader.hpp), so the game loads the variant without knowing.
//
// The selection comes from reframework\data\SF6_StageSlots_Data\state.json, written by the Lua
// script on the stage select screen and re-read when it changes:
//   { "selected": { "<stage id>": "<variant key>", ... } }
// Nothing is hooked while no variant is selected, and never in the first seconds of the game
// (other tools hook the same function at their first frame and scan for it before patching).
// Optional debug.txt in the same folder: "trace=<text>" logs every hashed path holding <text>.
// Log: SF6_StageSlots.log next to the game executable.

// Starts the watcher thread. Returns at once; safe to call from DllMain. `variants` must stay
// alive for the whole session.
void stage_slots_start(const wchar_t* game_dir, const std::vector<StageVariant>* variants);
