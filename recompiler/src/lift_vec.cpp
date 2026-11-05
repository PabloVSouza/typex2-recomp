// SSE / SSE2 / MMX lifting.
//
// Vector registers live in c->xmm[i] and c->mm[i] (both X2Vec; MMX uses the
// low 8 bytes). Each handler loads its source into a temporary `s`, operates on
// a copy `r` of the destination and writes `r` back, so src==dst aliasing is
// trivially correct.
#include <format>
#include <functional>
#include <unordered_map>

#include "lift.hpp"

using std::format;
using std::string;
using std::vector;

namespace x2 {

namespace {

string lanes(int n, const string& tmpl) {
    // replaces every "{k}" with the lane index
    string out;
    for (int k = 0; k < n; k++) {
        string s = tmpl;
        for (size_t p; (p = s.find("{k}")) != string::npos;) s.replace(p, 3, std::to_string(k));
        out += s + " ";
    }
    return out;
}

const std::unordered_map<string, string> kCmp = {
    {"eq", "a==b"}, {"lt", "a<b"}, {"le", "a<=b"}, {"unord", "a!=a||b!=b"},
    {"neq", "!(a==b)"}, {"nlt", "!(a<b)"}, {"nle", "!(a<=b)"}, {"ord", "a==a&&b==b"}};

}  // namespace

bool Lifter::lift_vec(const string& m, const cs_insn& d, vector<string>& L) {
    const cs_x86& x = d.detail->x86;
    const cs_x86_op* o = x.operands;
    int nops = x.op_count;

    // vector register name -> (C lvalue, width)
    auto vreg = [&](const cs_x86_op& op, int* width = nullptr) -> string {
        if (op.type != X86_OP_REG) return "";
        string n = rn(op.reg);
        if (n.rfind("xmm", 0) == 0) {
            if (width) *width = 16;
            return format("c->xmm[{}]", n.substr(3));
        }
        if (n.rfind("mm", 0) == 0) {
            if (width) *width = 8;
            return format("c->mm[{}]", n.substr(2));
        }
        return "";
    };
    auto load = [&](const cs_x86_op& op, int size = 0) -> string {
        if (op.type == X86_OP_REG) {
            string r = vreg(op);
            if (!r.empty()) return format("X2Vec s={};", r);
            return format("X2Vec s={{0}}; s.u[0]={};", rd(op));
        }
        if (op.type == X86_OP_MEM) return format("X2Vec s={{0}}; LDV(&s,{},{});", ea(op), size ? size : op.size);
        throw Unsupported{"vec src"};
    };
    auto dst = [&](const cs_x86_op& op) -> std::pair<string, string> {
        if (op.type == X86_OP_REG) {
            string r = vreg(op);
            if (r.empty()) throw Unsupported{"vec dst gpr"};
            return {format("X2Vec r={};", r), format("{}=r;", r)};
        }
        return {format("X2Vec r={{0}}; LDV(&r,{},{});", ea(op), op.size), format("STV({},&r,{});", ea(op), op.size)};
    };
    auto op2 = [&](const string& body, int n = 0) {
        auto [pre, post] = dst(o[0]);
        L.push_back(format("{{ {} {} {} {} }}", load(o[1], n), pre, body, post));
        return true;
    };

    // packed / scalar float arithmetic
    static const std::pair<const char*, const char*> arith[] = {{"add", "+"}, {"sub", "-"}, {"mul", "*"}, {"div", "/"}};
    for (auto [k, sym] : arith) {
        string ks = k;
        if (m == ks + "ps") return op2(lanes(4, format("r.f[{{k}}]=r.f[{{k}}]{}s.f[{{k}}];", sym)));
        if (m == ks + "ss") return op2(format("r.f[0]=r.f[0]{}s.f[0];", sym), 4);
        if (m == ks + "pd") return op2(lanes(2, format("r.d[{{k}}]=r.d[{{k}}]{}s.d[{{k}}];", sym)));
        if (m == ks + "sd") return op2(format("r.d[0]=r.d[0]{}s.d[0];", sym), 8);
    }

    struct Simple {
        string body;
        int load_size;  // 0 = operand size
    };
    static const std::unordered_map<string, Simple> simple = {
        {"maxps", {lanes(4, "r.f[{k}]=r.f[{k}]>s.f[{k}]?r.f[{k}]:s.f[{k}];"), 0}},
        {"minps", {lanes(4, "r.f[{k}]=r.f[{k}]<s.f[{k}]?r.f[{k}]:s.f[{k}];"), 0}},
        {"maxss", {"r.f[0]=r.f[0]>s.f[0]?r.f[0]:s.f[0];", 4}},
        {"minss", {"r.f[0]=r.f[0]<s.f[0]?r.f[0]:s.f[0];", 4}},
        {"maxsd", {"r.d[0]=r.d[0]>s.d[0]?r.d[0]:s.d[0];", 8}},
        {"minsd", {"r.d[0]=r.d[0]<s.d[0]?r.d[0]:s.d[0];", 8}},
        {"sqrtps", {lanes(4, "r.f[{k}]=x2_sqrtf(s.f[{k}]);"), 0}},
        {"sqrtss", {"r.f[0]=x2_sqrtf(s.f[0]);", 4}},
        {"sqrtsd", {"r.d[0]=x2_sqrt(s.d[0]);", 8}},
        {"rcpps", {lanes(4, "r.f[{k}]=1.0f/s.f[{k}];"), 0}},
        {"rcpss", {"r.f[0]=1.0f/s.f[0];", 4}},
        {"rsqrtps", {lanes(4, "r.f[{k}]=1.0f/sqrtf(s.f[{k}]);"), 0}},
        {"rsqrtss", {"r.f[0]=1.0f/sqrtf(s.f[0]);", 4}},
        {"unpcklps", {"r.u[3]=s.u[1]; r.u[2]=r.u[1]; r.u[1]=s.u[0];", 0}},
        {"unpckhps", {"r.u[0]=r.u[2]; r.u[1]=s.u[2]; r.u[2]=r.u[3]; r.u[3]=s.u[3];", 0}},
        {"unpcklpd", {"r.q[1]=s.q[0];", 0}},
        {"unpckhpd", {"r.q[0]=r.q[1]; r.q[1]=s.q[1];", 0}},
        {"movhlps", {"r.q[0]=s.q[1];", 0}},
        {"movlhps", {"r.q[1]=s.q[0];", 0}},
        {"cvtps2pd", {"r.d[1]=(f64)s.f[1]; r.d[0]=(f64)s.f[0];", 8}},
        {"cvtpd2ps", {"r.f[0]=(f32)s.d[0]; r.f[1]=(f32)s.d[1]; r.q[1]=0;", 0}},
        {"cvtss2sd", {"r.d[0]=(f64)s.f[0];", 4}},
        {"cvtsd2ss", {"r.f[0]=(f32)s.d[0];", 8}},
        {"cvtdq2ps", {lanes(4, "r.f[{k}]=(f32)s.i[{k}];"), 0}},
        {"cvtdq2pd", {"r.d[1]=(f64)s.i[1]; r.d[0]=(f64)s.i[0];", 8}},
        {"cvtps2dq", {lanes(4, "r.i[{k}]=x2_cvt_f2i(c,s.f[{k}]);"), 0}},
        {"cvttps2dq", {lanes(4, "r.i[{k}]=x2_cvtt_f2i(s.f[{k}]);"), 0}},
        {"cvtpi2ps", {"r.f[0]=(f32)s.i[0]; r.f[1]=(f32)s.i[1];", 8}},
        {"cvtps2pi", {"r.i[0]=x2_cvt_f2i(c,s.f[0]); r.i[1]=x2_cvt_f2i(c,s.f[1]);", 8}},
        {"cvttps2pi", {"r.i[0]=x2_cvtt_f2i(s.f[0]); r.i[1]=x2_cvtt_f2i(s.f[1]);", 8}},
        {"andps", {lanes(2, "r.q[{k}]&=s.q[{k}];"), 0}}, {"andpd", {lanes(2, "r.q[{k}]&=s.q[{k}];"), 0}},
        {"pand", {lanes(2, "r.q[{k}]&=s.q[{k}];"), 0}},
        {"orps", {lanes(2, "r.q[{k}]|=s.q[{k}];"), 0}}, {"orpd", {lanes(2, "r.q[{k}]|=s.q[{k}];"), 0}},
        {"por", {lanes(2, "r.q[{k}]|=s.q[{k}];"), 0}},
        {"xorps", {lanes(2, "r.q[{k}]^=s.q[{k}];"), 0}}, {"xorpd", {lanes(2, "r.q[{k}]^=s.q[{k}];"), 0}},
        {"pxor", {lanes(2, "r.q[{k}]^=s.q[{k}];"), 0}},
        {"andnps", {lanes(2, "r.q[{k}]=~r.q[{k}]&s.q[{k}];"), 0}},
        {"andnpd", {lanes(2, "r.q[{k}]=~r.q[{k}]&s.q[{k}];"), 0}},
        {"pandn", {lanes(2, "r.q[{k}]=~r.q[{k}]&s.q[{k}];"), 0}},
        {"paddd", {lanes(4, "r.u[{k}]+=s.u[{k}];"), 0}}, {"psubd", {lanes(4, "r.u[{k}]-=s.u[{k}];"), 0}},
        {"paddq", {lanes(2, "r.q[{k}]+=s.q[{k}];"), 0}}, {"psubq", {lanes(2, "r.q[{k}]-=s.q[{k}];"), 0}},
        {"paddw", {lanes(8, "r.w[{k}]+=s.w[{k}];"), 0}}, {"psubw", {lanes(8, "r.w[{k}]-=s.w[{k}];"), 0}},
        {"pcmpeqd", {lanes(4, "r.u[{k}]=r.u[{k}]==s.u[{k}]?~0u:0;"), 0}},
        {"pcmpgtw", {lanes(8, "r.w[{k}]=r.sw[{k}]>s.sw[{k}]?0xffff:0;"), 0}},
        {"pmaxsw", {lanes(8, "r.sw[{k}]=r.sw[{k}]>s.sw[{k}]?r.sw[{k}]:s.sw[{k}];"), 0}},
        {"punpcklbw", {"x2_punpckl(&r,&s,1,VW);", 0}}, {"punpckhbw", {"x2_punpckh(&r,&s,1,VW);", 0}},
        {"punpcklwd", {"x2_punpckl(&r,&s,2,VW);", 0}}, {"punpckhwd", {"x2_punpckh(&r,&s,2,VW);", 0}},
        {"punpckldq", {"x2_punpckl(&r,&s,4,VW);", 0}}, {"punpckhdq", {"x2_punpckh(&r,&s,4,VW);", 0}},
        {"punpcklqdq", {"r.q[1]=s.q[0];", 0}}, {"punpckhqdq", {"r.q[0]=r.q[1]; r.q[1]=s.q[1];", 0}},
        {"packssdw", {"x2_packssdw(&r,&s,VW);", 0}},
    };
    if (auto it = simple.find(m); it != simple.end()) {
        string body = it->second.body;
        if (auto p = body.find("VW"); p != string::npos) {
            int w = 16;
            vreg(o[0], &w);
            body.replace(p, 2, std::to_string(w));
        }
        return op2(body, it->second.load_size);
    }

    // shifts by immediate or by register/memory count
    struct Shift {
        const char* name;
        int lanes;
        const char* expr;
    };
    static const Shift shifts[] = {
        {"psrlq", 2, "r.q[{k}]=n>63?0:r.q[{k}]>>n;"}, {"psllq", 2, "r.q[{k}]=n>63?0:r.q[{k}]<<n;"},
        {"psrld", 4, "r.u[{k}]=n>31?0:r.u[{k}]>>n;"}, {"pslld", 4, "r.u[{k}]=n>31?0:r.u[{k}]<<n;"},
        {"psrad", 4, "r.i[{k}]=r.i[{k}]>>(n>31?31:n);"}, {"psrlw", 8, "r.w[{k}]=n>15?0:r.w[{k}]>>n;"},
        {"psllw", 8, "r.w[{k}]=n>15?0:r.w[{k}]<<n;"}, {"psraw", 8, "r.sw[{k}]=r.sw[{k}]>>(n>15?15:n);"},
    };
    for (auto& s : shifts) {
        if (m != s.name) continue;
        auto [pre, post] = dst(o[0]);
        string n = o[1].type == X86_OP_IMM ? format("u64 n={};", o[1].imm) : load(o[1], 8) + " u64 n=s.q[0];";
        L.push_back(format("{{ {} {} {} {} }}", n, pre, lanes(s.lanes, s.expr), post));
        return true;
    }

    // full-width moves
    if (m == "movaps" || m == "movups" || m == "movapd" || m == "movupd" || m == "movdqa" || m == "movdqu" ||
        m == "movntps" || m == "movntdq" || m == "lddqu") {
        if (o[0].type == X86_OP_MEM) L.push_back(format("{{ {} STV({},&s,16); }}", load(o[1]), ea(o[0])));
        else L.push_back(format("{{ {} {}=s; }}", load(o[1]), vreg(o[0])));
        return true;
    }
    // partial-width moves
    static const std::unordered_map<string, int> pmov = {
        {"movss", 4}, {"movd", 4}, {"movsd", 8}, {"movq", 8}, {"movntq", 8},
        {"movlps", 8}, {"movlpd", 8}, {"movhps", 8}, {"movhpd", 8}};
    if (auto it = pmov.find(m); it != pmov.end()) {
        int n = it->second;
        bool hi = m == "movhps" || m == "movhpd";
        int wa = 0, wb = 0;
        string ra = vreg(o[0], &wa), rb = vreg(o[1], &wb);
        if (o[0].type == X86_OP_MEM) {  // store
            if (rb.empty()) throw Unsupported{"vec store src"};
            L.push_back(format("STV({},(X2Vec*)({}.b+{}),{});", ea(o[0]), rb, hi ? 8 : 0, n));
        } else if (ra.empty()) {  // movd r32, xmm/mm
            L.push_back(wr(o[0], rb + ".u[0]"));
        } else if (hi) {
            L.push_back(format("LDV((X2Vec*)({}.b+8),{},8);", ra, ea(o[1])));
        } else if (o[1].type == X86_OP_MEM) {
            if (m == "movlps" || m == "movlpd") L.push_back(format("LDV(&{},{},8);", ra, ea(o[1])));
            else L.push_back(format("{{ X2Vec s={{0}}; LDV(&s,{},{}); {}=s; }}", ea(o[1]), n, ra));
        } else if (rb.empty()) {  // movd xmm/mm, r32
            L.push_back(format("{{ X2Vec s={{0}}; s.u[0]={}; {}=s; }}", rd(o[1]), ra));
        } else if (m == "movss") {
            L.push_back(format("{}.u[0]={}.u[0];", ra, rb));
        } else if (m == "movsd") {
            L.push_back(format("{}.q[0]={}.q[0];", ra, rb));
        } else if (wa == 16) {  // movq xmm, xmm/mm zero-extends
            L.push_back(format("{{ X2Vec s={{0}}; s.q[0]={}.q[0]; {}=s; }}", rb, ra));
        } else {
            L.push_back(format("{}.q[0]={}.q[0];", ra, rb));
        }
        return true;
    }
    if (m == "movmskps" || m == "movmskpd" || m == "pmovmskb") {
        int w = 16;
        string r = vreg(o[1], &w);
        string e = m == "movmskps"
                       ? format("(({0}.u[0]>>31)|(({0}.u[1]>>31)<<1)|(({0}.u[2]>>31)<<2)|(({0}.u[3]>>31)<<3))", r)
                   : m == "movmskpd" ? format("(u32)(({0}.q[0]>>63)|(({0}.q[1]>>63)<<1))", r)
                                     : format("x2_pmovmskb(&{},{})", r, w);
        L.push_back(wr(o[0], e));
        return true;
    }
    if (m == "shufps" || m == "pshufd" || m == "shufpd" || m == "pshufw") {
        auto [pre, post] = dst(o[0]);
        string body;
        if (m == "shufps")
            body = "X2Vec t=r; r.u[0]=t.u[i&3]; r.u[1]=t.u[(i>>2)&3]; r.u[2]=s.u[(i>>4)&3]; r.u[3]=s.u[(i>>6)&3];";
        else if (m == "pshufd")
            body = "r.u[0]=s.u[i&3]; r.u[1]=s.u[(i>>2)&3]; r.u[2]=s.u[(i>>4)&3]; r.u[3]=s.u[(i>>6)&3];";
        else if (m == "shufpd")
            body = "X2Vec t=r; r.q[0]=t.q[i&1]; r.q[1]=s.q[(i>>1)&1];";
        else
            body = "r.w[0]=s.w[i&3]; r.w[1]=s.w[(i>>2)&3]; r.w[2]=s.w[(i>>4)&3]; r.w[3]=s.w[(i>>6)&3];";
        L.push_back(format("{{ {} {} u32 i={}; {} {} }}", load(o[1], m == "pshufw" ? 8 : 0), pre, o[2].imm, body, post));
        return true;
    }
    if (m == "pextrw") {
        int w = 16;
        string r = vreg(o[1], &w);
        L.push_back(wr(o[0], format("{}.w[{}]", r, o[2].imm & (w == 16 ? 7 : 3))));
        return true;
    }
    if (m == "pinsrw") {
        int w = 16;
        string r = vreg(o[0], &w);
        string src = o[1].type == X86_OP_MEM ? rd(o[1], 2) : "(u16)" + rd(o[1]);
        L.push_back(format("{}.w[{}]={};", r, o[2].imm & (w == 16 ? 7 : 3), src));
        return true;
    }
    if (m == "comiss" || m == "ucomiss" || m == "comisd" || m == "ucomisd") {
        bool ss = m.ends_with("ss");
        L.push_back(format("{{ {} FCOMI_SSE({}.{}[0],s.{}[0]); }}", load(o[1], ss ? 4 : 8), vreg(o[0]), ss ? "f" : "d",
                           ss ? "f" : "d"));
        return true;
    }
    if (m.rfind("cmp", 0) == 0 && m.size() > 5) {
        string kind = m.substr(m.size() - 2), pred = m.substr(3, m.size() - 5);
        auto it = kCmp.find(pred);
        if (it != kCmp.end() && (kind == "ps" || kind == "ss" || kind == "pd" || kind == "sd")) {
            const string& c = it->second;
            if (kind == "ps") return op2(lanes(4, "{ f32 a=r.f[{k}], b=s.f[{k}]; r.u[{k}]=(" + c + ")?~0u:0; }"));
            if (kind == "ss") return op2("{ f32 a=r.f[0], b=s.f[0]; r.u[0]=(" + c + ")?~0u:0; }", 4);
            if (kind == "pd") return op2(lanes(2, "{ f64 a=r.d[{k}], b=s.d[{k}]; r.q[{k}]=(" + c + ")?~0ull:0; }"));
            return op2("{ f64 a=r.d[0], b=s.d[0]; r.q[0]=(" + c + ")?~0ull:0; }", 8);
        }
    }
    if (m == "cvtsi2ss" || m == "cvtsi2sd") {
        bool ss = m.ends_with("ss");
        L.push_back(format("{}.{}[0]=({})(s32){};", vreg(o[0]), ss ? "f" : "d", ss ? "f32" : "f64", rd(o[1], 4)));
        return true;
    }
    if (m == "cvttss2si" || m == "cvttsd2si" || m == "cvtss2si" || m == "cvtsd2si") {
        bool ss = m.find("ss2si") != string::npos; /* cvt[t]ss2si vs cvt[t]sd2si */
        bool trunc = m.rfind("cvtt", 0) == 0;
        string conv = trunc ? format("x2_cvtt_f2i(s.{}[0])", ss ? "f" : "d") : format("x2_cvt_f2i(c,s.{}[0])", ss ? "f" : "d");
        L.push_back(format("{{ {} {} }}", load(o[1], ss ? 4 : 8), wr(o[0], "(u32)" + conv)));
        return true;
    }
    (void)nops;
    return false;
}

}  // namespace x2
