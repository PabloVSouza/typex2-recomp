// List / extract SNK Type X2 `gamediskimg.arc` archives (KOF XII / XIII era NGLU engine).
//
// Format (little-endian):
//     u32 count
//     count x { u32 name_len; char name[name_len]; u32 offset; u32 size; u32 flags }
//     file data (offsets are absolute)
//
// `*.z` entries are zlib-compressed:
//     u32 raw_total; repeat { u32 raw_chunk; u32 comp_chunk; u8 zlib[comp_chunk] }
// `extract --decompress` writes them without the `.z` suffix.
//
// Usage:
//     arc list    <gamediskimg.arc>
//     arc extract [--decompress] <gamediskimg.arc> <out_dir> [substring-filter]
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Entry {
    std::string name;
    uint32_t off, size, flags;
};

static uint32_t rd32(std::ifstream& f) {
    uint32_t v;
    if (!f.read(reinterpret_cast<char*>(&v), 4)) throw std::runtime_error("truncated index");
    return v;
}

static std::vector<Entry> read_index(std::ifstream& f) {
    uint32_t count = rd32(f);
    std::vector<Entry> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t n = rd32(f);
        if (n > 4096) throw std::runtime_error("bad name length");
        std::string name(n, '\0');
        f.read(name.data(), n);
        uint32_t off = rd32(f), size = rd32(f), flags = rd32(f);
        out.push_back({name, off, size, flags});
    }
    return out;
}

static std::vector<uint8_t> decompress_z(const std::vector<uint8_t>& d) {
    auto u32at = [&](size_t p) {
        if (p + 4 > d.size()) throw std::runtime_error("truncated .z stream");
        uint32_t v;
        std::memcpy(&v, d.data() + p, 4);
        return v;
    };
    uint32_t total = u32at(0);
    std::vector<uint8_t> out;
    out.reserve(total);
    size_t pos = 4;
    while (out.size() < total && pos < d.size()) {
        uint32_t raw = u32at(pos), comp = u32at(pos + 4);
        pos += 8;
        if (pos + comp > d.size()) throw std::runtime_error("truncated .z chunk");
        size_t at = out.size();
        out.resize(at + raw);
        uLongf got = raw;
        if (uncompress(out.data() + at, &got, d.data() + pos, comp) != Z_OK || got != raw)
            throw std::runtime_error("zlib chunk error");
        pos += comp;
    }
    if (out.size() != total) throw std::runtime_error("total size mismatch");
    return out;
}

// Refuses names that would escape out_dir.
static fs::path safe_join(const fs::path& root, std::string name) {
    for (auto& ch : name)
        if (ch == '\\') ch = '/';
    fs::path p = (root / name).lexically_normal();
    auto rel = p.lexically_relative(root);
    if (rel.empty() || *rel.begin() == ".." || p.is_absolute() != root.is_absolute())
        throw std::runtime_error("unsafe path in archive: " + name);
    return p;
}

int main(int argc, char** argv) {
    std::vector<std::string> args;
    bool decompress = false;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--decompress") decompress = true;
        else args.push_back(argv[i]);
    }
    if (args.size() < 2 || (args[0] != "list" && args[0] != "extract" && args[0] != "locate") ||
        (args[0] == "extract" && args.size() < 3)) {
        std::fprintf(stderr,
                     "usage: arc list <gamediskimg.arc>\n"
                     "       arc extract [--decompress] <gamediskimg.arc> <out_dir> [filter]\n"
                     "       arc locate <gamediskimg.arc>   (hex offsets on stdin -> entry names, first hit order)\n");
        return 1;
    }
    try {
        std::ifstream f(args[1], std::ios::binary);
        if (!f) throw std::runtime_error("cannot open " + args[1]);
        auto entries = read_index(f);
        if (args[0] == "locate") {
            std::vector<bool> seen(entries.size(), false);
            char line[128];
            while (std::fgets(line, sizeof line, stdin)) {
                unsigned long long off = std::strtoull(line, nullptr, 16);
                for (size_t i = 0; i < entries.size(); i++) {
                    auto& e = entries[i];
                    if (off >= e.off && off < (unsigned long long)e.off + e.size) {
                        if (!seen[i]) std::printf("%s\n", e.name.c_str());
                        seen[i] = true;
                        break;
                    }
                }
            }
            return 0;
        }
        if (args[0] == "list") {
            for (auto& e : entries) std::printf("0x%08x %10u 0x%04x %s\n", e.off, e.size, e.flags, e.name.c_str());
            std::fprintf(stderr, "# %zu entries\n", entries.size());
            return 0;
        }
        fs::path out = fs::absolute(args[2]).lexically_normal();
        std::string filt = args.size() > 3 ? args[3] : "";
        size_t n = 0;
        for (auto& e : entries) {
            if (!filt.empty() && e.name.find(filt) == std::string::npos) continue;
            std::vector<uint8_t> data(e.size);
            f.seekg(e.off);
            if (!f.read(reinterpret_cast<char*>(data.data()), e.size)) throw std::runtime_error("truncated: " + e.name);
            std::string name = e.name;
            if (decompress && name.size() > 2 && name.ends_with(".z")) {
                data = decompress_z(data);
                name.resize(name.size() - 2);
            }
            fs::path p = safe_join(out, name);
            fs::create_directories(p.parent_path());
            std::ofstream o(p, std::ios::binary);
            o.write(reinterpret_cast<const char*>(data.data()), data.size());
            n++;
        }
        std::fprintf(stderr, "extracted %zu files to %s\n", n, out.c_str());
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "arc: %s\n", ex.what());
        return 1;
    }
    return 0;
}
