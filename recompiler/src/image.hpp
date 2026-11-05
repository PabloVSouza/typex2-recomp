// PE32 image model used by every recompiler pass.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace x2 {

struct Section {
    std::string name;
    uint32_t start = 0, end = 0;  // VA range, end exclusive (virtual size)
    bool exec = false, write = false;
};

struct Import {
    std::string dll, name;  // name is "#<ordinal>" for ordinal imports
};

struct Image {
    std::string path;
    uint32_t base = 0, size = 0, entry = 0;
    std::vector<uint8_t> mem;  // indexed by VA - base
    std::vector<Section> sections;
    std::map<uint32_t, Import> imports;  // IAT slot VA -> import
    std::vector<uint32_t> tls_callbacks;

    bool in_image(uint32_t va) const { return va >= base && va - base < size; }
    const Section* section_at(uint32_t va) const {
        for (auto& s : sections)
            if (va >= s.start && va < s.end) return &s;
        return nullptr;
    }
    bool is_code(uint32_t va) const {
        auto* s = section_at(va);
        return s && s->exec;
    }
    const uint8_t* ptr(uint32_t va) const { return mem.data() + (va - base); }
    // bytes available from va to the end of the image (for decoding)
    size_t avail(uint32_t va) const { return in_image(va) ? size - (va - base) : 0; }
    uint8_t u8(uint32_t va) const { return in_image(va) ? *ptr(va) : 0; }
    uint32_t u32(uint32_t va) const {
        if (!in_image(va) || avail(va) < 4) return 0;
        uint32_t v;
        std::memcpy(&v, ptr(va), 4);
        return v;
    }
};

// Throws std::runtime_error on malformed input.
Image load_pe(const std::string& path);

}  // namespace x2
