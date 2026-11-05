// lifttest: differential test of x2recomp's instruction lifting against Unicorn.
//
//   x2recomp <game.exe> build/lifttest/tests.c    (emits single-instruction tests)
//   lifttest <game.exe> [trials] [filter]
//
// Each test runs one real instruction from the game twice - in Unicorn and as
// lifted C - from identical random register/flag/memory state, and compares
// GPRs, EFLAGS (minus architecturally undefined bits), XMM registers and a
// scratch memory window. Memory operands are steered into the scratch window by
// pointing every register at it on half of the trials.
#include <unicorn/unicorn.h>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <sys/mman.h>
#include <vector>

extern "C" {
#include "x2/recomp.h"
typedef struct {
    u32 addr;
    u32 len;
    const char *text;
    void (*fn)(X2Cpu *);
} X2Test;
extern const X2Test x2_tests[];
extern const unsigned x2_test_count;

u8 *x2_mem;
// runtime services the generated code may reference
void x2_dispatch_call(X2Cpu *, u32) {}
void x2_dispatch_jmp(X2Cpu *, u32) {}
void x2_unimpl(X2Cpu *, u32 va, const char *t) {
    fprintf(stderr, "unimpl %08x %s\n", va, t);
    abort();
}
void x2_trap(X2Cpu *, u32, const char *) {}
void x2_cpuid(X2Cpu *) {}
void x2_rdtsc(X2Cpu *) {}
void x2_mxcsr_changed(X2Cpu *) {}
void x2_fldenv(X2Cpu *, u32) {}
void x2_fnstenv(X2Cpu *, u32) {}
void x2_frstor(X2Cpu *, u32) {}
void x2_fnsave(X2Cpu *, u32) {}
void x2_fninit(X2Cpu *) {}
}

/* x87 registers are transferred as 10 raw bytes: 64-bit mantissa + 16-bit sign/exponent */
struct uc_x86_float80 {
    uint64_t mantissa;
    uint16_t exponent;
    uint8_t pad[6];
};

static const u32 SCRATCH = 0x20000000u, SCRATCH_SIZE = 0x40000u;  // 256 KiB window
static u32 img_base, img_size;
static std::vector<u8> image;

static bool load_image(const char *path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<u8> f((std::istreambuf_iterator<char>(in)), {});
    if (f.size() < 0x400) return false;
    auto u16at = [&](size_t o) { return (u32)f[o] | (u32)f[o + 1] << 8; };
    auto u32at = [&](size_t o) { return u16at(o) | u16at(o + 2) << 16; };
    u32 nt = u32at(0x3C), fh = nt + 4, oh = fh + 20;
    u32 nsec = u16at(fh + 2), opt = u16at(fh + 16);
    img_base = u32at(oh + 28);
    img_size = u32at(oh + 56);
    image.assign(img_size, 0);
    memcpy(image.data(), f.data(), std::min<size_t>(u32at(oh + 60), f.size()));
    for (u32 i = 0, sh = oh + opt; i < nsec; i++, sh += 40) {
        u32 vs = u32at(sh + 8), rva = u32at(sh + 12), rs = u32at(sh + 16), ro = u32at(sh + 20);
        u32 n = std::min(vs ? vs : rs, rs);
        if (rva + n <= img_size && ro + n <= f.size()) memcpy(image.data() + rva, f.data() + ro, n);
    }
    return true;
}

// flags each mnemonic leaves undefined (bits to ignore when comparing)
static u32 undefined_flags(const std::string &m, const std::string &text) {
    const u32 CF = 1, PF = 4, AF = 0x10, ZF = 0x40, SF = 0x80, OF = 0x800;
    if (m == "and" || m == "or" || m == "xor" || m == "test") return AF;
    if (m == "shl" || m == "sal" || m == "shr" || m == "sar" || m == "shld" || m == "shrd") return AF | OF;
    if (m == "rol" || m == "ror" || m == "rcl" || m == "rcr") return OF;
    if (m == "mul" || m == "imul") return PF | AF | ZF | SF;
    if (m == "div" || m == "idiv") return CF | PF | AF | ZF | SF | OF;
    if (m == "bsf" || m == "bsr") return CF | PF | AF | SF | OF;
    if (m == "bt" || m == "bts" || m == "btr" || m == "btc") return PF | AF | ZF | SF | OF;
    (void)text;
    return 0;
}

static const int uc_gpr[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                              UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};
static const char *gpr_name[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: lifttest <game.exe> [trials] [mnemonic-filter]\n");
        return 2;
    }
    if (!load_image(argv[1])) return 1;
    int trials = argc > 2 ? atoi(argv[2]) : 16;
    const char *filter = argc > 3 ? argv[3] : nullptr;

    void *p = mmap(nullptr, 0x100000000ull + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    x2_mem = (u8 *)p;

    uc_engine *uc;
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc) != UC_ERR_OK) return 1;
    uc_mem_map(uc, img_base, (img_size + 0xFFF) & ~0xFFFu, UC_PROT_ALL);
    uc_mem_map(uc, SCRATCH, SCRATCH_SIZE, UC_PROT_ALL);

    std::mt19937 rng(12345);
    std::vector<u8> scratch(SCRATCH_SIZE);
    int failed_tests = 0, run = 0;
    for (unsigned t = 0; t < x2_test_count; t++) {
        const X2Test &T = x2_tests[t];
        std::string text = T.text, mn = text.substr(0, text.find(' '));
        if (filter && text.find(filter) == std::string::npos) continue;
        u32 undef = undefined_flags(mn, text);
        bool x87 = text.size() > 0 && text[0] == 'f';
        int fails = 0;
        for (int k = 0; k < trials && fails == 0; k++) {
            // random state; on even trials every register points into scratch
            u32 regs[8];
            for (int r = 0; r < 8; r++) {
                regs[r] = (k & 1) ? rng() : SCRATCH + 0x10000 + (rng() % 0x20000 & ~3u);
                if (k & 2) regs[r] &= 0xFF;  // small values exercise shifts/counts
            }
            regs[4] = SCRATCH + 0x30000 + (rng() % 0x1000 & ~3u);  // esp always valid
            regs[1] = (k & 1) ? regs[1] : (regs[1] & 0x3F);          // keep rep counts sane
            u32 flags = (rng() & 0x8D5) | 2;  // CF PF AF ZF SF OF, DF clear
            for (auto &b : scratch) b = (u8)rng();
            u8 xmm[8][16];
            for (auto &x : xmm)
                for (auto &b : x) b = (u8)rng();
            // floats with sane exponents for SSE arithmetic
            for (auto &x : xmm)
                for (int l = 0; l < 4; l++) {
                    float f = (float)((int)(rng() % 20001) - 10000) / 64.0f;
                    memcpy(x + 4 * l, &f, 4);
                }

            // x87 stack: 8 valid registers holding modest doubles, TOP = 0
            double st[8];
            for (int i = 0; i < 8; i++) st[i] = (double)((int)(rng() % 200001) - 100000) / 128.0;

            // --- reference: unicorn
            uc_mem_write(uc, img_base, image.data(), img_size);
            uc_mem_write(uc, SCRATCH, scratch.data(), SCRATCH_SIZE);
            for (int r = 0; r < 8; r++) uc_reg_write(uc, uc_gpr[r], &regs[r]);
            u32 uflags = flags | 0x200;  // IF set, as in user mode on Windows
            uc_reg_write(uc, UC_X86_REG_EFLAGS, &uflags);
            for (int i = 0; i < 8; i++) uc_reg_write(uc, UC_X86_REG_XMM0 + i, xmm[i]);
            u32 mxcsr = 0x1F80, fpcw = 0x27F, fpsw = 0, fptag = 0;
            uc_reg_write(uc, UC_X86_REG_MXCSR, &mxcsr);
            uc_reg_write(uc, UC_X86_REG_FPCW, &fpcw);
            uc_reg_write(uc, UC_X86_REG_FPSW, &fpsw);
            uc_reg_write(uc, UC_X86_REG_FPTAG, &fptag);
            for (int i = 0; i < 8; i++) {
                uc_x86_float80 f80;
                x2_st80(0, st[i]); /* scratch-free conversion via guest page 0..9 */
                memcpy(&f80.mantissa, x2_mem, 8);
                memcpy(&f80.exponent, x2_mem + 8, 2);
                uc_reg_write(uc, UC_X86_REG_FP0 + i, &f80);
            }
            uc_err err = uc_emu_start(uc, T.addr, T.addr + T.len, 0, 0);

            // --- lifted
            memcpy(x2_mem + img_base, image.data(), img_size);
            memcpy(x2_mem + SCRATCH, scratch.data(), SCRATCH_SIZE);
            X2Cpu c;
            memset(&c, 0, sizeof c);
            c.eax = regs[0]; c.ecx = regs[1]; c.edx = regs[2]; c.ebx = regs[3];
            c.esp = regs[4]; c.ebp = regs[5]; c.esi = regs[6]; c.edi = regs[7];
            c.eflags = flags;
            c.fcw = 0x27F;
            c.mxcsr = 0x1F80;
            memcpy(c.xmm, xmm, sizeof xmm);
            for (int i = 0; i < 8; i++) {
                c.st[i] = st[i];
                /* MMX registers alias the x87 mantissas */
                uc_x86_float80 f80;
                uc_reg_read(uc, UC_X86_REG_FP0 + i, &f80);
                memcpy(c.mm[i].b, &f80.mantissa, 8);
            }
            c.ftop = 0;

            if (err != UC_ERR_OK) continue;  // faulting inputs (e.g. bad address, #DE): skip
            T.fn(&c);
            run++;

            u32 lifted[8] = {c.eax, c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi};
            std::string why;
            for (int r = 0; r < 8; r++) {
                u32 v;
                uc_reg_read(uc, uc_gpr[r], &v);
                if (v != lifted[r]) {
                    char b[96];
                    snprintf(b, sizeof b, " %s=%08x want %08x", gpr_name[r], lifted[r], v);
                    why += b;
                }
            }
            u32 uf;
            uc_reg_read(uc, UC_X86_REG_EFLAGS, &uf);
            u32 mask = 0x8D5 & ~undef;
            if ((uf & mask) != (c.eflags & mask)) {
                char b[96];
                snprintf(b, sizeof b, " eflags=%03x want %03x (in %03x)", c.eflags & mask, uf & mask, flags & 0x8D5);
                why += b;
            }
            if (x87) {
                u32 sw;
                uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
                u32 top = (sw >> 11) & 7;
                if (top != (c.ftop & 7) || (sw & 0x4500) != (c.fsw_c & 0x4500)) {
                    char b[96];
                    snprintf(b, sizeof b, " top/cc=%u/%04x want %u/%04x", c.ftop & 7, c.fsw_c & 0x4500, top, sw & 0x4500);
                    why += b;
                }
                for (int i = 0; i < 8; i++) {
                    uc_x86_float80 f80;
                    uc_reg_read(uc, UC_X86_REG_FP0 + i, &f80);  /* physical register i */
                    memcpy(x2_mem, &f80.mantissa, 8);
                    memcpy(x2_mem + 8, &f80.exponent, 2);
                    double want = x2_ld80(0), got = c.st[i];
                    double tol = 1e-9 * (fabs(want) + 1.0);
                    if (!(fabs(want - got) <= tol) && !(want != want && got != got)) {
                        char b[96];
                        snprintf(b, sizeof b, " st[%d]=%g want %g", i, got, want);
                        why += b;
                    }
                }
            } else {
                for (int i = 0; i < 8; i++) {
                    u8 v[16];
                    uc_reg_read(uc, UC_X86_REG_XMM0 + i, v);
                    if (memcmp(v, c.xmm[i].b, 16)) {
                        char b[160];
                        snprintf(b, sizeof b, " xmm%d=%08x:%08x:%08x:%08x want %08x:%08x:%08x:%08x", i, c.xmm[i].u[3],
                                 c.xmm[i].u[2], c.xmm[i].u[1], c.xmm[i].u[0], ((u32 *)v)[3], ((u32 *)v)[2],
                                 ((u32 *)v)[1], ((u32 *)v)[0]);
                        why += b;
                    }
                }
            }
            std::vector<u8> ref(SCRATCH_SIZE);
            uc_mem_read(uc, SCRATCH, ref.data(), SCRATCH_SIZE);
            if (memcmp(ref.data(), x2_mem + SCRATCH, SCRATCH_SIZE)) {
                for (u32 o = 0; o < SCRATCH_SIZE; o++)
                    if (ref[o] != x2_mem[SCRATCH + o]) {
                        char b[96];
                        snprintf(b, sizeof b, " mem[%08x]=%02x want %02x", SCRATCH + o, x2_mem[SCRATCH + o], ref[o]);
                        why += b;
                        break;
                    }
            }
            if (!why.empty()) {
                fails++;
                printf("FAIL %08x %-40s%s\n", T.addr, T.text, why.c_str());
                printf("     in: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x fl=%03x\n",
                       regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7], flags);
            }
        }
        if (fails) failed_tests++;
    }
    printf("%u tests, %d executions, %d failing tests\n", x2_test_count, run, failed_tests);
    uc_close(uc);
    return failed_tests ? 1 : 0;
}
