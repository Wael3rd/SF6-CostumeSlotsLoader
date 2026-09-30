#pragma once

// Stage slots: runtime redirection of the stage files the game asks for.
//
// The engine turns every file path into a pak hash through one function (path_to_hash).
// A detour on it rewrites the path of a stage file to the copy of the chosen variant, so the
// game loads that variant without knowing. Nothing is hooked until a rule is active, and never
// before the game has been running for a while (other tools hook the same function at their
// first frame and scan for it before patching).
//
// Rules come from reframework\data\SF6_StageSlots_Data\redirect.txt, re-read when it changes:
//   <from>=<to>      replace <from> by <to> in every requested path (case-insensitive)
//   trace=<text>     log every requested path containing <text>, without changing it
// Log: SF6_StageSlots.log next to the game executable.

// Starts the watcher thread. Returns at once; safe to call from DllMain.
void stage_slots_start(const wchar_t* game_dir);
