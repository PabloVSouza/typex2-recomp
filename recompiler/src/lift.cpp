#include "lift.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <unordered_map>

using std::format;
using std::string;
using std::vector;

namespace x2 {

namespace {

const char* kR32[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
const std::unordered_map<string, string> kR16 = {
    {"ax", "eax"}, {"cx", "ecx"}, {"dx", "edx"}, {"bx", "ebx"},
    {"sp", "esp"}, {"bp", "ebp"}, {"si", "esi"}, {"di", "edi"}};
const std::unordered_map<string, string> kR8L = {{"al", "eax"}, {"cl", "ecx"}, {"dl", "edx"}, {"bl", "ebx"}};
const std::unordered_map<string, string> kR8H = {{"ah", "eax"}, {"ch", "ecx"}, {"dh", "edx"}, {"bh", "ebx"}};

const char* UT(int s) { return s == 1 ? "u8" : s == 2 ? "u16" : s == 8 ? "u64" : "u32"; }
const char* STy(int s) { return s == 1 ? "s8" : s == 2 ? "s16" : s == 8 ? "s64" : "s32"; }
uint64_t MASK(int s) { return s >= 8 ? ~0ull : (1ull << (s * 8)) - 1; }

const std::unordered_map<string, string> kCond = {
    {"o", "of"}, {"no", "!of"}, {"b", "cf"}, {"ae", "!cf"}, {"e", "zf"}, {"ne", "!zf"},
    {"be", "(cf|zf)"}, {"a", "(!cf&&!zf)"}, {"s", "sf"}, {"ns", "!sf"}, {"p", "PF()"},
    {"np", "!PF()"}, {"l", "(sf!=of)"}, {"ge", "(sf==of)"}, {"le", "(zf||sf!=of)"},
    {"g", "(!zf&&sf==of)"}};

bool is_r32(const string& n) {
    for (auto* r : kR32)
        if (n == r) return true;
    return false;
}

string join(const vector<string>& v, const char* sep = " ") {
    string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? sep : "") + v[i];
    return s;
}

const char* SPILL = "SPILL();";
const char* RELOAD = "RELOAD();";

}  // namespace

Lifter::Lifter(const Image& img, Discovery& disc) : img_(img), disc_(disc) {
    cs_open(CS_ARCH_X86, CS_MODE_32, &cs_);
    cs_option(cs_, CS_OPT_DETAIL, CS_OPT_ON);
    ci_ = cs_malloc(cs_);
}

Lifter::~Lifter() {
    cs_free(ci_, 1);
    cs_close(&cs_);
}

// ------------------------------------------------------------------ operands
string Lifter::rn(unsigned reg) const { return cs_reg_name(cs_, reg); }

string Lifter::ea(const cs_x86_op& op, bool with_seg) const {
    const x86_op_mem& m = op.mem;
    vector<string> parts;
    auto r32 = [&](unsigned r) {
        string n = rn(r);
        if (!is_r32(n)) throw Unsupported{"addr reg " + n};
        return n;
    };
    if (m.base) parts.push_back(r32(m.base));
    if (m.index) parts.push_back(m.scale == 1 ? r32(m.index) : format("{}*{}", r32(m.index), m.scale));
    uint32_t disp = (uint32_t)m.disp;
    if (disp || parts.empty()) parts.push_back(format("0x{:x}u", disp));
    string e = join(parts, "+");
    if (with_seg && m.segment == X86_REG_FS) e = "c->fs_base+" + e;
    if (with_seg && m.segment == X86_REG_GS) throw Unsupported{"gs segment"};
    return format("(u32)({})", e);
}

string Lifter::rd(const cs_x86_op& op, int size) const {
    if (!size) size = op.size;
    if (op.type == X86_OP_REG) {
        string n = rn(op.reg);
        if (is_r32(n)) return n;
        if (auto it = kR16.find(n); it != kR16.end()) return "(u16)" + it->second;
        if (auto it = kR8L.find(n); it != kR8L.end()) return "(u8)" + it->second;
        if (auto it = kR8H.find(n); it != kR8H.end()) return format("(u8)({}>>8)", it->second);
        throw Unsupported{"reg " + n};
    }
    if (op.type == X86_OP_IMM) return format("0x{:x}u", (uint64_t)op.imm & MASK(size));
    if (op.type == X86_OP_MEM) return format("RD{}({})", size * 8, ea(op));
    throw Unsupported{"operand"};
}

string Lifter::wr(const cs_x86_op& op, const string& val) const {
    if (op.type == X86_OP_REG) {
        string n = rn(op.reg);
        if (is_r32(n)) return format("{}=(u32)({});", n, val);
        if (auto it = kR16.find(n); it != kR16.end()) return format("SET16({},{});", it->second, val);
        if (auto it = kR8L.find(n); it != kR8L.end()) return format("SET8L({},{});", it->second, val);
        if (auto it = kR8H.find(n); it != kR8H.end()) return format("SET8H({},{});", it->second, val);
        throw Unsupported{"reg " + n};
    }
    if (op.type == X86_OP_MEM) return format("WR{}({},{});", op.size * 8, ea(op), val);
    throw Unsupported{"dst operand"};
}

// ------------------------------------------------------------------ emission

// Accesses through esp are thread-private: rewrite RD32(esp.. / WR8((u32)(esp.. to the
// plain RDS/WRS forms so only shared memory pays for x86 ordering (X2_TSO).
static void stack_plain(string& l) {
    for (size_t p = 0; (p = l.find_first_of("RW", p)) != string::npos; p++) {
        if (l.compare(p, 2, "RD") && l.compare(p, 2, "WR")) continue;
        if (p > 0 && (isalnum((unsigned char)l[p - 1]) || l[p - 1] == '_')) continue;
        size_t q = p + 2;
        if (q < l.size() && l[q] == 'F') q++;
        size_t digits = q;
        while (q < l.size() && isdigit((unsigned char)l[q])) q++;
        if (q == digits || q >= l.size() || l[q] != '(') continue;
        size_t r = q + 1;
        if (!l.compare(r, 6, "(u32)(")) r += 6;
        if (l.compare(r, 3, "esp") || (r + 3 < l.size() && (isalnum((unsigned char)l[r + 3]) || l[r + 3] == '_')))
            continue;
        l.insert(p + 2, "S");
        p = q + 1;
    }
}
string Lifter::branch_to(uint32_t t, const Function& fn) const {
    if (fn.blocks.count(t)) {
        // loop back-edge: give the runtime a chance to yield (GIL) and keep
        // spin-wait loops from being optimized into a single load
        if (t <= cur_addr_) return format("X2_YIELD(); goto L_{:08x};", t);
        return format("goto L_{:08x};", t);
    }
    if (disc_.funcs.count(t)) return format("{} f_{:08x}(c); return;", SPILL, t);
    return format("{} x2_dispatch_jmp(c,0x{:x}u); return;", SPILL, t);
}

// ------------------------------------------------------------ flags across calls
// Flags are C locals, so they do not survive a call. Compiled code never needs
// them to, but hand-written runtime asm does (MSVC's x87 math: "call _load_arg"
// sets ZF, the caller branches on it). Find the callees whose flags are read
// after the call and make them hand the flags back through X2Cpu.
namespace {
enum : unsigned { F_CF = 1, F_ZF = 2, F_SF = 4, F_OF = 8, F_PF = 16, F_AF = 32, F_ALL = 63 };
struct FlagUse {
    unsigned test = 0, write = 0;
};
FlagUse flag_use(const cs_insn& d) {
    FlagUse u;
    const cs_detail* det = d.detail;
    bool fpu = false;
    for (uint8_t g = 0; g < det->groups_count; g++) fpu |= det->groups[g] == X86_GRP_FPU;
    if (fpu) {  // detail holds FPU flags instead of EFLAGS
        std::string m = d.mnemonic;
        if (m.rfind("fcomi", 0) == 0 || m.rfind("fucomi", 0) == 0) u.write = F_ZF | F_PF | F_CF;
        if (m.rfind("fcmov", 0) == 0) u.test = F_CF | F_ZF | F_PF;
        return u;
    }
    uint64_t e = det->x86.eflags;
    struct { uint64_t test, mod, reset, set, undef; unsigned bit; } map[] = {
        {X86_EFLAGS_TEST_CF, X86_EFLAGS_MODIFY_CF, X86_EFLAGS_RESET_CF, X86_EFLAGS_SET_CF, X86_EFLAGS_UNDEFINED_CF, F_CF},
        {X86_EFLAGS_TEST_ZF, X86_EFLAGS_MODIFY_ZF, X86_EFLAGS_RESET_ZF, X86_EFLAGS_SET_ZF, X86_EFLAGS_UNDEFINED_ZF, F_ZF},
        {X86_EFLAGS_TEST_SF, X86_EFLAGS_MODIFY_SF, X86_EFLAGS_RESET_SF, X86_EFLAGS_SET_SF, X86_EFLAGS_UNDEFINED_SF, F_SF},
        {X86_EFLAGS_TEST_OF, X86_EFLAGS_MODIFY_OF, X86_EFLAGS_RESET_OF, X86_EFLAGS_SET_OF, X86_EFLAGS_UNDEFINED_OF, F_OF},
        {X86_EFLAGS_TEST_PF, X86_EFLAGS_MODIFY_PF, X86_EFLAGS_RESET_PF, X86_EFLAGS_SET_PF, X86_EFLAGS_UNDEFINED_PF, F_PF},
        {X86_EFLAGS_TEST_AF, X86_EFLAGS_MODIFY_AF, X86_EFLAGS_RESET_AF, X86_EFLAGS_SET_AF, X86_EFLAGS_UNDEFINED_AF, F_AF},
    };
    for (auto& f : map) {
        if (e & f.test) u.test |= f.bit;
        if (e & (f.mod | f.reset | f.set | f.undef)) u.write |= f.bit;
    }
    return u;
}
}  // namespace

bool Lifter::decode(uint32_t va) {
    const uint8_t* code = img_.ptr(va);
    size_t n = std::min<size_t>(img_.avail(va), 16);
    uint64_t a = va;
    return code && n && cs_disasm_iter(cs_, &code, &n, &a, ci_);
}

void Lifter::analyze_flag_exports() {
    // "F exports flags => T exports flags": F ends in "call T; ...; ret" with no
    // flag writes in between, or tail-jumps to T
    std::multimap<uint32_t, uint32_t> passes;
    for (auto& [entry, fn] : disc_.funcs) {
        std::map<uint32_t, const Insn*> insns;
        for (auto& [s, b] : fn.blocks)
            for (auto* i : b.insns) insns[i->addr] = i;
        for (auto& [va, ins] : insns) {
            if (ins->kind == Kind::Jmp && disc_.funcs.count(ins->target) && !fn.blocks.count(ins->target)) {
                passes.insert({entry, ins->target});
                continue;
            }
            if (ins->kind != Kind::Call) continue;
            uint32_t t = ins->target, p = va + ins->size;
            if (t == p || !disc_.funcs.count(t)) continue;
            unsigned written = 0;
            for (int k = 0; k < 32; k++) {
                auto it = insns.find(p);
                if (it == insns.end()) break;
                const Insn* j = it->second;
                if (j->kind == Kind::Ret) {
                    passes.insert({entry, t});
                    break;
                }
                if (!decode(p)) break;
                FlagUse u = flag_use(*ci_);
                if (u.test & ~written) {
                    flag_exporters.insert(t);
                    break;
                }
                written |= u.write;
                if (written == F_ALL || (j->kind != Kind::Seq && j->kind != Kind::Jcc)) break;
                p += j->size;
            }
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& [f, t] : passes)
            if (flag_exporters.count(f) && flag_exporters.insert(t).second) changed = true;
    }
}

// ---- native replacements ------------------------------------------------------
// Hot statically linked library routines, recognised by their machine code, get
// a body that calls a native implementation (runtime/src/natives.c) instead of
// being lifted. Each signature is the routine's bytes; '..' bytes are skipped
// (relocated addresses), and their values are passed to the native as arguments.
bool lift_natives = true;
namespace {
struct NativeSig {
    const char* name;
    const char* call; // native function; arguments: c, then the u32s at `args`
    vector<std::pair<uint32_t, string>> bytes; // offset, hex pattern
    vector<uint32_t> args;                     // offsets of u32 operands passed through
};
const vector<NativeSig>& native_sigs() {
    static const vector<NativeSig> sigs = {
        // zlib 1.2.3 inflate_fast, MSVC /O2 (identical in KOF XII, XIII and XIII
        // Climax apart from the three error-message addresses)
        {"zlib inflate_fast", "x2n_inflate_fast",
         {{0x000, "83ec3c535556578b4c24508b411c8b51048b58388b2983ed018d542afb895424148b710c8b49108bd12b54245483ee01"},
          {0x41e, "c74118"}, {0x425, "c7021b000000"}, {0x431, "c74018"}, {0x44f, "c74018"}},
         {0x421, 0x434, 0x452}},
        // byte fill loop, unoptimised: for (; n; n--) *dst++ = *val (KOF XIII Climax)
        {"byte fill", "x2n_fill8",
         {{0x000, "558beceb128b450c83e80189450c8b4d0883c101894d08837d0c00760c8b55088b45108a08880aebdc5dc3"}},
         {}},
    };
    return sigs;
}
} // namespace

static bool match_hex(const Image& img, uint32_t va, const string& hex) {
    if (img.avail(va) < hex.size() / 2) return false;
    for (size_t i = 0; i < hex.size() / 2; i++)
        if (img.u8(va + (uint32_t)i) != (uint8_t)std::stoul(hex.substr(i * 2, 2), nullptr, 16)) return false;
    return true;
}

// the native body for a recognised routine at `entry`, or "" if none matches
string native_body(const Image& img, uint32_t entry) {
    if (!lift_natives) return "";
    for (auto& s : native_sigs()) {
        bool ok = true;
        for (auto& [off, hex] : s.bytes) ok = ok && match_hex(img, entry + off, hex);
        if (!ok) continue;
        string args;
        for (uint32_t off : s.args) args += format(", 0x{:x}u", img.u32(entry + off));
        return format("void f_{:08x}(X2Cpu *c) {{ /* {}: native */\n  X2_ENTER(0x{:x}u);\n  {}(c{});\n}}\n", entry, s.name,
                      entry, s.call, args);
    }
    return "";
}

string Lifter::lift_function(const Function& fn) {
    if (string n = native_body(img_, fn.entry); !n.empty()) {
        std::fprintf(stderr, "native replacement at 0x%x\n", fn.entry);
        return n;
    }
    std::map<uint32_t, const Insn*> insns;
    std::map<uint32_t, const vector<uint32_t>*> tables;
    std::set<uint32_t> labels{fn.entry};
    for (auto& [s, b] : fn.blocks) {
        labels.insert(s);
        for (auto* i : b.insns) insns[i->addr] = i;
        if (!b.table.empty()) tables[b.insns.back()->addr] = &b.table;
    }
    struct Emitted {
        uint32_t va;
        Out out;
    };
    vector<Emitted> body;
    body.reserve(insns.size());
    for (auto& [va, ins] : insns) {
        auto t = tables.find(va);
        Out o;
        try {
            o = lift(*ins, fn, t == tables.end() ? nullptr : t->second);
        } catch (const Unsupported& e) {
            // describe the instruction for the report and the runtime message
            const uint8_t* code = img_.ptr(va);
            size_t n = std::min<size_t>(img_.avail(va), 16);
            uint64_t a = va;
            string text = "?";
            if (cs_disasm_iter(cs_, &code, &n, &a, ci_)) text = string(ci_->mnemonic) + " " + ci_->op_str;
            string key = (text.find(' ') != string::npos ? text.substr(0, text.find(' ')) : text) + " (" + e.why + ")";
            unsupported[key]++;
            o.lines = {format("{} x2_unimpl(c,0x{:x}u,\"{}\"); return;", SPILL, va, text)};
            o.falls = false;
        }
        body.push_back({va, std::move(o)});
    }
    // fallthrough targets that are not the next emitted instruction need labels
    for (size_t k = 0; k < body.size(); k++) {
        if (!body[k].out.falls) continue;
        uint32_t nxt = body[k].va + insns[body[k].va]->size;
        if (k + 1 >= body.size() || body[k + 1].va != nxt) labels.insert(nxt);
    }

    string s = format("void f_{:08x}(X2Cpu *c) {{\n  X2_ENTER(0x{:x}u);\n  LOCALS();\n", fn.entry, fn.entry);
    if (body.empty() || body[0].va != fn.entry) s += format("  goto L_{:08x};\n", fn.entry);
    for (size_t k = 0; k < body.size(); k++) {
        auto& e = body[k];
        if (labels.count(e.va)) s += format("L_{:08x}:;\n", e.va);
        for (auto& l : e.out.lines) {
            string t = l;
            stack_plain(t);
            s += "  " + t + "\n";
        }
        if (e.out.falls) {
            uint32_t nxt = e.va + insns[e.va]->size;
            if (k + 1 >= body.size() || body[k + 1].va != nxt) s += format("  goto L_{:08x};\n", nxt);
        }
    }
    // labels that no emitted instruction defines (fallthrough into undecoded bytes)
    for (uint32_t l : labels)
        if (!insns.count(l)) s += format("L_{:08x}:; {} x2_dispatch_jmp(c,0x{:x}u); return;\n", l, SPILL, l);
    s += "}\n";
    return s;
}

// ------------------------------------------------------------------- lifting
Lifter::Out Lifter::lift(const Insn& ins, const Function& fn, const vector<uint32_t>* table) {
    Out o;
    cur_addr_ = ins.addr;
    uint32_t nxt = ins.addr + ins.size;
    if (ins.kind == Kind::Trap) {
        o.lines = {format("{} x2_trap(c,0x{:x}u,\"trap\"); return;", SPILL, ins.addr)};
        o.falls = false;
        return o;
    }
    const uint8_t* code = img_.ptr(ins.addr);
    size_t n = std::min<size_t>(img_.avail(ins.addr), 16);
    uint64_t a = ins.addr;
    if (!cs_disasm_iter(cs_, &code, &n, &a, ci_)) throw Unsupported{"decode"};
    const cs_insn& d = *ci_;
    const cs_x86& x = d.detail->x86;
    string m = d.mnemonic;

    switch (ins.kind) {
        case Kind::Ret: {
            int64_t pop = x.op_count ? x.operands[0].imm : 0;
            o.lines = {format("esp+={}; {}{} return;", 4 + pop, flag_exporters.count(fn.entry) ? "SPILLF(); " : "", SPILL)};
            o.falls = false;
            return o;
        }
        case Kind::Jmp:
            o.lines = {branch_to(ins.target, fn)};
            o.falls = false;
            return o;
        case Kind::Jcc: {
            string br = branch_to(ins.target, fn);
            if (m == "jecxz") o.lines = {format("if (ecx==0) {{ {} }}", br)};
            else if (m == "jcxz") o.lines = {format("if ((u16)ecx==0) {{ {} }}", br)};
            else if (m == "loop") o.lines = {format("ecx--; if (ecx!=0) {{ {} }}", br)};
            else if (m == "loope") o.lines = {format("ecx--; if (ecx!=0&&zf) {{ {} }}", br)};
            else if (m == "loopne") o.lines = {format("ecx--; if (ecx!=0&&!zf) {{ {} }}", br)};
            else {
                auto it = kCond.find(m.substr(1));
                if (it == kCond.end()) throw Unsupported{"jcc " + m};
                o.lines = {format("if ({}) {{ {} }}", it->second, br)};
            }
            return o;
        }
        case Kind::Call: {
            string push = format("esp-=4; WR32(esp,0x{:x}u);", nxt);
            if (ins.target == nxt) {  // call $+5 get-PC idiom
                o.lines = {push};
                return o;
            }
            if (disc_.funcs.count(ins.target))
                o.lines = {push, format("{} f_{:08x}(c); {}{}", SPILL, ins.target, RELOAD,
                                        flag_exporters.count(ins.target) ? " RELOADF();" : "")};
            else
                o.lines = {push, format("{} x2_dispatch_call(c,0x{:x}u); {}", SPILL, ins.target, RELOAD)};
            return o;
        }
        case Kind::ICall: {
            o.lines = {format("{{ u32 t={}; esp-=4; WR32(esp,0x{:x}u); {} x2_dispatch_call(c,t); {} }}",
                              rd(x.operands[0], 4), nxt, SPILL, RELOAD)};
            if (disc_.is_noreturn_call(ins)) {
                o.lines.push_back("return;");
                o.falls = false;
            }
            return o;
        }
        case Kind::IJmp: {
            string t = rd(x.operands[0], 4);
            if (table) {
                std::set<uint32_t> uniq(table->begin(), table->end());
                string cases;
                for (uint32_t tt : uniq) cases += format("case 0x{:x}u: goto L_{:08x}; ", tt, tt);
                o.lines = {format("{{ u32 t={}; switch (t) {{ {}default: {} x2_dispatch_jmp(c,t); return; }} }}",
                                  t, cases, SPILL)};
            } else if (ins.mem.valid && ins.mem.index && fn.blocks.size() > 1) {
                // Unresolved indexed jump (e.g. negative index into a neighbouring
                // table): any target that is a known block of this function is
                // reached directly; everything else goes to the dispatcher.
                string cases;
                for (auto& [s, b] : fn.blocks)
                    if (s != fn.entry) cases += format("case 0x{:x}u: goto L_{:08x}; ", s, s);
                o.lines = {format("{{ u32 t={}; switch (t) {{ {}default: {} x2_dispatch_jmp(c,t); return; }} }}", t, cases,
                                  SPILL)};
            } else {
                o.lines = {format("{{ u32 t={}; {} x2_dispatch_jmp(c,t); return; }}", t, SPILL)};
            }
            o.falls = false;
            return o;
        }
        default:
            break;
    }
    return lift_seq(d, ins);
}

bool Lifter::lift_single(uint32_t addr, vector<string>& lines, string& text) {
    const Insn& ins = disc_.decode(addr);
    if (ins.kind != Kind::Seq) return false;
    const uint8_t* code = img_.ptr(addr);
    size_t n = std::min<size_t>(img_.avail(addr), 16);
    uint64_t a = addr;
    if (!cs_disasm_iter(cs_, &code, &n, &a, ci_)) return false;
    text = string(ci_->mnemonic) + " " + ci_->op_str;
    try {
        Out o = lift_seq(*ci_, ins);
        for (auto& l : o.lines)
            if (l.find("x2_") != string::npos && l.find("x2_fist") == string::npos && l.find("x2_frnd") == string::npos &&
                l.find("x2_cvt") == string::npos && l.find("x2_ld80") == string::npos && l.find("x2_st80") == string::npos &&
                l.find("x2_punpck") == string::npos && l.find("x2_packssdw") == string::npos &&
                l.find("x2_pmovmskb") == string::npos && l.find("x2_fxam") == string::npos &&
                l.find("x2_memmove") == string::npos && l.find("x2_memset") == string::npos)
                return false;  // runtime services (cpuid, traps, env) are out of scope
        lines = o.lines;
        return true;
    } catch (const Unsupported&) {
        return false;
    }
}

Lifter::Out Lifter::lift_seq(const cs_insn& d, const Insn& ins) {
    Out o;
    const uint8_t* b = img_.ptr(ins.addr);
    int opi = 0;
    while (opi < 15) {
        uint8_t p = b[opi];
        if (p == 0x26 || p == 0x2E || p == 0x36 || p == 0x3E || p == 0x64 || p == 0x65 || p == 0x66 ||
            p == 0x67 || p == 0xF0 || p == 0xF2 || p == 0xF3)  // 0x9B is `wait`, its own insn
            opi++;
        else
            break;
    }
    if (b[opi] >= 0xD8 && b[opi] <= 0xDF) {
        o.lines = lift_x87(d, b, opi);
        return o;
    }
    string m = d.mnemonic;
    if (lift_int(m, d, o.lines) || lift_string(m, d, o.lines) || lift_vec(m, d, o.lines)) return o;
    throw Unsupported{"no handler"};
}

// ------------------------------------------------------------------- integer
bool Lifter::lift_int(const string& m, const cs_insn& d, vector<string>& L) {
    const cs_x86& x = d.detail->x86;
    const cs_x86_op* o = x.operands;
    int nops = x.op_count;
    uint32_t va = (uint32_t)d.address;
    auto trap = [&](const char* why) { return format("{} x2_trap(c,0x{:x}u,\"{}\"); {}", SPILL, va, why, RELOAD); };
    auto imm_or = [&](const cs_x86_op& op, int sz) {
        return op.type == X86_OP_IMM ? format("0x{:x}u", (uint64_t)op.imm & MASK(sz)) : rd(op, sz);
    };
    auto is_segreg = [&](const cs_x86_op& op) {
        if (op.type != X86_OP_REG) return false;
        string n = rn(op.reg);
        return n == "es" || n == "cs" || n == "ss" || n == "ds" || n == "fs" || n == "gs" || (n[0] == 'c' && n[1] == 'r') ||
               (n[0] == 'd' && n[1] == 'r');
    };

    if (m == "nop" || m == "pause" || m == "wait" || m == "fwait" || m == "emms" || m == "prefetchnta" ||
        m == "prefetcht0" || m == "prefetcht1" || m == "prefetcht2" || m == "lfence" || m == "sfence" ||
        m == "mfence") {
        return true;
    }
    // Flat Win32 selectors, as seen by user-mode code on 32-bit Windows.
    auto selector = [&](const cs_x86_op& op) -> string {
        string n = rn(op.reg);
        if (n == "cs") return "0x1bu";
        if (n == "fs") return "0x3bu";
        if (n == "gs") return "0u";
        if (n == "es" || n == "ss" || n == "ds") return "0x23u";
        throw Unsupported{"ctrl/debug reg"};
    };
    if (m == "mov") {
        if (is_segreg(o[0])) throw Unsupported{"segment load"};
        if (is_segreg(o[1])) {
            L.push_back(wr(o[0], selector(o[1])));
            return true;
        }
        L.push_back(wr(o[0], imm_or(o[1], o[0].size)));
        return true;
    }
    if (m == "movzx") { L.push_back(wr(o[0], rd(o[1]))); return true; }
    if (m == "movsx") {
        int s = o[1].size;
        L.push_back(wr(o[0], format("({})({})({}){}", UT(o[0].size), STy(s), UT(s), rd(o[1]))));
        return true;
    }
    if (m == "lea") { L.push_back(wr(o[0], ea(o[1], false))); return true; }
    if (m == "push") {
        if (is_segreg(o[0])) {
            L.push_back(format("esp-=4; WR32(esp,{});", selector(o[0])));
            return true;
        }
        int sz = o[0].type == X86_OP_IMM ? 4 : (o[0].size ? o[0].size : 4);
        string v = o[0].type == X86_OP_IMM ? format("0x{:x}u", (uint32_t)o[0].imm) : rd(o[0], sz);
        L.push_back(format("{{ u32 v=(u32){}; esp-={}; WR{}(esp,v); }}", v, sz, sz * 8));
        return true;
    }
    if (m == "pop") {
        if (is_segreg(o[0])) throw Unsupported{"pop segreg"};
        int sz = o[0].size;
        L.push_back(format("{{ u32 v=RD{}(esp); esp+={}; {} }}", sz * 8, sz, wr(o[0], "v")));
        return true;
    }
    if (m == "pushal") {
        L.push_back("{ u32 t=esp; esp-=32; WR32(esp+28,eax); WR32(esp+24,ecx); WR32(esp+20,edx); WR32(esp+16,ebx); "
                    "WR32(esp+12,t); WR32(esp+8,ebp); WR32(esp+4,esi); WR32(esp,edi); }");
        return true;
    }
    if (m == "popal") {
        L.push_back("edi=RD32(esp); esi=RD32(esp+4); ebp=RD32(esp+8); ebx=RD32(esp+16); edx=RD32(esp+20); "
                    "ecx=RD32(esp+24); eax=RD32(esp+28); esp+=32;");
        return true;
    }
    if (m == "pushfd") { L.push_back("esp-=4; WR32(esp,GET_EFLAGS());"); return true; }
    if (m == "popfd") { L.push_back("SET_EFLAGS(RD32(esp)); esp+=4;"); return true; }
    if (m == "lahf") { L.push_back("SET8H(eax,GET_EFLAGS());"); return true; }
    if (m == "sahf") {
        L.push_back("{ u32 f=(eax>>8)&0xFF; cf=f&1; pfb=(f&4)?0:1; af=(f>>4)&1; zf=(f>>6)&1; sf=(f>>7)&1; }");
        return true;
    }
    if (m == "clc") { L.push_back("cf=0;"); return true; }
    if (m == "stc") { L.push_back("cf=1;"); return true; }
    if (m == "cmc") { L.push_back("cf^=1;"); return true; }
    if (m == "cld") { L.push_back("df=0;"); return true; }
    if (m == "std") { L.push_back("df=1;"); return true; }

    // two-operand ALU
    static const std::set<string> alu = {"add", "sub", "adc", "sbb", "and", "or", "xor", "cmp", "test"};
    if (alu.count(m)) {
        int sz = o[0].size, bits = sz * 8;
        const char* t = UT(sz);
        bool write = m != "cmp" && m != "test";
        if ((m == "xor" || m == "sub") && o[0].type == X86_OP_REG && o[1].type == X86_OP_REG && o[0].reg == o[1].reg) {
            L.push_back("cf=0; of=0; zf=1; sf=0; pfb=0; af=0; " + wr(o[0], "0"));
            return true;
        }
        string s = format("{{ {} a={}, b={}, r; ", t, rd(o[0]), imm_or(o[1], sz));
        if (m == "add") s += format("r=({})(a+b); FL_ADD({},a,b,r); ", t, bits);
        else if (m == "sub" || m == "cmp") s += format("r=({})(a-b); FL_SUB({},a,b,r); ", t, bits);
        else if (m == "adc") s += format("u64 w=(u64)a+b+cf; r=({})w; FL_ADD({},a,b,r); cf=(u32)(w>>{})&1; ", t, bits, bits);
        else if (m == "sbb") s += format("u64 w=(u64)a-b-cf; r=({})w; FL_SUB({},a,b,r); cf=(u32)(w>>{})&1; ", t, bits, bits);
        else {
            const char* sym = m == "and" || m == "test" ? "&" : m == "or" ? "|" : "^";
            s += format("r=({})(a{}b); FL_LOGIC({},r); ", t, sym, bits);
        }
        if (write) s += wr(o[0], "r") + " ";
        L.push_back(s + "}");
        return true;
    }
    if (m == "inc" || m == "dec") {
        int sz = o[0].size;
        const char* t = UT(sz);
        L.push_back(format("{{ {} a={}, b=1, r=({})(a{}1); u32 c0=cf; FL_{}({},a,b,r); cf=c0; {} }}", t, rd(o[0]), t,
                           m == "inc" ? "+" : "-", m == "inc" ? "ADD" : "SUB", sz * 8, wr(o[0], "r")));
        return true;
    }
    if (m == "neg") {
        int sz = o[0].size;
        const char* t = UT(sz);
        L.push_back(format("{{ {} a=0, b={}, r=({})(0-b); FL_SUB({},a,b,r); {} }}", t, rd(o[0]), t, sz * 8, wr(o[0], "r")));
        return true;
    }
    if (m == "not") { L.push_back(wr(o[0], format("({})~{}", UT(o[0].size), rd(o[0])))); return true; }

    auto count = [&]() -> string {
        if (nops < 2) return "1";
        if (o[1].type == X86_OP_IMM) return std::to_string(o[1].imm & 31);
        return "(ecx&31)";
    };
    if (m == "shl" || m == "sal" || m == "shr" || m == "sar") {
        int sz = o[0].size, bits = sz * 8;
        const char* t = UT(sz);
        string n = count();
        if (n == "0") return true;
        string body;
        if (m == "shl" || m == "sal")
            body = format("r=({})((u64)a<<n); cf=(u32)(((u64)a<<n)>>{})&1; of=((r>>{})&1)^cf;", t, bits, bits - 1);
        else if (m == "shr")
            body = format("r=({})((u64)a>>n); cf=(u32)((u64)a>>(n-1))&1; of=(a>>{})&1;", t, bits - 1);
        else
            body = format("r=({})((s64)({})a>>(n>{}?{}:n)); cf=(u32)((s64)({})a>>(n-1>{}?{}:n-1))&1; of=0;", t, STy(sz),
                          bits - 1, bits - 1, STy(sz), bits - 1, bits - 1);
        L.push_back(format("{{ {} a={}, r; u32 n={}; if (n) {{ {} zf=r==0; sf=(r>>{})&1; pfb=(u8)r; {} }} }}", t, rd(o[0]),
                           n, body, bits - 1, wr(o[0], "r")));
        return true;
    }
    if (m == "rol" || m == "ror" || m == "rcr" || m == "rcl") {
        int sz = o[0].size, bits = sz * 8;
        const char* t = UT(sz);
        string body;
        if (m == "rol")
            body = format("n%={0}; r=n?({1})((a<<n)|(a>>({0}-n))):a; cf=r&1; of=((r>>{2})&1)^cf;", bits, t, bits - 1);
        else if (m == "ror")
            body = format("n%={0}; r=n?({1})((a>>n)|(a<<({0}-n))):a; cf=(r>>{2})&1; of=((r>>{2})^(r>>{3}))&1;", bits, t,
                          bits - 1, bits - 2);
        else if (m == "rcr")
            body = format("r=a; while (n--) {{ u32 lo=r&1; r=({0})((r>>1)|((u64)cf<<{1})); cf=lo; }} of=((r>>{1})^(r>>{2}))&1;",
                          t, bits - 1, bits - 2);
        else
            body = format("r=a; while (n--) {{ u32 hi=(r>>{1})&1; r=({0})((r<<1)|cf); cf=hi; }} of=((r>>{1})&1)^cf;", t,
                          bits - 1);
        L.push_back(format("{{ {} a={}, r; u32 n={}; if (n) {{ {} {} }} }}", t, rd(o[0]), count(), body, wr(o[0], "r")));
        return true;
    }
    if (m == "shld" || m == "shrd") {
        if (o[0].size != 4) throw Unsupported{"16-bit shld/shrd"};
        string n = o[2].type == X86_OP_IMM ? std::to_string(o[2].imm & 31) : "(ecx&31)";
        string body = m == "shld" ? "r=(a<<n)|(b>>(32-n)); u32 c1=(a>>(32-n))&1;" : "r=(a>>n)|(b<<(32-n)); u32 c1=(a>>(n-1))&1;";
        L.push_back(format("{{ u32 a={}, b={}, r; u32 n={}; if (n) {{ {} FL_LOGIC(32,r); cf=c1; {} }} }}", rd(o[0]),
                           rd(o[1]), n, body, wr(o[0], "r")));
        return true;
    }
    if (m == "imul") {
        if (nops == 1) {
            int sz = o[0].size;
            if (sz == 4)
                L.push_back(format("{{ s64 r=(s64)(s32)eax*(s32){}; eax=(u32)r; edx=(u32)((u64)r>>32); cf=of=(r!=(s32)r); }}", rd(o[0])));
            else if (sz == 2)
                L.push_back(format("{{ s32 r=(s32)(s16)eax*(s16){}; SET16(eax,r); SET16(edx,(u32)r>>16); cf=of=(r!=(s16)r); }}", rd(o[0])));
            else
                L.push_back(format("{{ s16 r=(s16)(s8)eax*(s8){}; SET16(eax,r); cf=of=(r!=(s8)r); }}", rd(o[0])));
            return true;
        }
        int sz = o[0].size;
        const cs_x86_op& sa = nops == 3 ? o[1] : o[0];
        const cs_x86_op& sb = nops == 3 ? o[2] : o[1];
        const char* s = STy(sz);
        L.push_back(format("{{ s64 r=(s64)({0}){1}*({0}){2}; cf=of=(r!=({0})r); zf=({0})r==0; sf=({0})r<0; pfb=(u8)r; {3} }}", s,
                           rd(sa), imm_or(sb, sz), wr(o[0], format("({})r", UT(sz)))));
        return true;
    }
    if (m == "mul") {
        int sz = o[0].size;
        if (sz == 4) L.push_back(format("{{ u64 r=(u64)eax*{}; eax=(u32)r; edx=(u32)(r>>32); cf=of=(edx!=0); }}", rd(o[0])));
        else if (sz == 2) L.push_back(format("{{ u32 r=(u32)(u16)eax*{}; SET16(eax,r); SET16(edx,r>>16); cf=of=((r>>16)!=0); }}", rd(o[0])));
        else L.push_back(format("{{ u32 r=(u32)(u8)eax*{}; SET16(eax,r); cf=of=((r>>8)!=0); }}", rd(o[0])));
        return true;
    }
    if (m == "div" || m == "idiv") {
        int sz = o[0].size;
        string z = format("if (!b) {{ {} }}", trap("divide by zero"));
        if (m == "div") {
            if (sz == 4) L.push_back(format("{{ u32 b={}; {} u64 a=((u64)edx<<32)|eax; eax=(u32)(a/b); edx=(u32)(a%b); }}", rd(o[0]), z));
            else if (sz == 2) L.push_back(format("{{ u32 b={}; {} u32 a=((edx&0xffff)<<16)|(eax&0xffff); SET16(eax,a/b); SET16(edx,a%b); }}", rd(o[0]), z));
            else L.push_back(format("{{ u32 b={}; {} u32 a=eax&0xffff; SET8L(eax,a/b); SET8H(eax,a%b); }}", rd(o[0]), z));
        } else {
            if (sz == 4) L.push_back(format("{{ s32 b=(s32){}; {} s64 a=(s64)(((u64)edx<<32)|eax); eax=(u32)(s32)(a/b); edx=(u32)(s32)(a%b); }}", rd(o[0]), z));
            else if (sz == 2) L.push_back(format("{{ s32 b=(s16){}; {} s32 a=(s32)(((edx&0xffff)<<16)|(eax&0xffff)); SET16(eax,a/b); SET16(edx,a%b); }}", rd(o[0]), z));
            else L.push_back(format("{{ s32 b=(s8){}; {} s32 a=(s16)eax; SET8L(eax,a/b); SET8H(eax,a%b); }}", rd(o[0]), z));
        }
        return true;
    }
    if (m == "cdq") { L.push_back("edx=(u32)((s32)eax>>31);"); return true; }
    if (m == "cwde") { L.push_back("eax=(u32)(s32)(s16)eax;"); return true; }
    if (m == "cbw") { L.push_back("SET16(eax,(s16)(s8)eax);"); return true; }
    if (m == "cwd") { L.push_back("SET16(edx,((s16)eax<0)?0xffff:0);"); return true; }
    if (m == "xchg") {
        L.push_back(format("{{ u32 t={}; {} {} }}", rd(o[0]), wr(o[0], rd(o[1])), wr(o[1], "t")));
        return true;
    }
    if (m == "cmpxchg") {
        int sz = o[0].size;
        const char* t = UT(sz);
        const char* acc = sz == 4 ? "eax" : sz == 2 ? "(u16)eax" : "(u8)eax";
        const char* wacc = sz == 4 ? "eax=v;" : sz == 2 ? "SET16(eax,v);" : "SET8L(eax,v);";
        L.push_back(format("{{ {0} a={1}, b={2}, r=({0})(a-b); FL_SUB({3},a,b,r); if (zf) {{ {4} }} else {{ {0} v=b; {5} }} }}", t,
                           acc, rd(o[0]), sz * 8, wr(o[0], rd(o[1])), wacc));
        return true;
    }
    if (m == "xadd") {
        int sz = o[0].size;
        const char* t = UT(sz);
        L.push_back(format("{{ {0} a={1}, b={2}, r=({0})(a+b); FL_ADD({3},a,b,r); {4} {5} }}", t, rd(o[0]), rd(o[1]), sz * 8,
                           wr(o[1], "a"), wr(o[0], "r")));
        return true;
    }
    if (m == "leave") { L.push_back("esp=ebp; ebp=RD32(esp); esp+=4;"); return true; }
    if (m == "enter") {
        if (o[1].imm != 0) throw Unsupported{"enter nesting"};
        L.push_back(format("esp-=4; WR32(esp,ebp); ebp=esp; esp-={};", o[0].imm));
        return true;
    }
    if (m == "bt" || m == "bts" || m == "btr" || m == "btc") {
        int sz = o[0].size;
        if (o[0].type == X86_OP_MEM && o[1].type == X86_OP_REG) {
            // bit-string addressing: the signed bit offset selects the dword
            string base = format("{{ s32 bo=(s32){}; u32 p={}+(u32)((bo>>5)*4); u32 a=RD32(p); u32 n=(u32)bo&31; cf=(a>>n)&1; ",
                                 rd(o[1], 4), ea(o[0]));
            if (m == "bts") base += "WR32(p,a|(1u<<n)); ";
            if (m == "btr") base += "WR32(p,a&~(1u<<n)); ";
            if (m == "btc") base += "WR32(p,a^(1u<<n)); ";
            L.push_back(base + "}");
            return true;
        }
        const char* t = UT(sz);
        string base = format("{{ {} a={}; u32 n={}&{}; cf=(a>>n)&1; ", t, rd(o[0]), imm_or(o[1], sz), sz * 8 - 1);
        if (m == "bts") base += wr(o[0], format("a|(({})1<<n)", t));
        if (m == "btr") base += wr(o[0], format("a&~(({})1<<n)", t));
        if (m == "btc") base += wr(o[0], format("a^(({})1<<n)", t));
        L.push_back(base + " }");
        return true;
    }
    if (m == "bsf" || m == "bsr") {
        L.push_back(format("{{ u32 s={}; zf=s==0; if (s) {{ {} }} }}", rd(o[1]),
                           wr(o[0], m == "bsf" ? "__builtin_ctz(s)" : "31-__builtin_clz(s)")));
        return true;
    }
    if (m == "bswap") { L.push_back(wr(o[0], format("__builtin_bswap32({})", rd(o[0])))); return true; }
    if (m == "xlatb") { L.push_back("SET8L(eax,RD8(ebx+(u8)eax));"); return true; }
    if (m == "cpuid") { L.push_back(format("{} x2_cpuid(c); {}", SPILL, RELOAD)); return true; }
    if (m == "rdtsc") { L.push_back(format("{} x2_rdtsc(c); {}", SPILL, RELOAD)); return true; }
    if (m == "stmxcsr") { L.push_back(wr(o[0], "c->mxcsr")); return true; }
    if (m == "ldmxcsr") { L.push_back(format("c->mxcsr={}; x2_mxcsr_changed(c);", rd(o[0]))); return true; }
    if (m == "int3") { L.push_back(trap("int3")); return true; }
    if (m.rfind("set", 0) == 0 && kCond.count(m.substr(3))) {
        L.push_back(wr(o[0], format("({})?1:0", kCond.at(m.substr(3)))));
        return true;
    }
    if (m.rfind("cmov", 0) == 0 && kCond.count(m.substr(4))) {
        L.push_back(format("if ({}) {{ {} }}", kCond.at(m.substr(4)), wr(o[0], rd(o[1]))));
        return true;
    }
    return false;
}

// --------------------------------------------------------------- string ops
bool Lifter::lift_string(const string& m, const cs_insn& d, vector<string>& L) {
    const cs_x86& x = d.detail->x86;
    string rep, base = m;
    if (auto sp = m.find(' '); sp != string::npos) {
        rep = m.substr(0, sp);
        base = m.substr(sp + 1);
    }
    static const char* kinds[] = {"movs", "stos", "lods", "cmps", "scas"};
    string kind;
    for (auto* k : kinds)
        if (base.size() == 5 && base.rfind(k, 0) == 0 && std::string("bwd").find(base[4]) != string::npos) kind = k;
    if (kind.empty()) return false;
    for (int k = 0; k < x.op_count; k++)  // movsd xmm form is SSE2, not a string op
        if (x.operands[k].type == X86_OP_REG && rn(x.operands[k].reg).rfind("xmm", 0) == 0) return false;
    int sz = base[4] == 'b' ? 1 : base[4] == 'w' ? 2 : 4;
    int bits = sz * 8;
    const char* U = UT(sz);
    string step = format("(df?-{}:{})", sz, sz);
    const char* acc = sz == 1 ? "(u8)eax" : sz == 2 ? "(u16)eax" : "eax";
    const char* wacc = sz == 1 ? "SET8L(eax,v);" : sz == 2 ? "SET16(eax,v);" : "eax=v;";
    string seg;
    for (int k = 0; k < x.op_count; k++)
        if (x.operands[k].type == X86_OP_MEM && x.operands[k].mem.segment == X86_REG_FS) seg = "c->fs_base+";
    string body;
    if (kind == "movs") body = format("WR{0}(edi,RD{0}({1}esi)); esi+={2}; edi+={2};", bits, seg, step);
    else if (kind == "stos") body = format("WR{}(edi,{}); edi+={};", bits, acc, step);
    else if (kind == "lods") body = format("{{ {} v=RD{}({}esi); {} }} esi+={};", U, bits, seg, wacc, step);
    else if (kind == "cmps")
        body = format("{{ {0} a=RD{1}({2}esi), b=RD{1}(edi), r=({0})(a-b); FL_SUB({1},a,b,r); }} esi+={3}; edi+={3};", U, bits, seg, step);
    else body = format("{{ {0} a={1}, b=RD{2}(edi), r=({0})(a-b); FL_SUB({2},a,b,r); }} edi+={3};", U, acc, bits, step);

    if (rep.empty()) {
        L.push_back(body);
    } else if (rep == "rep") {
        if (kind == "movs" && seg.empty())
            L.push_back(format("if (!df) {{ x2_memmove(edi,esi,ecx*{0}); esi+=ecx*{0}; edi+=ecx*{0}; ecx=0; }} else while (ecx) {{ {1} ecx--; }}", sz, body));
        else if (kind == "stos")
            L.push_back(format("if (!df) {{ x2_memset{0}(edi,{1},ecx); edi+=ecx*{2}; ecx=0; }} else while (ecx) {{ {3} ecx--; }}", bits, acc, sz, body));
        else
            L.push_back(format("while (ecx) {{ {} ecx--; }}", body));
    } else {
        const char* stop = (rep == "repe" || rep == "repz") ? "!zf" : "zf";
        L.push_back(format("while (ecx) {{ {} ecx--; if ({}) break; }}", body, stop));
    }
    return true;
}

// ---------------------------------------------------------------------- x87
namespace {
const char* kArith[] = {"add", "mul", "com", "comp", "sub", "subr", "div", "divr"};

string fop(const string& k, const string& x, const string& y) {
    if (k == "add") return x + "+" + y;
    if (k == "mul") return x + "*" + y;
    if (k == "sub") return x + "-" + y;
    if (k == "subr") return y + "-" + x;
    if (k == "div") return x + "/" + y;
    return y + "/" + x;  // divr
}
}  // namespace

vector<string> Lifter::lift_x87(const cs_insn& d, const uint8_t* b, int i) {
    uint8_t op = b[i], modrm = b[i + 1];
    int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    const cs_x86& x = d.detail->x86;
    if (mod != 3) {
        const cs_x86_op* memop = nullptr;
        for (int k = 0; k < x.op_count; k++)
            if (x.operands[k].type == X86_OP_MEM) memop = &x.operands[k];
        if (!memop) throw Unsupported{"x87 mem operand"};
        string a = ea(*memop);
        if (op == 0xD8 || op == 0xDC || op == 0xDA || op == 0xDE) {
            string src = op == 0xD8 ? format("(f64)RDF32({})", a)
                       : op == 0xDC ? format("RDF64({})", a)
                       : op == 0xDA ? format("(f64)(s32)RD32({})", a)
                                    : format("(f64)(s16)RD16({})", a);
            string k = kArith[reg];
            if (k == "com") return {format("FCOM(ST(0),{});", src)};
            if (k == "comp") return {format("FCOM(ST(0),{}); FPOP();", src)};
            return {format("ST(0)={};", fop(k, "ST(0)", src))};
        }
        switch (op << 4 | reg) {
            case 0xD90: return {format("FPUSH((f64)RDF32({}));", a)};
            case 0xD92: return {format("WRF32({},(f32)ST(0));", a)};
            case 0xD93: return {format("WRF32({},(f32)ST(0)); FPOP();", a)};
            case 0xD94: return {format("x2_fldenv(c,{});", a)};
            case 0xD95: return {format("c->fcw=RD16({});", a)};
            case 0xD96: return {format("x2_fnstenv(c,{});", a)};
            case 0xD97: return {format("WR16({},c->fcw);", a)};
            case 0xDB0: return {format("FPUSH((f64)(s32)RD32({}));", a)};
            case 0xDB1: return {format("WR32({},x2_fist32(c,ST(0),1)); FPOP();", a)};
            case 0xDB2: return {format("WR32({},x2_fist32(c,ST(0),0));", a)};
            case 0xDB3: return {format("WR32({},x2_fist32(c,ST(0),0)); FPOP();", a)};
            case 0xDB5: return {format("FPUSH(x2_ld80({}));", a)};
            case 0xDB7: return {format("x2_st80({},ST(0)); FPOP();", a)};
            case 0xDD0: return {format("FPUSH(RDF64({}));", a)};
            case 0xDD1: return {format("WR64({},x2_fist64(c,ST(0),1)); FPOP();", a)};
            case 0xDD2: return {format("WRF64({},ST(0));", a)};
            case 0xDD3: return {format("WRF64({},ST(0)); FPOP();", a)};
            case 0xDD4: return {format("x2_frstor(c,{});", a)};
            case 0xDD6: return {format("x2_fnsave(c,{});", a)};
            case 0xDD7: return {format("WR16({},FSW());", a)};
            case 0xDF0: return {format("FPUSH((f64)(s16)RD16({}));", a)};
            case 0xDF1: return {format("WR16({},x2_fist16(c,ST(0),1)); FPOP();", a)};
            case 0xDF2: return {format("WR16({},x2_fist16(c,ST(0),0));", a)};
            case 0xDF3: return {format("WR16({},x2_fist16(c,ST(0),0)); FPOP();", a)};
            case 0xDF5: return {format("FPUSH((f64)(s64)RD64({}));", a)};
            case 0xDF7: return {format("WR64({},x2_fist64(c,ST(0),0)); FPOP();", a)};
        }
        throw Unsupported{format("x87 mem {:02x}/{}", op, reg)};
    }

    string si = format("ST({})", rm);
    auto dc_kind = [](int r) -> const char* {
        switch (r) {
            case 0: return "add"; case 1: return "mul"; case 4: return "subr";
            case 5: return "sub"; case 6: return "divr"; case 7: return "div";
        }
        return nullptr;
    };
    switch (op) {
        case 0xD8: {
            string k = kArith[reg];
            if (k == "com") return {format("FCOM(ST(0),{});", si)};
            if (k == "comp") return {format("FCOM(ST(0),{}); FPOP();", si)};
            return {format("ST(0)={};", fop(k, "ST(0)", si))};
        }
        case 0xDC:  // st(i) = st(i) op st(0); sub/subr and div/divr swap vs D8
            if (auto* k = dc_kind(reg)) return {format("{}={};", si, fop(k, si, "ST(0)"))};
            break;
        case 0xDE:
            if (modrm == 0xD9) return {"FCOM(ST(0),ST(1)); FPOP(); FPOP();"};
            if (auto* k = dc_kind(reg)) return {format("{}={}; FPOP();", si, fop(k, si, "ST(0)"))};
            break;
        case 0xD9:
            if (reg == 0) return {format("{{ f64 v={}; FPUSH(v); }}", si)};
            if (reg == 1) return {format("{{ f64 t=ST(0); ST(0)={}; {}=t; }}", si, si)};
            switch (modrm) {
                case 0xD0: return {};
                case 0xE0: return {"ST(0)=-ST(0);"};
                case 0xE1: return {"ST(0)=fabs(ST(0));"};
                case 0xE4: return {"FCOM(ST(0),0.0);"};
                case 0xE5: return {"x2_fxam(c);"};
                case 0xE8: return {"FPUSH(1.0);"};
                case 0xE9: return {"FPUSH(3.321928094887362347870);"};
                case 0xEA: return {"FPUSH(1.442695040888963407360);"};
                case 0xEB: return {"FPUSH(3.141592653589793238463);"};
                case 0xEC: return {"FPUSH(0.301029995663981195214);"};
                case 0xED: return {"FPUSH(0.693147180559945309417);"};
                case 0xEE: return {"FPUSH(0.0);"};
                case 0xF0: return {"ST(0)=exp2(ST(0))-1.0;"};
                case 0xF1: return {"ST(1)=ST(1)*log2(ST(0)); FPOP();"};
                case 0xF2: return {"ST(0)=tan(ST(0)); FPUSH(1.0); c->fsw_c&=~0x400;"};
                case 0xF3: return {"ST(1)=atan2(ST(1),ST(0)); FPOP();"};
                case 0xF5: return {"ST(0)=remainder(ST(0),ST(1)); c->fsw_c&=~0x400;"};
                case 0xF6: return {"c->ftop=(c->ftop-1)&7;"};
                case 0xF7: return {"c->ftop=(c->ftop+1)&7;"};
                case 0xF8: return {"ST(0)=fmod(ST(0),ST(1)); c->fsw_c&=~0x400;"};
                case 0xF9: return {"ST(1)=ST(1)*log2(ST(0)+1.0); FPOP();"};
                case 0xFA: return {"ST(0)=sqrt(ST(0));"};
                case 0xFB: return {"{ f64 v=ST(0); ST(0)=sin(v); FPUSH(cos(v)); c->fsw_c&=~0x400; }"};
                case 0xFC: return {"ST(0)=x2_frnd(c,ST(0));"};
                case 0xFD: return {"ST(0)=ldexp(ST(0),(int)trunc(ST(1)));"};
                case 0xFE: return {"ST(0)=sin(ST(0)); c->fsw_c&=~0x400;"};
                case 0xFF: return {"ST(0)=cos(ST(0)); c->fsw_c&=~0x400;"};
            }
            break;
        case 0xDA: {
            if (modrm == 0xE9) return {"FCOM(ST(0),ST(1)); FPOP(); FPOP();"};
            const char* cnd[] = {"cf", "zf", "(cf|zf)", "PF()"};
            if (reg < 4) return {format("if ({}) ST(0)={};", cnd[reg], si)};
            break;
        }
        case 0xDB: {
            if (modrm == 0xE2) return {"c->fsw_c&=0x4700;"};
            if (modrm == 0xE3) return {"x2_fninit(c);"};
            if (reg == 5 || reg == 6) return {format("FCOMI(ST(0),{});", si)};
            const char* cnd[] = {"!cf", "!zf", "!(cf|zf)", "!PF()"};
            if (reg < 4) return {format("if ({}) ST(0)={};", cnd[reg], si)};
            break;
        }
        case 0xDD:
            switch (reg) {
                case 0: return {};  // ffree
                case 2: return {format("{}=ST(0);", si)};
                case 3: return {format("{}=ST(0); FPOP();", si)};
                case 4: return {format("FCOM(ST(0),{});", si)};
                case 5: return {format("FCOM(ST(0),{}); FPOP();", si)};
            }
            break;
        case 0xDF:
            if (modrm == 0xE0) return {"SET16(eax,FSW());"};
            if (reg == 5 || reg == 6) return {format("FCOMI(ST(0),{}); FPOP();", si)};
            break;
    }
    throw Unsupported{format("x87 {:02x} {:02x}", op, modrm)};
}

}  // namespace x2
