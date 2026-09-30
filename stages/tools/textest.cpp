// textest: offline test harness for loader/preview_tex.{hpp,cpp}.
//
//   textest <in.png|in.jpg> <out.tex>     encode a stage screenshot into an SF6 preview .tex
//   textest --dump <in.tex> <out.png>     decode our own uncompressed RGBA8 .tex back to PNG,
//                                         for a visual sanity check of the crop/resize (this
//                                         reader only understands the exact layout preview_tex.cpp
//                                         writes: single image, single mip, format 28/29 RGBA8).
//
// No in-game testing here; this only validates the module offline (header fields, dimensions,
// and a visual round-trip of the pixels).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../loader/preview_tex.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../loader/third_party/stb_image_write.h"

namespace {

template <class T>
T get(const std::vector<uint8_t>& b, size_t off) {
    T v{};
    memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

std::wstring widen(const char* s) {
    std::wstring w;
    while (*s) w += (wchar_t)(unsigned char)*s++;
    return w;
}

int do_encode(const char* in_path, const char* out_path) {
    std::vector<uint8_t> tex;
    std::string err;
    if (!make_stage_preview_tex(widen(in_path), tex, err)) {
        fprintf(stderr, "encode failed: %s\n", err.c_str());
        return 1;
    }
    FILE* f = fopen(out_path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", out_path); return 1; }
    fwrite(tex.data(), 1, tex.size(), f);
    fclose(f);

    uint16_t w = get<uint16_t>(tex, 8), h = get<uint16_t>(tex, 10);
    uint8_t image_count = tex[14], mip_header_size = tex[15];
    int32_t format = get<int32_t>(tex, 16), flags = get<int32_t>(tex, 28);
    int32_t swizzle = get<int32_t>(tex, 20);
    int64_t mip_off = get<int64_t>(tex, 40);
    int32_t pitch = get<int32_t>(tex, 48), size = get<int32_t>(tex, 52);
    printf("OK: %s (%zu bytes)\n", out_path, tex.size());
    printf("  %ux%u format=%d flags=0x%X swizzle_control=%d image_count=%u mip_count=%u\n",
           w, h, format, flags, swizzle, image_count, mip_header_size / 16);
    printf("  mip[0] offset=%lld pitch=%d size=%d\n", (long long)mip_off, pitch, size);
    return 0;
}

int do_dump(const char* in_path, const char* out_path) {
    FILE* f = fopen(in_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", in_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data((size_t)sz);
    if (fread(data.data(), 1, data.size(), f) != data.size()) { fclose(f); fprintf(stderr, "read error\n"); return 1; }
    fclose(f);

    if (data.size() < 56 || memcmp(data.data(), "TEX\0", 4) != 0) { fprintf(stderr, "not a TEX file\n"); return 1; }
    int32_t format = get<int32_t>(data, 16);
    if (format != 28 && format != 29) { fprintf(stderr, "unsupported format %d (this dumper only reads uncompressed RGBA8)\n", format); return 1; }
    uint16_t w = get<uint16_t>(data, 8), h = get<uint16_t>(data, 10);
    int64_t mip_off = get<int64_t>(data, 40);
    int32_t pitch = get<int32_t>(data, 48), size = get<int32_t>(data, 52);
    if (mip_off < 0 || (size_t)mip_off + (size_t)size > data.size() || pitch < w * 4) {
        fprintf(stderr, "inconsistent mip table\n");
        return 1;
    }
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy(rgba.data() + (size_t)y * w * 4, data.data() + mip_off + (size_t)y * pitch, (size_t)w * 4);

    if (!stbi_write_png(out_path, w, h, 4, rgba.data(), w * 4)) {
        fprintf(stderr, "png write failed\n");
        return 1;
    }
    printf("OK: %s (%ux%u, from %s)\n", out_path, w, h, in_path);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "--dump") == 0) {
        return do_dump(argv[2], argv[3]);
    }
    if (argc == 3) {
        return do_encode(argv[1], argv[2]);
    }
    fprintf(stderr, "usage:\n  textest <in.png|in.jpg> <out.tex>\n  textest --dump <in.tex> <out.png>\n");
    return 2;
}
