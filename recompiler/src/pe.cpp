// Minimal PE32 loader: maps sections into a flat image and reads the import
// and TLS directories. Only what the recompiler needs; no relocations (the
// Type X2 executables are linked at a fixed base without .reloc).
#include <algorithm>
#include <fstream>
#include <stdexcept>

#include "image.hpp"

namespace x2 {

namespace {

template <class T>
T rd(const std::vector<uint8_t>& f, size_t off) {
    if (off + sizeof(T) > f.size()) throw std::runtime_error("PE: read past end of file");
    T v;
    std::memcpy(&v, f.data() + off, sizeof(T));
    return v;
}

std::string cstr(const Image& img, uint32_t va, size_t max = 512) {
    std::string s;
    while (img.in_image(va) && s.size() < max) {
        char ch = (char)img.u8(va++);
        if (!ch) break;
        s += ch;
    }
    return s;
}

}  // namespace

Image load_pe(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), {});

    if (rd<uint16_t>(f, 0) != 0x5A4D) throw std::runtime_error("PE: no MZ header");
    uint32_t nt = rd<uint32_t>(f, 0x3C);
    if (rd<uint32_t>(f, nt) != 0x00004550) throw std::runtime_error("PE: no PE signature");
    uint32_t fh = nt + 4;
    if (rd<uint16_t>(f, fh) != 0x14C) throw std::runtime_error("PE: not i386");
    uint16_t nsec = rd<uint16_t>(f, fh + 2);
    uint16_t opt_size = rd<uint16_t>(f, fh + 16);
    uint32_t oh = fh + 20;
    if (rd<uint16_t>(f, oh) != 0x10B) throw std::runtime_error("PE: not PE32");

    Image img;
    img.path = path;
    img.entry = rd<uint32_t>(f, oh + 16);
    img.base = rd<uint32_t>(f, oh + 28);
    img.size = rd<uint32_t>(f, oh + 56);
    img.entry += img.base;
    uint32_t hdr_size = rd<uint32_t>(f, oh + 60);
    uint32_t ndirs = rd<uint32_t>(f, oh + 92);
    auto dir = [&](uint32_t i) -> std::pair<uint32_t, uint32_t> {
        if (i >= ndirs) return {0, 0};
        return {rd<uint32_t>(f, oh + 96 + i * 8), rd<uint32_t>(f, oh + 100 + i * 8)};
    };

    img.mem.assign(img.size, 0);
    std::memcpy(img.mem.data(), f.data(), std::min<size_t>({hdr_size, f.size(), img.size}));

    uint32_t sh = oh + opt_size;
    for (uint16_t i = 0; i < nsec; i++, sh += 40) {
        char name[9] = {};
        std::memcpy(name, f.data() + sh, 8);
        uint32_t vsize = rd<uint32_t>(f, sh + 8);
        uint32_t rva = rd<uint32_t>(f, sh + 12);
        uint32_t raw_size = rd<uint32_t>(f, sh + 16);
        uint32_t raw_off = rd<uint32_t>(f, sh + 20);
        uint32_t ch = rd<uint32_t>(f, sh + 36);
        if (!vsize) vsize = raw_size;
        if (rva + vsize > img.size) throw std::runtime_error("PE: section outside image");
        size_t n = std::min(vsize, raw_size);
        if (raw_off + n > f.size()) n = f.size() > raw_off ? f.size() - raw_off : 0;
        std::memcpy(img.mem.data() + rva, f.data() + raw_off, n);
        img.sections.push_back({name, img.base + rva, img.base + rva + vsize,
                                (ch & 0x20000000u) != 0, (ch & 0x80000000u) != 0});
    }

    // imports
    auto [imp_rva, imp_size] = dir(1);
    for (uint32_t d = img.base + imp_rva; imp_rva && img.in_image(d + 19); d += 20) {
        uint32_t oft = img.u32(d), name_rva = img.u32(d + 12), ft = img.u32(d + 16);
        if (!oft && !name_rva && !ft) break;
        std::string dll = cstr(img, img.base + name_rva);
        uint32_t lookup = oft ? oft : ft;
        for (uint32_t i = 0;; i++) {
            uint32_t t = img.u32(img.base + lookup + i * 4);
            if (!t) break;
            std::string nm = (t & 0x80000000u) ? "#" + std::to_string(t & 0xFFFF)
                                               : cstr(img, img.base + t + 2);
            img.imports[img.base + ft + i * 4] = {dll, nm};
        }
    }

    // TLS callbacks
    auto [tls_rva, tls_size] = dir(9);
    if (tls_rva) {
        uint32_t cb = img.u32(img.base + tls_rva + 12);
        while (cb && img.in_image(cb)) {
            uint32_t fn = img.u32(cb);
            if (!fn) break;
            img.tls_callbacks.push_back(fn);
            cb += 4;
        }
    }
    return img;
}

}  // namespace x2
