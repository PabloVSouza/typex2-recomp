// kofarc: list/extract SNK "media.kof" archives (KOF XIII), decrypting TXAC
// entries with the game's TypeXmulti/txac key.
//
// Layout: 256-byte directory records {char name[0xF4]; be32 offset; be32 0;
// be32 size}; the directory runs until the first entry's data.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../runtime/src/txac.h"

namespace fs = std::filesystem;

struct Entry {
    std::string name;
    uint32_t off, size;
};

static uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static std::vector<Entry> read_dir(FILE *f) {
    std::vector<Entry> v;
    uint8_t rec[256];
    uint64_t end = UINT64_MAX;
    for (uint64_t pos = 0; pos + 256 <= end; pos += 256) {
        if (fread(rec, 1, 256, f) != 256) break;
        Entry e;
        e.name.assign((const char *)rec, strnlen((const char *)rec, 0xF4));
        e.off = be32(rec + 0xF4);
        e.size = be32(rec + 0xFC);
        if (e.name.empty()) break;
        if (e.off < end) end = e.off;
        v.push_back(e);
    }
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: kofarc list <media.kof>\n"
                "       kofarc extract <media.kof> <out_dir> [filter]\n"
                "TXAC entries are decrypted when <dir of media.kof>/TypeXmulti/txac/*.key exists.\n");
        return 1;
    }
    std::string cmd = argv[1];
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    fs::path game = fs::absolute(argv[2]).parent_path();
    bool have_key = x2_txac_load_from_dir(game.string().c_str());
    auto dir = read_dir(f);
    const char *filter = argc > 4 ? argv[4] : nullptr;
    int enc = 0, bad = 0;
    for (auto &e : dir) {
        if (filter && e.name.find(filter) == std::string::npos) continue;
        std::vector<uint8_t> buf(e.size);
        fseeko(f, e.off, SEEK_SET);
        if (fread(buf.data(), 1, e.size, f) != e.size) { fprintf(stderr, "short read: %s\n", e.name.c_str()); continue; }
        bool txac = e.size >= 4 && !memcmp(buf.data(), "TXAC", 4);
        uint8_t *p = buf.data();
        size_t n = buf.size();
        if (txac) {
            enc++;
            if (!have_key || !x2_txac_decrypt(buf.data(), buf.size(), &p, &n)) {
                bad++;
                p = buf.data();
                n = buf.size();
            }
        }
        if (cmd == "list") {
            printf("%10u %10u %s%s", e.off, e.size, txac ? "[TXAC] " : "", e.name.c_str());
            if (txac && p != buf.data()) {
                printf("  ->");
                for (size_t i = 0; i < 8 && i < n; i++) printf(" %02x", p[i]);
            }
            printf("\n");
        } else if (cmd == "extract") {
            std::string rel = e.name;
            for (auto &ch : rel) if (ch == '\\') ch = '/';
            fs::path out = fs::path(argv[3]) / rel;
            fs::create_directories(out.parent_path());
            FILE *o = fopen(out.string().c_str(), "wb");
            if (!o) { perror(out.string().c_str()); continue; }
            fwrite(p, 1, n, o);
            fclose(o);
        }
    }
    fprintf(stderr, "%zu entries, %d TXAC (%d not decrypted)%s\n", dir.size(), enc, bad, have_key ? "" : ", no key found");
    fclose(f);
    return 0;
}
