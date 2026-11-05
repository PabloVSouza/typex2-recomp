// Recursive-descent code discovery for x86-32.
//
// Policy, chosen for robustness over minimality:
//  * Functions may overlap / share code. A branch to a known function entry is
//    a tail call; any other branch target is pulled into the current function.
//  * Entries: PE entry, TLS callbacks, direct call targets, code addresses used
//    as immediates in code (push offset fn), dword-aligned pointers in data
//    sections (vtables, callback/SEH tables, thread procs), and 16-aligned
//    starts after int3/nop padding in otherwise-unreached .text.
//  * Undecodable bytes become a trap terminator, so a wrong noreturn guess
//    degrades to a runtime abort instead of a broken build.
#pragma once
#include <capstone/capstone.h>

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "image.hpp"

namespace x2 {

enum class Kind : uint8_t { Seq, Jcc, Jmp, Call, Ret, IJmp, ICall, Trap };

struct MemRef {
    x86_reg base = X86_REG_INVALID, index = X86_REG_INVALID;
    int scale = 0;
    uint32_t disp = 0;
    bool valid = false;
};

struct Insn {
    uint32_t addr = 0;
    uint8_t size = 1;
    Kind kind = Kind::Trap;
    uint32_t target = 0;       // direct branch/call target
    MemRef mem;                // for indirect jmp/call through memory
    std::vector<uint32_t> imms;  // immediates / lea disps that may be code ptrs
    // jump-table bound recovery
    bool is_cmp_imm = false;
    int64_t cmp_imm = 0;
    bool is_and_imm = false;
    int64_t and_imm = 0;
    bool is_movzx_byte_mem = false;
    uint32_t movzx_disp = 0;
};

struct Block {
    uint32_t start = 0;
    std::vector<const Insn*> insns;
    std::vector<uint32_t> succs;
    std::vector<uint32_t> table;  // jump table targets if terminator is indexed ijmp
};

struct Function {
    uint32_t entry = 0;
    std::map<uint32_t, Block> blocks;
};

class Discovery {
public:
    explicit Discovery(const Image& img);
    ~Discovery();

    void run(const std::vector<uint32_t>& extra_entries = {});
    const Insn& decode(uint32_t va);
    const Import* import_of(const Insn& ins) const;
    bool is_noreturn_call(const Insn& ins) const;
    std::set<uint32_t> covered() const;

    const Image& img;
    csh cs = 0;
    std::map<uint32_t, Function> funcs;
    std::set<uint32_t> entries;
    std::vector<std::pair<uint32_t, uint32_t>> data_in_code;  // jump/index tables in .text

private:
    Function explore(uint32_t entry, std::set<uint32_t>& calls);
    bool jump_table(const std::vector<const Insn*>& blk, const Insn& ij, std::vector<uint32_t>& out);
    std::vector<uint32_t> gap_candidates();
    std::set<uint32_t> data_code_pointers() const;

    std::unordered_map<uint32_t, Insn> cache_;
    cs_insn* ci_ = nullptr;
};

}  // namespace x2
