#pragma once

// Turns a stage mod's screenshot (PNG or JPEG) into an SF6 stage-select preview texture
// (.tex.241101895): decode -> crop to 4:1 -> resize to 1920x480 -> encode as an uncompressed
// RGBA8 .tex with a single mip. See preview_tex.cpp for the exact header layout.
//
// SEH-safe: never throws, never lets an exception escape. Runs fine from DllMain (no threads,
// no LoadLibrary, static CRT).

#include <cstdint>
#include <string>
#include <vector>

// image_path: PNG or JPEG file on disk (read with _wfopen, not stb's stdio path, so it works
// with wide/UNC paths under DllMain).
// out_tex: filled with the full .tex file bytes on success.
// err: filled with a short message on failure; untouched on success.
// Returns false on any problem (bad path, decode failure, degenerate image, OOM, ...).
bool make_stage_preview_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err);
