#include "pak.hpp"
#include "patch.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <chrono>

// ============================================================================
// paktool -- CLI exerciser for the pak module
//
//   paktool list   <pak>                   -- header + first 10 entries
//   paktool hash   <path>                  -- print 64-bit pak hash
//   paktool extract <pak> <path> <outfile> -- decompress by path
//   paktool copy   <src.pak> <dst.pak>     -- passthrough copy
// ============================================================================

static void usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  paktool list   <pak>\n"
        "  paktool hash   <path>\n"
        "  paktool extract <pak> <path> <outfile>\n"
        "  paktool copy   <src.pak> <dst.pak>\n");
}

// --------------------------------------------------------------------------

static int cmd_list(const char* pak_path) {
    auto t0 = std::chrono::high_resolution_clock::now();

    PakReader reader;
    if (!reader.open(pak_path)) {
        std::fprintf(stderr, "error: cannot open %s\n", pak_path);
        return 1;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::printf("%s: %zu entries (indexed in %.1f ms)\n",
                pak_path, reader.entry_count(), ms);

    // Print first 10
    int shown = 0;
    for (auto& [hash, e] : reader.entries()) {
        if (shown >= 10) break;
        std::printf("  [%d] hash=%016llX  off=%lld  cs=%lld  ds=%lld  "
                    "att=0x%llX  comp=%d\n",
                    shown,
                    (unsigned long long)hash,
                    (long long)e.offset,
                    (long long)e.compressed_size,
                    (long long)e.decompressed_size,
                    (unsigned long long)e.attributes,
                    e.compression());
        ++shown;
    }
    return 0;
}

// --------------------------------------------------------------------------

static int cmd_hash(const char* path) {
    uint64_t h = pak_path_hash(std::string_view(path));
    std::printf("%016llX\n", (unsigned long long)h);
    return 0;
}

// --------------------------------------------------------------------------

static int cmd_extract(const char* pak_path, const char* file_path,
                       const char* out_path) {
    PakReader reader;
    if (!reader.open(pak_path)) {
        std::fprintf(stderr, "error: cannot open %s\n", pak_path);
        return 1;
    }

    uint64_t h = pak_path_hash(std::string_view(file_path));
    std::printf("hash(%s) = %016llX\n", file_path, (unsigned long long)h);

    const PakEntry* e = reader.find(h);
    if (!e) {
        std::fprintf(stderr, "error: hash not found in pak\n");
        return 1;
    }

    std::printf("found: off=%lld cs=%lld ds=%lld att=0x%llX comp=%d\n",
                (long long)e->offset,
                (long long)e->compressed_size,
                (long long)e->decompressed_size,
                (unsigned long long)e->attributes,
                e->compression());

    auto data = reader.read(*e);
    std::printf("decompressed: %zu bytes\n", data.size());

    FILE* fp = std::fopen(out_path, "wb");
    if (!fp) {
        std::fprintf(stderr, "error: cannot write %s\n", out_path);
        return 1;
    }
    std::fwrite(data.data(), 1, data.size(), fp);
    std::fclose(fp);

    std::printf("wrote %s (%zu bytes)\n", out_path, data.size());
    return 0;
}

// --------------------------------------------------------------------------

static int cmd_copy(const char* src_path, const char* dst_path) {
    auto t0 = std::chrono::high_resolution_clock::now();

    PakReader reader;
    if (!reader.open(src_path)) {
        std::fprintf(stderr, "error: cannot open %s\n", src_path);
        return 1;
    }
    std::printf("source: %zu entries\n", reader.entry_count());

    PakWriter writer;
    for (auto& [hash, e] : reader.entries()) {
        auto raw = reader.read_raw(e);
        writer.add_raw(hash, std::move(raw), e.attributes, e.decompressed_size);
    }

    if (!writer.write(dst_path)) {
        std::fprintf(stderr, "error: cannot write %s\n", dst_path);
        return 1;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::printf("copied %zu entries to %s (%.1f ms)\n",
                writer.entry_count(), dst_path, ms);
    return 0;
}

// --------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(); return 1; }

    const char* cmd = argv[1];

    if (std::strcmp(cmd, "list") == 0 && argc >= 3) {
        return cmd_list(argv[2]);
    }
    if (std::strcmp(cmd, "hash") == 0 && argc >= 3) {
        return cmd_hash(argv[2]);
    }
    if (std::strcmp(cmd, "extract") == 0 && argc >= 5) {
        return cmd_extract(argv[2], argv[3], argv[4]);
    }
    if (std::strcmp(cmd, "copy") == 0 && argc >= 4) {
        return cmd_copy(argv[2], argv[3]);
    }

    // Hidden: extract-hash <pak> <hex-hash> <outfile>
    if (std::strcmp(cmd, "extract-hash") == 0 && argc >= 5) {
        PakReader reader;
        if (!reader.open(argv[2])) {
            std::fprintf(stderr, "error: cannot open %s\n", argv[2]);
            return 1;
        }
        uint64_t h = std::strtoull(argv[3], nullptr, 16);
        const PakEntry* e = reader.find(h);
        if (!e) {
            std::fprintf(stderr, "error: hash %016llX not found\n",
                         (unsigned long long)h);
            return 1;
        }
        std::printf("found: off=%lld cs=%lld ds=%lld att=0x%llX comp=%d\n",
                    (long long)e->offset, (long long)e->compressed_size,
                    (long long)e->decompressed_size,
                    (unsigned long long)e->attributes, e->compression());
        auto data = reader.read(*e);
        std::printf("decompressed: %zu bytes\n", data.size());
        FILE* fp = std::fopen(argv[4], "wb");
        if (!fp) { std::fprintf(stderr, "error: cannot write %s\n", argv[4]); return 1; }
        std::fwrite(data.data(), 1, data.size(), fp);
        std::fclose(fp);
        std::printf("wrote %s\n", argv[4]);
        return 0;
    }

    usage();
    return 1;
}
