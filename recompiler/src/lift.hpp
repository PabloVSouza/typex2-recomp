// x86-32 -> C lifter.
//
// Each guest function becomes `void f_XXXXXXXX(X2Cpu *c)`. General-purpose
// registers and EFLAGS bits are C locals (clang drops dead flag computations),
// spilled to `c` around anything that can observe them: calls, returns and
// runtime helpers. x87/SSE/MMX state lives in `c`.
//
// Generated code relies on the inline helpers in runtime/include/x2/recomp.h.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "discover.hpp"

namespace x2 {

struct Unsupported {
    std::string why;
};

class Lifter {
public:
    Lifter(const Image& img, Discovery& disc);
    ~Lifter();

    // Returns the C definition of the function.
    std::string lift_function(const Function& fn);
    // Lifts one non-control-flow instruction to C statements (for lifter tests).
    // Returns false if it is control flow or unsupported.
    bool lift_single(uint32_t addr, std::vector<std::string>& lines, std::string& text);

    std::map<std::string, int> unsupported;  // "mnemonic (reason)" -> count

    // Functions whose callers read EFLAGS set by the callee (hand-written CRT asm:
    // "call _load_arg / jz special"). They save the flags at ret (SPILLF) and
    // their direct call sites reload them (RELOADF). Run before lift_function.
    void analyze_flag_exports();
    std::set<uint32_t> flag_exporters;

    // used by the vector/x87 helpers
    std::string rn(unsigned reg) const;
    std::string ea(const cs_x86_op& op, bool with_seg = true) const;
    std::string rd(const cs_x86_op& op, int size = 0) const;
    std::string wr(const cs_x86_op& op, const std::string& val) const;

private:
    struct Out {
        std::vector<std::string> lines;
        bool falls = true;
    };
    Out lift(const Insn& ins, const Function& fn, const std::vector<uint32_t>* table);
    Out lift_seq(const cs_insn& d, const Insn& ins);
    bool lift_int(const std::string& m, const cs_insn& d, std::vector<std::string>& L);
    bool lift_string(const std::string& m, const cs_insn& d, std::vector<std::string>& L);
    bool lift_vec(const std::string& m, const cs_insn& d, std::vector<std::string>& L);
    std::vector<std::string> lift_x87(const cs_insn& d, const uint8_t* b, int opi);
    std::string branch_to(uint32_t target, const Function& fn) const;
    bool decode(uint32_t va);  // into ci_

    const Image& img_;
    Discovery& disc_;
    csh cs_ = 0;
    cs_insn* ci_ = nullptr;
    mutable uint32_t cur_addr_ = 0;  // instruction being lifted (back-edge detection)
};

// lift routines x2recomp would otherwise replace with natives (--no-natives)
extern bool lift_natives;
// apply the built-in byte patches (lift.cpp kPatches) to the image; returns how many
int apply_byte_patches(Image& img);

}  // namespace x2
