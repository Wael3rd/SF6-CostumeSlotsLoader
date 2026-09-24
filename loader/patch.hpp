#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>

// Binary search-and-replace of folder references inside file data.
// Replaces  fighter_dir/old_folder/  ->  fighter_dir/new_folder/
//       and fighter_dir_old_folder_  ->  fighter_dir_new_folder_
// in both UTF-16LE and UTF-8, plus uppercase variants of each.
// old_folder and new_folder must be the same length (3-digit strings).
// Returns the total number of individual replacements made.
size_t patch_paths(std::vector<uint8_t>& data,
                   std::string_view fighter_dir,
                   std::string_view old_folder,
                   std::string_view new_folder);

#include <string>
#include <unordered_set>
#include <unordered_map>

// Minimal scene patching: only mesh/mdf2/CCVD references in UTF-16LE.
// CCVD: patched only at 2nd occurrence (instance data, not userdata_infos).
// relocated_paths: set of pak paths in output (for existence check). May be null.
// suffixes: extension -> version string (e.g. "tex" -> "241101895"). May be null.
size_t patch_scene_minimal(std::vector<uint8_t>& data,
                           std::string_view fighter_dir,
                           std::string_view old_folder,
                           std::string_view new_folder,
                           const std::unordered_set<std::string>* relocated_paths,
                           const std::unordered_map<std::string, std::string>* suffixes);

// Minimal mdf2 patching: only repoint textures whose filename is in mod_tex_stems.
size_t patch_mdf2_minimal(std::vector<uint8_t>& data,
                          std::string_view fighter_dir,
                          std::string_view old_folder,
                          std::string_view new_folder,
                          const std::unordered_set<std::string>& mod_tex_stems);
