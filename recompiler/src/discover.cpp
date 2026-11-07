#include "discover.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace x2 {

namespace {

// RaiseException is not here: it returns when a handler or debugger continues
// (MSVC's SetThreadName raises 0x406D1388 and carries on, which we emulate)
const std::set<std::string> kNoreturnImports = {
    "ExitProcess", "ExitThread", "TerminateProcess",
    "FatalAppExitA", "FreeLibraryAndExitThread",
};

bool is_jcc(unsigned id) {
    switch (id) {
        case X86_INS_JO: case X86_INS_JNO: case X86_INS_JB: case X86_INS_JAE:
        case X86_INS_JE: case X86_INS_JNE: case X86_INS_JBE: case X86_INS_JA:
        case X86_INS_JS: case X86_INS_JNS: case X86_INS_JP: case X86_INS_JNP:
        case X86_INS_JL: case X86_INS_JGE: case X86_INS_JLE: case X86_INS_JG:
        case X86_INS_JCXZ: case X86_INS_JECXZ: case X86_INS_LOOP:
        case X86_INS_LOOPE: case X86_INS_LOOPNE:
            return true;
        default:
            return false;
    }
}

}  // namespace

Discovery::Discovery(const Image& i) : img(i) {
    if (cs_open(CS_ARCH_X86, CS_MODE_32, &cs) != CS_ERR_OK) throw std::runtime_error("cs_open failed");
    cs_option(cs, CS_OPT_DETAIL, CS_OPT_ON);
    ci_ = cs_malloc(cs);
}

Discovery::~Discovery() {
    cs_free(ci_, 1);
    cs_close(&cs);
}

const Insn& Discovery::decode(uint32_t va) {
    auto it = cache_.find(va);
    if (it != cache_.end()) return it->second;
    Insn ins;
    ins.addr = va;
    const uint8_t* code = img.ptr(va);
    size_t n = std::min<size_t>(img.avail(va), 16);
    uint64_t a = va;
    if (img.is_code(va) && cs_disasm_iter(cs, &code, &n, &a, ci_)) {
        const cs_insn& c = *ci_;
        const cs_x86& x = c.detail->x86;
        ins.size = (uint8_t)c.size;
        unsigned id = c.id;
        ins.kind = Kind::Seq;
        for (int k = 0; k < x.op_count; k++) {
            const cs_x86_op& op = x.operands[k];
            if (op.type == X86_OP_IMM)
                ins.imms.push_back((uint32_t)op.imm);
            else if (op.type == X86_OP_MEM && id == X86_INS_LEA && !op.mem.base && !op.mem.index)
                ins.imms.push_back((uint32_t)op.mem.disp);
        }
        if (id == X86_INS_CMP && x.op_count == 2 && x.operands[1].type == X86_OP_IMM) {
            ins.is_cmp_imm = true;
            ins.cmp_imm = x.operands[1].imm;
        }
        if (id == X86_INS_AND && x.op_count == 2 && x.operands[1].type == X86_OP_IMM && x.operands[0].type == X86_OP_REG) {
            ins.is_and_imm = true;
            ins.and_imm = x.operands[1].imm;
        }
        if (id == X86_INS_MOVZX && x.op_count == 2 && x.operands[1].type == X86_OP_MEM &&
            x.operands[1].size == 1 && x.operands[1].mem.disp) {
            ins.is_movzx_byte_mem = true;
            ins.movzx_disp = (uint32_t)x.operands[1].mem.disp;
        }
        bool branch = id == X86_INS_CALL || id == X86_INS_JMP || is_jcc(id);
        if (id == X86_INS_RET || id == X86_INS_RETF || id == X86_INS_IRET || id == X86_INS_IRETD) {
            ins.kind = Kind::Ret;
        } else if (id == X86_INS_INT3 || id == X86_INS_HLT || id == X86_INS_UD2 || id == X86_INS_LJMP ||
                   id == X86_INS_LCALL || id == X86_INS_INTO) {
            ins.kind = Kind::Trap;
        } else if (branch) {
            const cs_x86_op& op = x.operands[0];
            bool direct = op.type == X86_OP_IMM;
            if (direct) {
                ins.target = (uint32_t)op.imm;
                ins.imms.clear();
            } else if (op.type == X86_OP_MEM) {
                ins.mem = {op.mem.base, op.mem.index, op.mem.scale, (uint32_t)op.mem.disp, true};
            }
            if (id == X86_INS_CALL) ins.kind = direct ? Kind::Call : Kind::ICall;
            else if (id == X86_INS_JMP) ins.kind = direct ? Kind::Jmp : Kind::IJmp;
            else ins.kind = Kind::Jcc;
        }
    }
    return cache_.emplace(va, std::move(ins)).first->second;
}

const Import* Discovery::import_of(const Insn& ins) const {
    if (!ins.mem.valid || ins.mem.base || ins.mem.index) return nullptr;
    auto it = img.imports.find(ins.mem.disp);
    return it == img.imports.end() ? nullptr : &it->second;
}

bool Discovery::is_noreturn_call(const Insn& ins) const {
    if (ins.kind != Kind::ICall) return false;
    auto* imp = import_of(ins);
    return imp && kNoreturnImports.count(imp->name);
}

bool Discovery::jump_table(const std::vector<const Insn*>& blk, const Insn& ij, std::vector<uint32_t>& out) {
    const MemRef& m = ij.mem;
    if (m.base || !m.index || m.scale != 4) return false;
    int64_t bound = -1;
    int64_t byte_table = -1;
    for (int k = (int)blk.size() - 2; k >= 0; k--) {
        const Insn& p = *blk[k];
        if (p.is_movzx_byte_mem && byte_table < 0) byte_table = p.movzx_disp;
        if (p.is_cmp_imm) {
            bound = p.cmp_imm + 1;
            break;
        }
        if (p.is_and_imm && p.and_imm >= 0 && p.and_imm < 256) {
            bound = p.and_imm + 1;
            break;
        }
    }
    int64_t n = -1;
    if (byte_table >= 0 && bound > 0 && bound < 65536 && img.in_image((uint32_t)byte_table)) {
        int mx = 0;
        for (int64_t k = 0; k < bound; k++) mx = std::max<int>(mx, img.u8((uint32_t)(byte_table + k)));
        n = mx + 1;
        data_in_code.push_back({(uint32_t)byte_table, (uint32_t)(byte_table + bound)});
    } else if (bound > 0 && bound < 65536) {
        n = bound;
    }
    for (int64_t k = 0; n < 0 || k < n; k++) {
        uint32_t slot = m.disp + 4 * (uint32_t)k;
        if (!img.in_image(slot)) break;
        uint32_t t = img.u32(slot);
        if (k > 4096) break;
        if (!img.is_code(t)) {
            if (n < 0) break;  // unbounded: the first non-code entry ends the table
            continue;          // bounded: unused slots (e.g. index 0 of `and eax,3`) may hold anything
        }
        out.push_back(t);
    }
    // Negative indices (e.g. MSVC memmove's `jmp [ecx*4+TrailDownVec]` with
    // ecx in -7..0): code pointers directly below the base belong to the table.
    uint32_t lo = m.disp;
    for (int k = 1; k <= 16; k++) {
        uint32_t slot = m.disp - 4 * (uint32_t)k;
        if (!img.in_image(slot)) break;
        uint32_t t = img.u32(slot);
        if (!img.is_code(t) || t == slot) break;
        out.push_back(t);
        lo = slot;
    }
    if (out.empty()) return false;
    if (lo < m.disp) data_in_code.push_back({lo, m.disp});
    data_in_code.push_back({m.disp, m.disp + 4 * (uint32_t)(n > 0 ? n : (int64_t)out.size())});
    return true;
}

Function Discovery::explore(uint32_t entry, std::set<uint32_t>& calls) {
    Function fn;
    fn.entry = entry;
    std::vector<uint32_t> work{entry};
    while (!work.empty()) {
        uint32_t start = work.back();
        work.pop_back();
        if (fn.blocks.count(start)) continue;
        Block blk;
        blk.start = start;
        uint32_t va = start;
        for (;;) {
            const Insn& ins = decode(va);
            blk.insns.push_back(&ins);
            uint32_t nxt = va + ins.size;
            bool stop = true;
            switch (ins.kind) {
                case Kind::Seq:
                    stop = false;
                    break;
                case Kind::Call:
                    if (img.is_code(ins.target)) calls.insert(ins.target);
                    stop = false;
                    break;
                case Kind::ICall:
                    stop = is_noreturn_call(ins);
                    break;
                case Kind::Jcc:
                    blk.succs.push_back(ins.target);
                    blk.succs.push_back(nxt);
                    break;
                case Kind::Jmp:
                    if (!(entries.count(ins.target) && ins.target != entry)) blk.succs.push_back(ins.target);
                    break;
                case Kind::IJmp:
                    if (ins.mem.valid && jump_table(blk.insns, ins, blk.table)) {
                        std::set<uint32_t> uniq(blk.table.begin(), blk.table.end());
                        blk.succs.insert(blk.succs.end(), uniq.begin(), uniq.end());
                    }
                    break;
                default:
                    break;
            }
            if (stop) break;
            va = nxt;
            if (fn.blocks.count(nxt)) {  // ran into a known block: split here
                blk.succs.push_back(nxt);
                break;
            }
        }
        for (uint32_t s : blk.succs)
            if (!fn.blocks.count(s) && s != start) work.push_back(s);
        fn.blocks.emplace(start, std::move(blk));
    }
    return fn;
}

std::set<uint32_t> Discovery::data_code_pointers() const {
    std::set<uint32_t> out;
    for (auto& s : img.sections) {
        if (s.exec) continue;
        for (uint32_t va = s.start; va + 4 <= s.end; va += 4) {
            uint32_t v = img.u32(va);
            if (img.is_code(v)) out.insert(v);
        }
    }
    return out;
}

std::set<uint32_t> Discovery::covered() const {
    std::set<uint32_t> cov;
    for (auto& [e, fn] : funcs)
        for (auto& [s, b] : fn.blocks)
            for (auto* i : b.insns)
                for (uint32_t a = i->addr; a < i->addr + i->size; a++) cov.insert(a);
    for (auto [a, b] : data_in_code)
        for (uint32_t x = a; x < b; x++) cov.insert(x);
    return cov;
}

std::vector<uint32_t> Discovery::gap_candidates() {
    // byte-granular coverage bitmap over executable sections
    std::vector<uint32_t> out;
    std::vector<bool> cov(img.size, false);
    for (auto& [e, fn] : funcs)
        for (auto& [s, b] : fn.blocks)
            for (auto* i : b.insns)
                for (uint32_t a = i->addr; a < i->addr + i->size && img.in_image(a); a++) cov[a - img.base] = true;
    for (auto [a, b] : data_in_code)
        for (uint32_t x = a; x < b && img.in_image(x); x++) cov[x - img.base] = true;
    for (auto& s : img.sections) {
        if (!s.exec) continue;
        uint32_t va = s.start;
        while (va < s.end) {
            if (cov[va - img.base]) { va++; continue; }
            uint32_t end = va;
            while (end < s.end && !cov[end - img.base]) end++;
            for (uint32_t a = va; a < end; a++) {
                uint8_t b = img.u8(a);
                if (b == 0xCC || b == 0x90 || b == 0x00 || (a & 15)) continue;
                uint8_t prev = img.u8(a - 1);
                if ((prev == 0xCC || prev == 0x90 || cov[a - 1 - img.base]) && !entries.count(a)) out.push_back(a);
                break;
            }
            va = end;
        }
    }
    return out;
}

void Discovery::run(const std::vector<uint32_t>& extra) {
    std::vector<uint32_t> work{img.entry};
    work.insert(work.end(), img.tls_callbacks.begin(), img.tls_callbacks.end());
    work.insert(work.end(), extra.begin(), extra.end());
    auto ptrs = data_code_pointers();
    work.insert(work.end(), ptrs.begin(), ptrs.end());
    entries.clear();
    funcs.clear();
    for (;;) {
        while (!work.empty()) {
            uint32_t e = work.back();
            work.pop_back();
            if (entries.count(e) || !img.is_code(e) || decode(e).kind == Kind::Trap) continue;
            entries.insert(e);
            std::set<uint32_t> calls;
            Function fn = explore(e, calls);
            for (auto& [s, b] : fn.blocks)
                for (auto* ins : b.insns)
                    if (ins->kind == Kind::Seq)
                        for (uint32_t v : ins->imms)
                            if (img.is_code(v) && !entries.count(v)) work.push_back(v);
            for (uint32_t c : calls)
                if (!entries.count(c)) work.push_back(c);
            funcs[e] = std::move(fn);
        }
        work = gap_candidates();
        if (work.empty()) break;
    }
    // Re-explore with the final entry set so jumps into other functions become
    // tail calls rather than inlined copies.
    for (auto& [e, fn] : funcs) {
        std::set<uint32_t> calls;
        fn = explore(e, calls);
    }
}

}  // namespace x2
