// Stage-select preview texture builder.
//
// Pipeline: decode PNG/JPEG (stb_image) -> crop to a 4:1 band -> resize to 1920x480 -> write an
// SF6 .tex.241101895 file, uncompressed RGBA8 (sRGB), single mip.
//
// .tex header layout (SF6 / version 241101895, confirmed against the vanilla stage preview
// natives/stm/product/gui/data/area_image/ess/ess0000_00/tex_stageimage_ess0000_00_im.tex.241101895
// extracted from re_chunk_000.pak -- see loader_core.cpp / patch.cpp::texture_mip_check for the
// same layout used elsewhere in this project):
//
//   offset  size  field
//   0       4     magic "TEX\0"
//   4       4     version (int32)            = 241101895
//   8       2     width  (uint16)
//   10      2     height (uint16)
//   12      2     depth  (uint16)             = 1
//   14      1     image_count (uint8)         = 1
//   15      1     mip_header_size (uint8)     = mip_count * 16
//   16      4     format (int32, DXGI)        = 29 (R8G8B8A8_UNORM_SRGB)
//   20      4     swizzle_control (int32)     = -1  (matches vanilla)
//   24      4     cubemap_marker (uint32)     = 0
//   28      4     flags (int32)               = 0x800 (matches vanilla ess0000_00 preview)
//   32      1     swizzle_height_depth (uint8)= 0
//   33      1     swizzle_width (uint8)       = 0
//   34      2     null1 (uint16)              = 0
//   36      2     seven (uint16)              = 0
//   38      2     one (uint16)                = 0
//   40      16*n  mip table: { offset:int64, pitch:int32, size:int32 } per (mip_count*image_count)
//   ...           mip data (raw RGBA8 rows, pitch = width*4)
//
// Format choice: the vanilla preview is BC1_UNORM_SRGB (format 72, block-compressed). This module
// writes UNCOMPRESSED RGBA8 as required by the brief, but keeps the same colour space: format 29
// (R8G8B8A8_UNORM_SRGB), not 28 (UNORM), so colours match what the vanilla sRGB preview shows.
// flags/swizzle_control/cubemap_marker/swizzle_* are copied from the vanilla file rather than
// guessed, since they are independent of pixel format and this is a proven single-mip, single-
// image, non-cubemap texture from the same game build and the same GUI preview pipeline.

#include "preview_tex.hpp"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace {

constexpr int32_t  TEX_VERSION      = 241101895;
constexpr int32_t  TEX_FORMAT_RGBA8_SRGB = 29;   // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
constexpr int32_t  TEX_FLAGS        = 0x800;     // matches vanilla ess0000_00 preview
constexpr int32_t  TEX_SWIZZLE_CTRL = -1;        // matches vanilla
constexpr int      OUT_W = 1920;
constexpr int      OUT_H = 480;                  // OUT_W / 4

// ---------------------------------------------------------------------------
// File I/O (wide path, no CRT stdio file handle held by stb_image itself)
// ---------------------------------------------------------------------------

bool read_whole_file(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    bool ok = false;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        long long sz = _ftelli64(f);
        if (sz > 0 && sz < (1ll << 31) && _fseeki64(f, 0, SEEK_SET) == 0) {
            out.resize((size_t)sz);
            ok = fread(out.data(), 1, out.size(), f) == out.size();
        }
    }
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// Crop rectangle: full width, band height = width/4, centred at 45% of the image
// height, clamped inside the image. If width/4 does not fit inside the image
// height (very wide/panoramic source), the band height is clamped to the image
// height instead of failing -- still a centred crop of the source.
// ---------------------------------------------------------------------------

void compute_crop(int img_w, int img_h, int& crop_y, int& crop_h) {
    int band_h = std::max(1, img_w / 4);
    if (band_h > img_h) band_h = img_h;
    int center_y = (int)std::lround(img_h * 0.45);
    int top = center_y - band_h / 2;
    top = std::max(0, std::min(top, img_h - band_h));
    crop_y = top;
    crop_h = band_h;
}

// ---------------------------------------------------------------------------
// Separable resize, RGBA8, one channel plane at a time. Upscale (dst >= src) uses
// bilinear interpolation; downscale uses a box filter (average over the
// source footprint of each destination pixel) to avoid aliasing.
// ---------------------------------------------------------------------------

// Resizes one channel plane (src_w x src_h, row stride src_w) horizontally to dst_w, writing into
// a temporary plane of dst_w x src_h (row stride dst_w).
void resize_horizontal(const uint8_t* src, int src_w, int h, int stride_in,
                        std::vector<uint8_t>& dst, int dst_w) {
    dst.assign((size_t)dst_w * h, 0);
    const double scale = (double)src_w / dst_w;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = src + (size_t)y * stride_in;
        uint8_t* out_row = dst.data() + (size_t)y * dst_w;
        if (dst_w >= src_w) {
            // Bilinear (upscale or 1:1).
            for (int x = 0; x < dst_w; x++) {
                double sx = (x + 0.5) * scale - 0.5;
                sx = std::max(0.0, std::min(sx, (double)src_w - 1));
                int x0 = (int)std::floor(sx);
                int x1 = std::min(x0 + 1, src_w - 1);
                double t = sx - x0;
                out_row[x] = (uint8_t)std::lround(row[x0] * (1.0 - t) + row[x1] * t);
            }
        } else {
            // Box filter (downscale): average the source footprint of each destination pixel.
            for (int x = 0; x < dst_w; x++) {
                double s0 = x * scale;
                double s1 = (x + 1) * scale;
                int i0 = (int)std::floor(s0);
                int i1 = (int)std::min((double)src_w, std::ceil(s1));
                i1 = std::max(i1, i0 + 1);
                double sum = 0.0, weight = 0.0;
                for (int i = i0; i < i1 && i < src_w; i++) {
                    double lo = std::max((double)i, s0);
                    double hi = std::min((double)i + 1, s1);
                    double w = std::max(0.0, hi - lo);
                    sum += row[i] * w;
                    weight += w;
                }
                out_row[x] = (uint8_t)std::lround(weight > 0 ? sum / weight : row[std::min(i0, src_w - 1)]);
            }
        }
    }
}

// Resizes one channel plane (w x src_h, row stride w) vertically to dst_h.
void resize_vertical(const uint8_t* src, int w, int src_h,
                      std::vector<uint8_t>& dst, int dst_h) {
    dst.assign((size_t)w * dst_h, 0);
    const double scale = (double)src_h / dst_h;
    if (dst_h >= src_h) {
        for (int y = 0; y < dst_h; y++) {
            double sy = (y + 0.5) * scale - 0.5;
            sy = std::max(0.0, std::min(sy, (double)src_h - 1));
            int y0 = (int)std::floor(sy);
            int y1 = std::min(y0 + 1, src_h - 1);
            double t = sy - y0;
            const uint8_t* r0 = src + (size_t)y0 * w;
            const uint8_t* r1 = src + (size_t)y1 * w;
            uint8_t* out_row = dst.data() + (size_t)y * w;
            for (int x = 0; x < w; x++)
                out_row[x] = (uint8_t)std::lround(r0[x] * (1.0 - t) + r1[x] * t);
        }
    } else {
        for (int y = 0; y < dst_h; y++) {
            double s0 = y * scale;
            double s1 = (y + 1) * scale;
            int i0 = (int)std::floor(s0);
            int i1 = (int)std::min((double)src_h, std::ceil(s1));
            i1 = std::max(i1, i0 + 1);
            uint8_t* out_row = dst.data() + (size_t)y * w;
            std::vector<double> sum(w, 0.0);
            double weight = 0.0;
            for (int i = i0; i < i1 && i < src_h; i++) {
                double lo = std::max((double)i, s0);
                double hi = std::min((double)i + 1, s1);
                double wgt = std::max(0.0, hi - lo);
                if (wgt <= 0) continue;
                const uint8_t* row = src + (size_t)i * w;
                for (int x = 0; x < w; x++) sum[x] += row[x] * wgt;
                weight += wgt;
            }
            for (int x = 0; x < w; x++)
                out_row[x] = (uint8_t)std::lround(weight > 0 ? sum[x] / weight : 0.0);
        }
    }
}

// Resizes an RGBA8 image (src_w x src_h) to exactly OUT_W x OUT_H, writing interleaved RGBA into
// out (already sized OUT_W*OUT_H*4). Alpha is forced to 255 regardless of source alpha.
void resize_rgba(const uint8_t* src, int src_w, int src_h, uint8_t* out) {
    // De-interleave into per-channel planes (RGB only; alpha is forced to 255 below).
    std::vector<uint8_t> plane_r((size_t)src_w * src_h), plane_g((size_t)src_w * src_h), plane_b((size_t)src_w * src_h);
    for (size_t i = 0, n = (size_t)src_w * src_h; i < n; i++) {
        plane_r[i] = src[i * 4 + 0];
        plane_g[i] = src[i * 4 + 1];
        plane_b[i] = src[i * 4 + 2];
    }

    std::vector<uint8_t> tmp_r, tmp_g, tmp_b;   // src_h rows x OUT_W cols, after horizontal pass
    resize_horizontal(plane_r.data(), src_w, src_h, src_w, tmp_r, OUT_W);
    resize_horizontal(plane_g.data(), src_w, src_h, src_w, tmp_g, OUT_W);
    resize_horizontal(plane_b.data(), src_w, src_h, src_w, tmp_b, OUT_W);

    std::vector<uint8_t> out_r, out_g, out_b;   // OUT_H rows x OUT_W cols
    resize_vertical(tmp_r.data(), OUT_W, src_h, out_r, OUT_H);
    resize_vertical(tmp_g.data(), OUT_W, src_h, out_g, OUT_H);
    resize_vertical(tmp_b.data(), OUT_W, src_h, out_b, OUT_H);

    for (int i = 0; i < OUT_W * OUT_H; i++) {
        out[i * 4 + 0] = out_r[i];
        out[i * 4 + 1] = out_g[i];
        out[i * 4 + 2] = out_b[i];
        out[i * 4 + 3] = 255;
    }
}

// ---------------------------------------------------------------------------
// .tex header writer
// ---------------------------------------------------------------------------

template <class T>
void put(std::vector<uint8_t>& buf, size_t off, T v) {
    memcpy(buf.data() + off, &v, sizeof(T));
}

void write_tex_header(std::vector<uint8_t>& out, int w, int h, uint32_t pitch, uint32_t data_size) {
    out.resize(56);   // 40-byte header + one 16-byte mip entry
    memset(out.data(), 0, out.size());
    memcpy(out.data() + 0, "TEX\0", 4);
    put<int32_t>(out, 4, TEX_VERSION);
    put<uint16_t>(out, 8, (uint16_t)w);
    put<uint16_t>(out, 10, (uint16_t)h);
    put<uint16_t>(out, 12, 1);              // depth
    out[14] = 1;                            // image_count
    out[15] = 16;                           // mip_header_size (1 mip * 16)
    put<int32_t>(out, 16, TEX_FORMAT_RGBA8_SRGB);
    put<int32_t>(out, 20, TEX_SWIZZLE_CTRL);
    put<uint32_t>(out, 24, 0);              // cubemap_marker
    put<int32_t>(out, 28, TEX_FLAGS);
    out[32] = 0;                            // swizzle_height_depth
    out[33] = 0;                            // swizzle_width
    put<uint16_t>(out, 34, 0);              // null1
    put<uint16_t>(out, 36, 0);              // "seven"
    put<uint16_t>(out, 38, 0);              // "one"
    // mip[0]
    put<int64_t>(out, 40, (int64_t)out.size());
    put<int32_t>(out, 48, (int32_t)pitch);
    put<int32_t>(out, 52, (int32_t)data_size);
}

} // namespace

bool make_stage_preview_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err) {
    out_tex.clear();
    try {
        std::vector<uint8_t> file_data;
        if (!read_whole_file(image_path, file_data) || file_data.empty()) {
            err = "cannot read image file";
            return false;
        }
        if (file_data.size() > (size_t)INT_MAX) {
            err = "image file too large";
            return false;
        }

        int img_w = 0, img_h = 0, img_n = 0;
        uint8_t* pixels = stbi_load_from_memory(file_data.data(), (int)file_data.size(),
                                                 &img_w, &img_h, &img_n, 4 /* force RGBA */);
        if (!pixels) {
            const char* reason = stbi_failure_reason();
            err = std::string("decode failed: ") + (reason ? reason : "unknown");
            return false;
        }
        if (img_w <= 0 || img_h <= 0) {
            stbi_image_free(pixels);
            err = "decoded image has no pixels";
            return false;
        }

        int crop_y = 0, crop_h = 0;
        compute_crop(img_w, img_h, crop_y, crop_h);
        if (crop_h <= 0 || crop_y < 0 || crop_y + crop_h > img_h) {
            stbi_image_free(pixels);
            err = "crop computation failed";
            return false;
        }

        const uint8_t* crop_ptr = pixels + (size_t)crop_y * img_w * 4;

        std::vector<uint8_t> resized((size_t)OUT_W * OUT_H * 4);
        resize_rgba(crop_ptr, img_w, crop_h, resized.data());
        stbi_image_free(pixels);

        const uint32_t pitch = (uint32_t)OUT_W * 4;
        const uint32_t data_size = pitch * (uint32_t)OUT_H;
        if (resized.size() != data_size) {
            err = "internal size mismatch";
            return false;
        }

        write_tex_header(out_tex, OUT_W, OUT_H, pitch, data_size);
        out_tex.insert(out_tex.end(), resized.begin(), resized.end());
        return true;
    } catch (...) {
        err = "unexpected error building preview texture";
        out_tex.clear();
        return false;
    }
}
