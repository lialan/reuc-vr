#include "ppc/insn.h"
#include "ppc/isa.h"
#include "translator/emit.h"
#include "translator/regeffect.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

// opcheck_gen <opcheck.cpp> [<regcheck.cpp>]
// opcheck.cpp: one call of every op the translator emits (it must compile).
// regcheck.cpp: random encodings of every op with the register-cache model (translator/regeffect.h) of each; tests/test_reg_effects.cpp
// checks the model against what the op really reads and writes.

static bool memory_op(const ppc::OpInfo& op) {
    const std::string n = op.name;
    using ppc::Kind;
    if (op.kind == Kind::PSQ || op.kind == Kind::PSQX || n == "dcbz" || n == "dcbz_l") return true;
    if (op.primary >= 32 && op.primary <= 55) return true;
    if (op.primary != 31 || op.kind == Kind::NONE) return false;
    for (const char* p : {"lwz", "lbz", "lhz", "lha", "lfs", "lfd", "stw", "stb", "sth", "stf", "lwbrx", "lhbrx", "lwarx", "dcbz"})
        if (n.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

static bool indexed(const ppc::OpInfo& op) {
    return op.kind == ppc::Kind::PSQX || op.kind == ppc::Kind::AB || (op.primary == 31 && op.kind == ppc::Kind::RRR);
}

static void write_regcheck(const char* path) {
    std::FILE* o = std::fopen(path, "w");
    if (!o) return;
    uc::Program p; uc::EmitStats st;
    p.reg_cache = false;
    uc::Function f{0x80003100, 0x80003200};
    std::mt19937 rng(12345);
    std::fprintf(o, "#include \"ppc_rt.h\"\n#include <cstddef>\n#include <cstdint>\n");
    std::fprintf(o, "struct RegCase { const char* name; uint32_t word; bool mem, idx; uint64_t use[2], may[2], must[2]; void (*fn)(ppc::Ctx&); };\n");
    std::string table;
    size_t n = 0, count = 0;
    auto* const* ops = ppc::all_ops(n);
    for (size_t i = 0; i < n; ++i) {
        const ppc::OpInfo& op = *ops[i];
        using ppc::Kind;
        if (op.kind == Kind::BR || op.kind == Kind::BC || op.kind == Kind::BCLR || op.kind == Kind::BCCTR || op.kind == Kind::SC || op.kind == Kind::UNSUP) continue;
        const bool mem = memory_op(op), idx = mem && indexed(op);
        for (int k = 0; k < 24; ++k) {
            uint32_t w = uint32_t(op.primary) << 26;
            uint32_t r = rng() & 0x03FFFFFFu;
            switch (op.sub) {
                case ppc::Sub::N: w |= r; break;
                case ppc::Sub::X5: w |= (r & ~0x3Eu) | uint32_t(op.xo) << 1; break;
                case ppc::Sub::X6: w |= (r & ~0x7Fu) | uint32_t(op.xo) << 1; break;
                case ppc::Sub::X9: w |= (r & ~0x7FEu) | uint32_t(op.xo) << 1; break;   // OE = 0
                case ppc::Sub::X10: w |= (r & ~0x7FEu) | uint32_t(op.xo) << 1; break;
            }
            if (ppc::decode(w) != &op) continue;
            if (mem) {
                // the test points every base register at RAM: rA is never 0, never the index register, and displacements stay small
                if (ppc::a(w) == 0) w |= 1u << 16;
                if (idx && ppc::b(w) == ppc::a(w)) w = (w & ~(31u << 11)) | (((ppc::a(w) + 1) & 31) << 11);
                if (idx && ppc::b(w) == 0) w |= 2u << 11;
                if (op.kind == Kind::PSQ) w = (w & ~0xFFFu) | (rng() & 0x1FFu);
                else if (op.sub == ppc::Sub::N) w = (w & ~0xFFFFu) | (uint32_t(int32_t(rng() % 512) - 256) & 0xFFFFu);
            }
            if (std::string(op.name) == "mtspr" || std::string(op.name) == "mfspr") {
                static const uint32_t sprs[] = {1, 8, 9, 912, 913, 917, 919, 272, 1008};
                const uint32_t s = sprs[rng() % (sizeof sprs / sizeof sprs[0])];
                w = (w & ~(0x3FFu << 11)) | ((s & 31) << 16) | ((s >> 5) << 11);
            }
            const std::string code = uc::emit_insn(p, f, 0x80003100, w, st);
            if (code.find("ppc::op_") != 0) continue;
            bool ok = false;
            const uc::RegEffect e = uc::reg_effect(op, w, op.rc && ppc::rc(w), ok);
            if (!ok) continue;
            auto lo = [](const uc::RegSet& s) { return (s & uc::RegSet(~0ull)).to_ullong(); };
            auto hi = [](const uc::RegSet& s) { return (s >> 64).to_ullong(); };
            std::fprintf(o, "static void f%zu(ppc::Ctx& c) { %s }\n", count, code.c_str());
            char row[512];
            std::snprintf(row, sizeof row, "  {\"%s\", 0x%08Xu, %s, %s, {0x%llXull, 0x%llXull}, {0x%llXull, 0x%llXull}, {0x%llXull, 0x%llXull}, f%zu},\n", op.name, w,
                          mem ? "true" : "false", idx ? "true" : "false", (unsigned long long)lo(e.use), (unsigned long long)hi(e.use), (unsigned long long)lo(e.may),
                          (unsigned long long)hi(e.may), (unsigned long long)lo(e.must), (unsigned long long)hi(e.must), count);
            table += row;
            ++count;
        }
    }
    std::fprintf(o, "extern const RegCase kRegCases[] = {\n%s};\nextern const size_t kRegCaseCount = %zu;\n", table.c_str(), count);
    std::fclose(o);
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    uc::Program p; uc::EmitStats st;
    p.reg_cache = false;
    uc::Function f{0x80003100, 0x80003200};
    std::FILE* o = std::fopen(argv[1], "w");
    if (!o) return 1;
    std::fprintf(o, "#include \"ppc_rt.h\"\nvoid opcheck(ppc::Ctx& c) {\n");
    size_t n = 0;
    auto* const* ops = ppc::all_ops(n);
    for (size_t i = 0; i < n; ++i) {
        const ppc::OpInfo& op = *ops[i];
        using ppc::Kind;
        if (op.kind == Kind::BR || op.kind == Kind::BC || op.kind == Kind::BCLR || op.kind == Kind::BCCTR || op.kind == Kind::SC || op.kind == Kind::UNSUP) continue;
        uint32_t w = uint32_t(op.primary) << 26 | (op.sub == ppc::Sub::N ? 0u : uint32_t(op.xo) << 1);
        std::string s = uc::emit_insn(p, f, 0x80003100, w, st);
        if (s.find("ppc::op_") != 0) continue;   // traps (OE forms, twi 31) are not op calls
        std::fprintf(o, "  %s\n", s.c_str());
    }
    std::fprintf(o, "}\n");
    std::fclose(o);
    if (argc == 3) write_regcheck(argv[2]);
    return 0;
}
