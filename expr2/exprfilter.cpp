/*
* Copyright (c) 2012-2019 Fredrik Mellbin
* Copyright (c) 2021-     Akarin
*
* lexpr is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 3 of the License, or (at your option) any later version.
*
* lexpr is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with lexpr; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#define USE_EXPR_CACHE

#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>
#include "VapourSynth.h"
#include "VSHelper.h"
#include "version.h"

#include "Module.hpp"
#include "Debug.hpp"

namespace {

#define MAX_EXPR_INPUTS 26
#define LANES 8
#define UNROLL 1

#define ALIGNMENT 32 /* VapourSynth should guarantee at least this for all data */

enum class ExprOpType {
    // Terminals.
    MEM_LOAD, CONSTANTI, CONSTANTF, CONST_LOAD,
    VAR_LOAD, VAR_STORE,

    // Arithmetic primitives.
    ADD, SUB, MUL, DIV, MOD, SQRT, ABS, MAX, MIN, CLAMP, CMP,

    // Integer conversions.
    TRUNC, ROUND, FLOOR,

    // Logical operators.
    AND, OR, XOR, NOT,

    // Transcendental functions.
    EXP, LOG, POW, SIN, COS,

    // Ternary operator
    TERNARY,

    // Rank-order operator
    SORT,

    // Stack helpers.
    DUP, SWAP, DROP,
};

std::vector<std::string> features = {
    "x.property",
    "sin", "cos",
    "%", "clip", "clamp",
    "N", "X", "Y", "pi", "width", "height",
    "trunc", "round", "floor",
    "var@", "var!",
    "x[x,y]", "x[x,y]:m",
    "drop",
    "sort",
};

enum class ComparisonType {
    EQ = 0,
    LT = 1,
    LE = 2,
    NEQ = 4,
    NLT = 5,
    NLE = 6,
};

enum class LoadConstType {
    N = 0,
    X = 1,
    Y = 2,
    Width = 3,
    Height = 4,
    LAST = 5,
};

enum class LoadConstIndex {
    N = 0,
    LAST = 1,
};

enum class BoundaryCondition {
    Unspecified = 0,
    Clamped,
    Mirrored,
};

union ExprUnion {
    int32_t i;
    uint32_t u;
    float f;

    constexpr ExprUnion() : u{} {}

    constexpr ExprUnion(int32_t i) : i(i) {}
    constexpr ExprUnion(uint32_t u) : u(u) {}
    constexpr ExprUnion(float f) : f(f) {}
};

struct ExprOp {
    ExprOpType type;
    ExprUnion imm;
    std::string name;
    int x, y;
    BoundaryCondition bc;

    ExprOp(ExprOpType type, ExprUnion param = {}, std::string name = {}, int x = 0, int y = 0, BoundaryCondition bc = BoundaryCondition::Unspecified)
        : type(type), imm(param), name(name), x(x), y(y), bc(bc) {}
};

bool operator==(const ExprOp &lhs, const ExprOp &rhs) {
    return lhs.type == rhs.type && lhs.imm.u == rhs.imm.u && lhs.name == rhs.name &&
        lhs.x == rhs.x && lhs.y == rhs.y;
}
bool operator!=(const ExprOp &lhs, const ExprOp &rhs) { return !(lhs == rhs); }

enum PlaneOp {
    poProcess, poCopy, poUndefined
};

struct Compiled {
    std::shared_ptr<rr::Routine> routine;
    struct PropAccess {
        int clip;
        std::string name;
    };
    std::vector<PropAccess> propAccess;
};

struct ExprData {
    VSNodeRef *node[MAX_EXPR_INPUTS];
    VSVideoInfo vi;
    int plane[3];
    int numInputs;
    Compiled compiled[3];
    typedef void (*ProcessProc)(void *rwptrs, int strides[MAX_EXPR_INPUTS + 1], float *props, int width, int height);
    ProcessProc proc[3];

    ExprData() : node(), vi(), plane(), numInputs(), proc() {}
};

std::vector<std::string> tokenize(const std::string &expr)
{
    std::vector<std::string> tokens;
    auto it = expr.begin();
    auto prev = expr.begin();

    while (it != expr.end()) {
        char c = *it;

        if (std::isspace(c)) {
            if (it != prev)
                tokens.push_back(expr.substr(prev - expr.begin(), it - prev));
            prev = it + 1;
        }
        ++it;
    }
    if (prev != expr.end())
        tokens.push_back(expr.substr(prev - expr.begin(), expr.end() - prev));

    return tokens;
}

ExprOp decodeToken(const std::string &token)
{
    static const std::unordered_map<std::string, ExprOp> simple{
        { "+",    { ExprOpType::ADD } },
        { "-",    { ExprOpType::SUB } },
        { "*",    { ExprOpType::MUL } },
        { "/",    { ExprOpType::DIV } } ,
        { "%",    { ExprOpType::MOD } } ,
        { "sqrt", { ExprOpType::SQRT } },
        { "abs",  { ExprOpType::ABS } },
        { "max",  { ExprOpType::MAX } },
        { "min",  { ExprOpType::MIN } },
        { "clip", { ExprOpType::CLAMP } }, // for compat with AVS+ Expr
        { "clamp",{ ExprOpType::CLAMP } },
        { "<",    { ExprOpType::CMP, static_cast<int>(ComparisonType::LT) } },
        { ">",    { ExprOpType::CMP, static_cast<int>(ComparisonType::NLE) } },
        { "=",    { ExprOpType::CMP, static_cast<int>(ComparisonType::EQ) } },
        { ">=",   { ExprOpType::CMP, static_cast<int>(ComparisonType::NLT) } },
        { "<=",   { ExprOpType::CMP, static_cast<int>(ComparisonType::LE) } },
        { "trunc",{ ExprOpType::TRUNC } },
        { "round",{ ExprOpType::ROUND } },
        { "floor",{ ExprOpType::FLOOR } },
        { "and",  { ExprOpType::AND } },
        { "or",   { ExprOpType::OR } },
        { "xor",  { ExprOpType::XOR } },
        { "not",  { ExprOpType::NOT } },
        { "?",    { ExprOpType::TERNARY } },
        { "exp",  { ExprOpType::EXP } },
        { "log",  { ExprOpType::LOG } },
        { "pow",  { ExprOpType::POW } },
        { "sin",  { ExprOpType::SIN } },
        { "cos",  { ExprOpType::COS } },
        { "dup",  { ExprOpType::DUP, 0 } },
        { "swap", { ExprOpType::SWAP, 1 } },
        { "drop", { ExprOpType::DROP, 1 } },
        { "pi",   { ExprOpType::CONSTANTF, static_cast<float>(M_PI) } },
        { "N",    { ExprOpType::CONST_LOAD, static_cast<int>(LoadConstType::N) } },
        { "X",    { ExprOpType::CONST_LOAD, static_cast<int>(LoadConstType::X) } },
        { "Y",    { ExprOpType::CONST_LOAD, static_cast<int>(LoadConstType::Y) } },
        { "width",{ ExprOpType::CONST_LOAD, static_cast<int>(LoadConstType::Width) } },
        {"height",{ ExprOpType::CONST_LOAD, static_cast<int>(LoadConstType::Height) } },
    };
    static const std::regex relpixelRe { "^([a-z])\\[(-?[0-9]+),(-?[0-9]+)\\](:[cm])?$" };
    std::smatch match;

    auto it = simple.find(token);
    if (it != simple.end()) {
        return it->second;
    } else if (token.size() == 1 && token[0] >= 'a' && token[0] <= 'z') {
        return{ ExprOpType::MEM_LOAD, token[0] >= 'x' ? token[0] - 'x' : token[0] - 'a' + 3 };
    } else if ((token.back() != '@' && token.back() != '!') &&
               (token.substr(0, 3) == "dup" || token.substr(0, 4) == "swap" ||
                token.substr(0, 4) == "drop" || token.substr(0, 4) == "sort")) {
        size_t prefix = token[1] == 'u' ? 3 : 4;
        size_t count = 0;
        int idx = -1;

        try {
            idx = std::stoi(token.substr(prefix), &count);
        } catch (...) {
            // ...
        }

        if (idx < 0 || prefix + count != token.size())
            throw std::runtime_error("illegal token: " + token);
        if (token[1] == 'u')
            return{ ExprOpType::DUP, idx };
        else if (token[1] == 'w')
            return{ ExprOpType::SWAP, idx };
        else if (token[1] == 'r')
            return{ ExprOpType::DROP, idx };
        else //if (token[1] == 'o')
            return{ ExprOpType::SORT, idx };
    } else if (token.size() >= 2 && (token.back() == '@' || token.back() == '!')) {
        // 'name@' load variable; 'name!' store to variable.
        return{ token.back() == '@' ? ExprOpType::VAR_LOAD : ExprOpType::VAR_STORE, -1, token.substr(0, token.size()-1) };
    } else if (token.size() >= 3 && token[0] >= 'a' && token[0] <= 'z' && token[1] == '.') {
        // frame property access
        return{ ExprOpType::CONST_LOAD, static_cast<int>(LoadConstType::LAST) + (token[0] >= 'x' ? token[0] - 'x' : token[0] - 'a' + 3), token.substr(2) };
    } else if (std::regex_match(token, match, relpixelRe)) {
        ASSERT(match.size() == 5);
        auto clip = match[1].str(), sx = match[2].str(), sy = match[3].str(), flag = match[4].str();
        BoundaryCondition bc = flag.size() == 0 ? BoundaryCondition::Unspecified :
            (flag[1] == 'm' ? BoundaryCondition::Mirrored : BoundaryCondition::Clamped);
        return{ ExprOpType::MEM_LOAD, clip[0] >= 'x' ? clip[0] - 'x' : clip[0] - 'a' + 3, "", atoi(sx.c_str()), atoi(sy.c_str()), bc };
    } else {
        size_t pos = 0;
        long long l = 0;
        float f = 0;
        const size_t len = token.size();
        try {
            l = std::stoll(token, &pos, 0);
        } catch (...) {
            pos = 0;
        }
        if (pos == len) {
            if ((int32_t)l == l) return { ExprOpType::CONSTANTI, (int32_t)l };
            else if ((uint32_t)l == l) return { ExprOpType::CONSTANTI, (uint32_t)l };
            return { ExprOpType::CONSTANTF, (float)l };
        }
        try {
            f = std::stof(token, &pos);
        } catch (...) {
            pos = 0;
        }
        if (pos == len)
            return { ExprOpType::CONSTANTF, f };
        else if (pos > 0)
            throw std::runtime_error("failed to convert '" + token + "' to float, not the whole token could be converted");
        else
            throw std::runtime_error("failed to convert '" + token + "' to float");
    }
}

template<int lanes>
struct VectorTypes {
    typedef rr::Void Byte;
    typedef rr::Void UShort;
    typedef rr::Void Int;
    typedef rr::Void Float;
};

template<>
struct VectorTypes<4> {
    typedef rr::Byte4 Byte;
    typedef rr::UShort4 UShort;
    typedef rr::Int4 Int;
    typedef rr::Float4 Float;
    typedef uint16_t SwizzleMask;
};

template<>
struct VectorTypes<8> {
public:
    typedef rr::Byte8 Byte;
    typedef rr::UShort8 UShort;
    typedef rr::Int8 Int;
    typedef rr::Float8 Float;
    typedef uint32_t SwizzleMask;
};

static std::unordered_map<std::string, Compiled> exprCache;

template<int lanes>
class Compiler {
    struct Context {
        const std::string expr;
        std::vector<std::string> tokens;
        std::vector<ExprOp> ops;
        const VSVideoInfo *vo;
        const VSVideoInfo * const *vi;
        int numInputs;
        int optMask;
        bool mirror;
        bool cached;
        Context(const std::string &expr, const VSVideoInfo *vo, const VSVideoInfo *const *vi, int numInputs, int opt, int mirror):
            expr(expr), vo(vo), vi(vi), numInputs(numInputs), optMask(opt), mirror(!!mirror), cached(false) {
            auto iter = exprCache.find(key());
            if (iter != exprCache.end()) {
                cached = true;
                return;
            }

            tokens = tokenize(expr);
            for (const auto &tok: tokens) {
                auto op = decodeToken(tok);
                if (op.bc == BoundaryCondition::Unspecified)
                    op.bc = mirror ? BoundaryCondition::Mirrored : BoundaryCondition::Clamped;
                ops.push_back(op);
            }
        }
        enum {
            flagUseInteger = 1<<0,
        };
        static std::string videoInfoKey(const VSVideoInfo *vi) {
            std::stringstream ss;
            ss << vi->format->name << ";";
            return ss.str();
        }
        std::string key() const {
            std::stringstream ss;
            ss << "n=" << numInputs << "|opt=" << optMask << "|mirror=" << mirror
                << "|expr=" << expr << "|vo=" << videoInfoKey(vo);
            for (int i = 0; i < numInputs; i++)
                ss << "|vi" << i << "=" << videoInfoKey(vi[i]);
            return ss.str();
        }
        Compiled getCached() { return exprCache[key()]; }

        bool forceFloat() const { return !(optMask & flagUseInteger); }
    } ctx;

    using pointer = rr::Pointer<rr::Byte>;
    using Types = VectorTypes<lanes>;
    using ByteV = typename Types::Byte;
    using UShortV = typename Types::UShort;
    using IntV = typename Types::Int;
    using FloatV = typename Types::Float;

    struct Helper {
        using ftype = rr::ModuleFunction<FloatV(FloatV)>;
        using ftype2 = rr::ModuleFunction<FloatV(FloatV, FloatV)>;
        std::unique_ptr<ftype> Exp;
        std::unique_ptr<ftype> Log;
        std::unique_ptr<ftype> Sin;
        std::unique_ptr<ftype> Cos;
        std::unique_ptr<ftype2> Pow;
    };
    rr::RValue<FloatV> Exp_(rr::RValue<FloatV>);
    rr::RValue<FloatV> Log_(rr::RValue<FloatV>);
    rr::RValue<FloatV> SinCos_(rr::RValue<FloatV>, bool issin);

    class Value {
        std::variant<IntV, FloatV> v;
        bool constant;

    public:
        bool isFloat() { return std::holds_alternative<FloatV>(v); }
        bool isConst() { return constant; }

        Value(int x) : v(IntV(x)), constant(true) {}
        Value(float x) : v(FloatV(x)), constant(true) {}

        Value(IntV i) : v(i), constant(false) {}
        Value(rr::RValue<IntV> i) : v(IntV(i)), constant(false) {}
        Value(rr::Reference<IntV> i) : v(IntV(i)), constant(false) {}
        Value(FloatV f) : v(f), constant(false) {}
        Value(rr::RValue<FloatV> f) : v(FloatV(f)), constant(false) {}
        Value(rr::Reference<FloatV> f) : v(FloatV(f)), constant(false) {}

        FloatV f() { return std::get<FloatV>(v); }
        IntV i() { return std::get<IntV>(v); }

        FloatV ensureFloat() { return isFloat() ? f() : FloatV(i()); }

        Value Max(Value &rhs) { return (isFloat() || rhs.isFloat()) ? Value(rr::Max(f(), rhs.f())) : Value(rr::Max(i(), rhs.i())); }
        Value Min(Value &rhs) { return (isFloat() || rhs.isFloat()) ? Value(rr::Min(f(), rhs.f())) : Value(rr::Min(i(), rhs.i())); }
    };

    struct State {
        std::vector<pointer> wptrs;
        rr::Int strides[MAX_EXPR_INPUTS+1];
        rr::Pointer<rr::Float> consts;
        rr::Int width;
        rr::Int height;
        IntV xvec;

        rr::Int y;
        rr::Int x;

        std::vector<Value> variables;
    };

    Helper buildHelpers(rr::Module &mod);
    void buildOneIter(const Helper &helpers, State &state);

public:
    Compiler(const std::string &expr, const VSVideoInfo *vo, const VSVideoInfo * const *vi, int numInputs, int opt = 0, int mirror = 0) :
        ctx(expr, vo, vi, numInputs, opt, mirror) {}

    Compiled compile();
};

template<int lanes>
rr::RValue<typename Compiler<lanes>::FloatV> Compiler<lanes>::Exp_(rr::RValue<typename Compiler<lanes>::FloatV> x_)
{
    FloatV x = x_;
    using namespace rr;
    const float exp_hi = 88.3762626647949f, exp_lo = -88.3762626647949f, log2e  = 1.44269504088896341f,
          exp_c1 = 0.693359375f, exp_c2 = -2.12194440e-4f, exp_p0 = 1.9875691500E-4f, exp_p1 = 1.3981999507E-3f,
          exp_p2 = 8.3334519073E-3f, exp_p3 = 4.1665795894E-2f, exp_p4 = 1.6666665459E-1f, exp_p5 = 5.0000001201E-1f;
    x = Min(x, FloatV(exp_hi));
    x = Max(x, FloatV(exp_lo));
    FloatV fx = FloatV(log2e);
    fx = FMA(fx, x, FloatV(0.5f));
    IntV emm0 = RoundInt(fx);
    FloatV etmp = FloatV(emm0);
    FloatV mask = As<FloatV>(As<IntV>(FloatV(1.0f)) & CmpGT(etmp, fx));
    fx = etmp - mask;
    x = FMA(fx, FloatV(-exp_c1), x);
    x = FMA(fx, FloatV(-exp_c2), x);
    FloatV z = x * x;
    FloatV y = FloatV(exp_p0);
    y = FMA(y, x, FloatV(exp_p1));
    y = FMA(y, x, FloatV(exp_p2));
    y = FMA(y, x, FloatV(exp_p3));
    y = FMA(y, x, FloatV(exp_p4));
    y = FMA(y, x, FloatV(exp_p5));
    y = FMA(y, z, x);
    y = y + FloatV(1.0f);
    emm0 = RoundInt(fx);
    emm0 = emm0 + IntV(0x7f);
    emm0 = emm0 << 23;
    x = y * As<FloatV>(emm0);
    return x;
}

template<int lanes>
rr::RValue<typename Compiler<lanes>::FloatV> Compiler<lanes>::Log_(rr::RValue<typename Compiler<lanes>::FloatV> x_)
{
    FloatV x = x_;
    using namespace rr;
    const uint32_t min_norm_pos = 0x00800000, inv_mant_mask = ~0x7F800000;
    const float float_half = 0.5f, sqrt_1_2 = 0.707106781186547524f, log_p0 = 7.0376836292E-2f, log_p1 = -1.1514610310E-1f,
          log_p2 = 1.1676998740E-1f, log_p3 = -1.2420140846E-1f, log_p4 = +1.4249322787E-1f, log_p5 = -1.6668057665E-1f,
          log_p6 = +2.0000714765E-1f, log_p7 = -2.4999993993E-1f, log_p8 = +3.3333331174E-1f, log_q2 = 0.693359375f,
          log_q1 = -2.12194440e-4f;
    const float zero = 0.0f, one = 1.0f;
    IntV invalid_mask = CmpLE(x, FloatV(zero));
    x = Max(x, As<FloatV>(IntV(min_norm_pos)));
    IntV emm0i = As<IntV>(x) >> 23;
    x = As<FloatV>(As<IntV>(x) & IntV(inv_mant_mask));
    x = As<FloatV>(As<IntV>(x) | As<IntV>(FloatV(float_half)));
    emm0i = emm0i - IntV(0x7f);
    FloatV emm0 = FloatV(emm0i);
    emm0 = emm0 + FloatV(one);
    IntV mask = CmpLT(x, FloatV(sqrt_1_2));
    FloatV etmp = As<FloatV>(mask & As<IntV>(x));
    x = x - FloatV(one);
    FloatV maskf = As<FloatV>(mask & As<IntV>(FloatV(one)));
    emm0 = emm0 - maskf;
    x = x + etmp;
    FloatV z = x * x;
    FloatV y = FloatV(log_p0);
    y = FMA(y, x, FloatV(log_p1));
    y = FMA(y, x, FloatV(log_p2));
    y = FMA(y, x, FloatV(log_p3));
    y = FMA(y, x, FloatV(log_p4));
    y = FMA(y, x, FloatV(log_p5));
    y = FMA(y, x, FloatV(log_p6));
    y = FMA(y, x, FloatV(log_p7));
    y = FMA(y, x, FloatV(log_p8));
    y = y * x;
    y = y * z;
    y = FMA(emm0, FloatV(log_q1), y);
    y = FMA(z, FloatV(-float_half), y);
    x = x + y;
    x = FMA(emm0, FloatV(log_q2), x);
    x = As<FloatV>(invalid_mask | As<IntV>(x));
    return x;
}

template<int lanes>
rr::RValue<typename Compiler<lanes>::FloatV> Compiler<lanes>::SinCos_(rr::RValue<typename Compiler<lanes>::FloatV> x_, bool issin)
{
    FloatV x = x_;
    using namespace rr;
    auto conv = [](uint32_t x) -> FloatV { return As<FloatV>(IntV(x)); };
    IntV absmask = IntV(0x7FFFFFFF);
    FloatV float_invpi = conv(0x3ea2f983),
           float_rintf = conv(0x4b400000),
           float_pi1 = conv(0x40490000),
           float_pi2 = conv(0x3a7da000),
           float_pi3 = conv(0x34222000),
           float_pi4 = conv(0x2cb4611a),
           float_sinC3 = conv(0xbe2aaaa6),
           float_sinC5 = conv(0x3c08876a),
           float_sinC7 = conv(0xb94fb7ff),
           float_sinC9 = conv(0x362edef8),
           float_cosC2 = conv(0xBEFFFFE2),
           float_cosC4 = conv(0x3D2AA73C),
           float_cosC6 = conv(0XBAB58D50),
           float_cosC8 = conv(0x37C1AD76);
    IntV sign;
    if (issin)
        sign = As<IntV>(x) & ~absmask;
    else
        sign = IntV(0);
    FloatV t1 = Abs(x);
    // Range reduction
    FloatV t2 = t1 * float_invpi;
    IntV t2i = RoundInt(t2);
    IntV t4 = t2i << 31;
    sign = sign ^ t4;
    t2 = FloatV(t2i);

    t1 = FMA(t2, -float_pi1, t1);
    t1 = FMA(t2, -float_pi2, t1);
    t1 = FMA(t2, -float_pi3, t1);
    t1 = FMA(t2, -float_pi4, t1);

    if (issin) {
        // minimax polynomial for sin(x) in [-pi/2, pi/2] interval.
        // compute X + X * X^2 * (C3 + X^2 * (C5 + X^2 * (C7 + X^2 * C9)))
        t2 = t1 * t1;
        FloatV t3 = FMA(t2, float_sinC9, float_sinC7);
        t3 = FMA(t3, t2, float_sinC5);
        t3 = FMA(t3, t2, float_sinC3);
        t3 = t3 * t2;
        t3 = t3 * t1;
        t1 = t1 + t3;
    } else {
        // minimax polynomial for cos(x) in [-pi/2, pi/2] interval.
        // compute 1 + X^2 * (C2 + X^2 * (C4 + X^2 * (C6 + X^2 * C8)))
        t1 = t1 * t1;
        FloatV t2 = FMA(t1, float_cosC8, float_cosC6);
        t2 = FMA(t2, t1, float_cosC4);
        t2 = FMA(t2, t1, float_cosC2);
        t1 = FMA(t2, t1, FloatV(1.0f));
    }
    // Apply sign.
    return As<FloatV>(sign ^ As<IntV>(t1));
}

template<int lanes, typename VType>
static VType relativeAccessAdjust(rr::Int &x, rr::Int &alignedx, rr::Int &width, const ExprOp &op, VType v)
{
    using namespace rr;
    if (op.x == 0)
        return v;
    else if (op.bc == BoundaryCondition::Mirrored)
        return v;
    else if (op.bc == BoundaryCondition::Clamped) {
        BasicBlock *contBb = Nucleus::createBasicBlock();
        if (op.x < 0) {
            int absx = std::abs(op.x);
            SwitchCases *switchCases = Nucleus::createSwitch(alignedx.loadValue(), contBb, (absx + lanes - 1) / lanes);
            for (int i = 0; i < absx; i += lanes) {
                BasicBlock *bb = Nucleus::createBasicBlock();
                Nucleus::addSwitchCase(switchCases, i, bb);
                Nucleus::setInsertBlock(bb);
                typename VectorTypes<lanes>::SwizzleMask select = 0;
                for (int j = 0; j < lanes; j++) {
                    select <<= 4;
                    select |= std::max(i + j + op.x, 0) % lanes;
                }
                v = Swizzle(v, select);
                Nucleus::createBr(contBb);
            }
        } else {
            Int dist = x + lanes - width;
            BasicBlock *switchBb = Nucleus::createBasicBlock();
            rr::Bool cond = dist > 0;
            Nucleus::createCondBr(cond.loadValue(), switchBb, contBb);
            Nucleus::setInsertBlock(switchBb);
            BasicBlock *defaultBb = Nucleus::createBasicBlock();
            SwitchCases *switchCases = Nucleus::createSwitch(dist.loadValue(), defaultBb, lanes-2);
            for (int i = 1; i < lanes-1; i++) {
                BasicBlock *bb = Nucleus::createBasicBlock();
                Nucleus::addSwitchCase(switchCases, i, bb);
                Nucleus::setInsertBlock(bb);
                typename VectorTypes<lanes>::SwizzleMask select = 0, last = 0;
                for (int j = 0; j < lanes; j++) {
                    last = select & 0xf;
                    select <<= 4;
                    if (j + i < lanes)
                        select |= j;
                    else
                        select |= last;
                }
                v = Swizzle(v, select);
                Nucleus::createBr(contBb);
            }
            Nucleus::setInsertBlock(defaultBb);
            v = Swizzle(v, 0);
            Nucleus::createBr(contBb);
        }
        Nucleus::setInsertBlock(contBb);
    }
    return v;
}

typedef std::vector<std::pair<int, int>> SortingNetwork;
static const SortingNetwork &buildSortNet(int n) {
    static std::map<int, SortingNetwork> built;
    auto it = built.find(n);
    if (it != built.end()) return it->second;

    built.insert({ n, {} });
    auto &sn = built.find(n)->second;

    int t = 0;
    while (n > (1<<t)) t++;
    int p = 1 << (t - 1);
    while (p > 0) {
        int q = 1 << (t - 1), r = 0, d = p;
        while (d > 0) {
            for (int i = 0; i < n - d; i++)
                if ((i & p) == r)
                    sn.emplace_back(i, i+d);
            d = q - p;
            q >>= 1;
            r = p;
        }
        p >>= 1;
    }
    return sn;
}

template<int lanes>
void Compiler<lanes>::buildOneIter(const Helper &helpers, State &state)
{
    constexpr unsigned char numOperands[] = {
        0, // MEM_LOAD
        0, // CONSTANTI
        0, // CONSTANTF
        0, // CONST_LOAD
        0, // VAR_LOAD
        1, // VAR_STORE
        2, // ADD
        2, // SUB
        2, // MUL
        2, // DIV
        2, // MOD
        1, // SQRT
        1, // ABS
        2, // MAX
        2, // MIN
        3, // CLAMP
        2, // CMP
        1, // TRUNC
        1, // ROUND
        1, // FLOOR
        2, // AND
        2, // OR
        2, // XOR
        1, // NOT
        1, // EXP
        1, // LOG
        2, // POW
        1, // SIN
        1, // COS
        3, // TERNARY
        0, // SORT
        0, // DUP
        0, // SWAP
        0, // DROP
    };
    static_assert(sizeof(numOperands) == static_cast<unsigned>(ExprOpType::DROP) + 1, "invalid table");

    using namespace rr;
    std::vector<Value> stack;

    for (size_t i = 0; i < ctx.ops.size(); i++) {
        const std::string &tok = ctx.tokens[i];
        const ExprOp &op = ctx.ops[i];

        // Check validity.
        if (op.type == ExprOpType::MEM_LOAD && op.imm.i >= ctx.numInputs)
            throw std::runtime_error("reference to undefined clip: " + tok);
        if ((op.type == ExprOpType::DUP || op.type == ExprOpType::SWAP) && op.imm.u >= stack.size())
            throw std::runtime_error("insufficient values on stack: " + tok);
        if ((op.type == ExprOpType::DROP || op.type == ExprOpType::SORT) && op.imm.u > stack.size())
            throw std::runtime_error("insufficient values on stack: " + tok);
        if (stack.size() < numOperands[static_cast<size_t>(op.type)])
            throw std::runtime_error("insufficient values on stack: " + tok);

#define OUT(x) stack.push_back(x)
        switch (op.type) {
        case ExprOpType::DUP:
            stack.push_back(stack[stack.size() - 1 - op.imm.u]);
            break;
        case ExprOpType::SWAP: {
            std::swap(stack[stack.size()-1], stack[stack.size() - 1 - op.imm.u]);
            break;
        }
        case ExprOpType::DROP: {
            for (unsigned i = 0; i < op.imm.u; i++)
                stack.pop_back();
            break;
        }

        case ExprOpType::SORT: {
            // "3 7 1 2 0 4 6 5 sort8" -> "7 6 5 4 3 2 1 0"
            auto at = [&stack](int i) -> Value& { return stack.at(stack.size() - 1 - i); };
            const auto &sn = buildSortNet(op.imm.u);
            for (auto cmp: sn) {
                auto &a = at(cmp.first), &b = at(cmp.second);
                Value min = a.Min(b), max = a.Max(b);
                a = min, b = max;
            }
            break;
        }

        case ExprOpType::MEM_LOAD: {
            Pointer<Byte> p = state.wptrs[op.imm.i + 1];
            const VSFormat *format = ctx.vi[op.imm.i]->format;
            const bool unaligned = op.x != 0;
            Int y = state.y, x = state.x;
            IntV offsets = 0;
            if (op.bc == BoundaryCondition::Clamped) {
                if (op.y != 0)
                    y = Clamp(state.y + op.y, 0, state.height-1);
                if (op.x != 0)
                    x = Clamp(state.x + op.x, 0, state.width-1);
            } else { // Mirrored
                if (op.y != 0) {
                    Int sy = state.y + Clamp(op.y, -state.height, state.height);
                    y = IfThenElse(sy < 0, -1 - sy,
                            IfThenElse(sy >= state.height, 2*state.height-1 - sy, sy));
                }
                if (op.x != 0) {
                    Int cx = Clamp(op.x, -state.width, state.width);
                    Int w2m1 = 2 * state.width - 1;
                    for (int i = 0; i < lanes; i++) {
                        Int sx = x + i + cx;
                        Int xi = IfThenElse(sx < 0, -1 - sx,
                                    IfThenElse(sx >= state.width, w2m1 - sx, sx));
                        offsets = Insert(offsets, xi, i);
                    }
                    offsets = offsets * IntV(format->bytesPerSample);
                    x = 0;
                }
            }
            p += y * state.strides[op.imm.i + 1] + x * format->bytesPerSample;
            const bool regularLoad = op.bc != BoundaryCondition::Mirrored || op.x == 0;
            if (format->sampleType == stInteger) {
                IntV v;
                if (format->bytesPerSample == 1) {
                    if (regularLoad)
                        v = IntV(*Pointer<ByteV>(p, (unaligned ? 1:lanes)*sizeof(uint8_t)));
                    else
                        v = IntV(Gather(Pointer<Byte>(p), offsets, IntV(~0), sizeof(uint8_t)));
                } else if (format->bytesPerSample == 2) {
                    if (regularLoad)
                        v = IntV(*Pointer<UShortV>(p, (unaligned ? 1:lanes)*sizeof(uint16_t)));
                    else
                        v = IntV(Gather(Pointer<UShort>(p), offsets, IntV(~0), sizeof(uint16_t)));
                } else if (format->bytesPerSample == 4) {
                    if (regularLoad)
                        v = IntV(*Pointer<IntV>(p, (unaligned ? 1:lanes)*sizeof(uint32_t)));
                    else
                        v = IntV(Gather(Pointer<Int>(p), offsets, IntV(~0), sizeof(uint16_t)));
                }
                v = relativeAccessAdjust<lanes>(x, state.x, state.width, op, v);
                if (ctx.forceFloat())
                    OUT(FloatV(v));
                else
                    OUT(v);
            } else if (format->sampleType == stFloat) {
                FloatV v;
                if (format->bytesPerSample == 2)
                    abort(); // XXX: f16 not supported
                else if (format->bytesPerSample == 4) {
                    if (regularLoad)
                        v = *Pointer<FloatV>(p, (unaligned ? 1:lanes)*sizeof(float));
                    else
                        v = Gather(Pointer<Float>(p), offsets, IntV(~0), sizeof(float));
                }
                v = relativeAccessAdjust<lanes>(x, state.x, state.width, op, v);
                OUT(v);
            }
            break;
        }
        case ExprOpType::CONSTANTI:
            OUT((int)op.imm.i);
            break;
        case ExprOpType::CONSTANTF:
            if (op.imm.f == (float)(int)op.imm.f)
                OUT((int)op.imm.f);
            else
                OUT(op.imm.f);
            break;
        case ExprOpType::CONST_LOAD: {
            switch (static_cast<LoadConstType>(op.imm.i)) {
            case LoadConstType::N:
                OUT(IntV(Pointer<Int>(state.consts)[static_cast<int>(LoadConstIndex::N)]));
                break;
            case LoadConstType::Y:
                OUT(IntV(state.y));
                break;
            case LoadConstType::X:
                OUT(state.xvec + IntV(state.x));
                break;
            case LoadConstType::Width:
                OUT(IntV(state.width));
                break;
            case LoadConstType::Height:
                OUT(IntV(state.height));
                break;
            default: {
                constexpr int bias = static_cast<int>(LoadConstIndex::LAST) - static_cast<int>(LoadConstType::LAST);
                OUT(FloatV(state.consts[op.imm.i + bias]));
            }
            }
            break;
        }

#define LOAD1(x) \
            Value x = stack.back(); stack.pop_back()
#define LOAD2(l, r) \
            LOAD1(r); \
            LOAD1(l)
#define BINARYOP(op, forceFloat) { \
            LOAD2(l, r); \
            if (l.isFloat() && r.isFloat()) \
                OUT(op(l.f(), r.f())); \
            else if (l.isFloat()) \
                OUT(op(l.f(), FloatV(r.i()))); \
            else if (r.isFloat()) \
                OUT(op(FloatV(l.i()), r.f())); \
            else if (forceFloat) \
                OUT(op(FloatV(l.i()), FloatV(r.i()))); \
            else \
                OUT(op(l.i(), r.i())); \
            break; \
        }
#define BINARYOPF(op) { \
            LOAD2(l, r); \
            OUT(op(l.ensureFloat(), r.ensureFloat())); \
            break; \
        }
#define UNARYOP(op, forceFloat) { \
            LOAD1(x); \
            if (x.isFloat()) \
                OUT(op(x.f())); \
            else if (forceFloat) \
                OUT(op(FloatV(x.i()))); \
            else \
                OUT(op(x.i())); \
            break; \
        }
#define UNARYOPF(op) { \
            LOAD1(x); \
            OUT(op(x.ensureFloat())); \
            break; \
        }
#define LOGICOP(op) { \
            LOAD2(l, r); \
            IntV li = l.isFloat() ? CmpGT(l.f(), FloatV(0.0)) : CmpGT(l.i(), IntV(0)); \
            IntV ri = l.isFloat() ? CmpGT(r.f(), FloatV(0.0)) : CmpGT(r.i(), IntV(0)); \
            auto x = op(li, ri); \
            OUT(x & IntV(1)); \
            break; \
        }

        case ExprOpType::VAR_LOAD:
            OUT(state.variables[op.imm.i]);
            break;
        case ExprOpType::VAR_STORE: {
            LOAD1(x);
            state.variables[op.imm.i] = x;
            break;
        }

        case ExprOpType::ADD: BINARYOP(operator +, false);
        case ExprOpType::SUB: BINARYOP(operator -, false);
        case ExprOpType::MUL: BINARYOP(operator *, false);
        case ExprOpType::DIV: BINARYOP(operator /, true);
        case ExprOpType::MOD: BINARYOP(operator %, true);
        case ExprOpType::SQRT: UNARYOPF([](RValue<FloatV> x) -> FloatV { return Sqrt(Max(x, FloatV(0.0))); });
        case ExprOpType::ABS: UNARYOP(Abs, ctx.forceFloat());
        case ExprOpType::MAX: BINARYOP(Max, ctx.forceFloat());
        case ExprOpType::MIN: BINARYOP(Min, ctx.forceFloat());
        case ExprOpType::CLAMP: {
            LOAD2(min, max);
            LOAD1(x);
            if (x.isFloat() || min.isFloat() || max.isFloat() || ctx.forceFloat()) {
                FloatV xf = x.ensureFloat();
                FloatV minf = min.ensureFloat();
                FloatV maxf = max.ensureFloat();
                OUT(Max(Min(xf, maxf), minf));
            } else
                OUT(Max(Min(x.i(), max.i()), min.i()));
            break;
        }
#define CMP(l, r) \
            switch (static_cast<ComparisonType>(op.imm.u)) { \
            case ComparisonType::EQ:  x = CmpEQ(l, r);  break; \
            case ComparisonType::LT:  x = CmpLT(l, r);  break; \
            case ComparisonType::LE:  x = CmpLE(l, r);  break; \
            case ComparisonType::NEQ: x = CmpNEQ(l, r); break; \
            case ComparisonType::NLT: x = CmpNLT(l, r); break; \
            case ComparisonType::NLE: x = CmpNLE(l, r); break; \
            }
        case ExprOpType::CMP: {
            LOAD2(l, r);
            IntV x;
            if (l.isFloat() || r.isFloat()) {
                FloatV lf = l.ensureFloat();
                FloatV rf = r.ensureFloat();
                CMP(lf, rf);
            } else {
                CMP(l.i(), r.i());
            }
            OUT(x & IntV(1));
            break;
        }
#undef CMP

        case ExprOpType::AND: LOGICOP(operator &);
        case ExprOpType::OR: LOGICOP(operator |);
        case ExprOpType::XOR: LOGICOP(operator ^);
        case ExprOpType::NOT: {
            LOAD1(x);
            IntV xi = x.isFloat() ? CmpLE(x.f(), FloatV(0.0f)) : CmpLE(x.i(), IntV(0));
            OUT(xi & IntV(1));
            break;
        }

        case ExprOpType::TRUNC: UNARYOPF(Trunc);
        case ExprOpType::ROUND: UNARYOPF(Round);
        case ExprOpType::FLOOR: UNARYOPF(Floor);

        case ExprOpType::EXP: UNARYOPF([&helpers](RValue<FloatV> x) -> FloatV { return helpers.Exp->Call(x); });
        case ExprOpType::LOG: UNARYOPF([&helpers](RValue<FloatV> x) -> FloatV { return helpers.Log->Call(x); });
        case ExprOpType::POW: {
            LOAD2(l, r);
            if (!r.isFloat()) {
                OUT(IfThenElse(RValue<IntV>(r.i()).IsConstant(),
                        BuiltinPow(l.ensureFloat(), FloatV(r.i())),
                        helpers.Pow->Call(l.ensureFloat(), r.ensureFloat())));
            } else {
                OUT(helpers.Pow->Call(l.ensureFloat(), r.ensureFloat()));
            }
            break;
        }
        case ExprOpType::SIN: UNARYOPF([&helpers](RValue<FloatV> x) -> FloatV { return helpers.Sin->Call(x); });
        case ExprOpType::COS: UNARYOPF([&helpers](RValue<FloatV> x) -> FloatV { return helpers.Cos->Call(x); });

        case ExprOpType::TERNARY: {
            LOAD2(t, f);
            LOAD1(c);
            auto ci = c.isFloat() ? CmpGT(c.f(), FloatV(0.0f)) : CmpGT(c.i(), IntV(0));
            if (t.isFloat() || f.isFloat()) {
                FloatV tf = t.ensureFloat();
                FloatV ff = f.ensureFloat();
                OUT(As<FloatV>((As<IntV>(tf) & ci) | (As<IntV>(ff) & ~ci)));
            } else
                OUT((t.i() & ci) | (f.i() & ~ci));
            break;
        }
#undef LOGICOP
#undef UNARYOP
#undef BINARYOP
#undef OUT
#undef LOAD2
#undef LOAD1
        } // switch
    }

    if (stack.empty())
        throw std::runtime_error("empty expression: " + ctx.expr);
    if (stack.size() > 1)
        throw std::runtime_error("unconsumed values on stack: " + ctx.expr);

    auto res = stack.back();
    auto format = ctx.vo->format;
    Pointer<Byte> p = state.wptrs[0];
    p += state.y * state.strides[0] + state.x * format->bytesPerSample;
    if (format->sampleType == stInteger) {
        IntV rounded;
        const int maxval = (1<<format->bitsPerSample) - 1;
        if (res.isFloat()) {
            FloatV clamped = Min(Max(res.f(), FloatV(0)), FloatV(maxval));
            rounded = RoundInt(clamped);
        } else if (format->bitsPerSample < 32)
            rounded = Min(Max(res.i(), IntV(0)), IntV(maxval));
        else
            rounded = res.i();
        if (format->bytesPerSample == 1)
            *Pointer<ByteV>(p, lanes*sizeof(uint8_t)) = ByteV(UShortV(rounded));
        else if (format->bytesPerSample == 2)
            *Pointer<UShortV>(p, lanes*sizeof(uint16_t)) = UShortV(rounded);
        else if (format->bytesPerSample == 4)
            *Pointer<IntV>(p, lanes*sizeof(uint32_t)) = rounded;
    } else if (format->sampleType == stFloat) {
        if (format->bytesPerSample == 2) // XXX: f16 not supported.
            abort();
        else if (format->bytesPerSample == 4)
            *Pointer<FloatV>(p, lanes*sizeof(float)) = res.ensureFloat();
    }
}

template<int lanes>
typename Compiler<lanes>::Helper Compiler<lanes>::buildHelpers(rr::Module &mod)
{
    Helper h;
    using ftype = typename Compiler<lanes>::Helper::ftype;
    using ftype2 = typename Compiler<lanes>::Helper::ftype2;
    h.Sin = std::make_unique<ftype>(mod, "vsin");
    h.Sin->setPure();
    {
        FloatV x = h.Sin->template Arg<0>();
        Return(SinCos_(x, true));
    }
    h.Cos = std::make_unique<ftype>(mod, "vcos");
    h.Cos->setPure();
    {
        FloatV x = h.Sin->template Arg<0>();
        Return(SinCos_(x, false));
    }
    h.Exp = std::make_unique<ftype>(mod, "vexp");
    h.Exp->setPure();
    {
        FloatV x = h.Sin->template Arg<0>();
        Return(Exp_(x));
    }
    h.Log = std::make_unique<ftype>(mod, "vlog");
    h.Log->setPure();
    {
        FloatV x = h.Sin->template Arg<0>();
        Return(Log_(x));
    }
    h.Pow = std::make_unique<ftype2>(mod, "vpow");
    h.Pow->setPure();
    {
        FloatV x = h.Pow->template Arg<0>();
        FloatV y = h.Pow->template Arg<1>();
        Return(h.Exp->Call(h.Log->Call(x) * y));
    }

    return h;
}

template<int lanes>
Compiled Compiler<lanes>::compile()
{
    if (ctx.cached) {
        return ctx.getCached();
    }

    using namespace rr;
    Module mod;

    std::map<std::pair<int, std::string>, int> paMap;
    for (size_t i = 0; i < ctx.ops.size(); i++) {
        const std::string &tok = ctx.tokens[i];
        ExprOp &op = ctx.ops[i];

        constexpr int last = static_cast<int>(LoadConstType::LAST);
        if (op.type != ExprOpType::CONST_LOAD || op.imm.i < last) continue;

        int id = op.imm.i - last;
        if (id >= ctx.numInputs)
            throw std::runtime_error("reference to undefined clip: " + tok);

        auto key = std::make_pair(id, op.name);
        auto it = paMap.find(key);
        if (it == paMap.end())
            paMap.insert({key, (int)paMap.size()});
        op.imm.i = last + paMap.at(key);
    }
    std::vector<Compiled::PropAccess> pa(paMap.size());
    for (const auto &item: paMap) {
        pa[item.second] = Compiled::PropAccess{ item.first.first, item.first.second };
    }

    std::map<std::string, int> varMap;
    for (size_t i = 0; i < ctx.ops.size(); i++) {
        const std::string &tok = ctx.tokens[i];
        ExprOp &op = ctx.ops[i];

        if (op.type != ExprOpType::VAR_LOAD && op.type != ExprOpType::VAR_STORE) continue;
        auto it = varMap.find(op.name);
        if (it == varMap.end()) {
            if (op.type == ExprOpType::VAR_LOAD)
                throw std::runtime_error("reference to uninitialized variable: " + tok);
            varMap.insert({ op.name, (int)varMap.size() });
        }
        op.imm.i = varMap.at(op.name);
    }

    Helper helpers = buildHelpers(mod);

    //            void *rwptrs, int strides[], float *props, int width, int height
    ModuleFunction<Void(Pointer<Byte>, Pointer<Byte>, Pointer<Byte>, Int, Int)> function(mod, "procPlane");

    State state;
    pointer rwptrs = function.Arg<0>();
    Pointer<Int> strides = Pointer<Int>(Pointer<Byte>(function.Arg<1>()));
    state.consts = Pointer<Float>(Pointer<Byte>(function.Arg<2>()));
    state.width = function.Arg<3>();
    state.height = function.Arg<4>();

    for (size_t i = 0; i < varMap.size(); i++)
        state.variables.push_back(Value(IntV(0)));

    for (int i = 0; i < lanes; i++)
        state.xvec = Insert(state.xvec, i, i);

    for (int i = 0; i < ctx.numInputs + 1; i++) {
        state.wptrs.push_back(*Pointer<Pointer<Byte>>(rwptrs + sizeof(void *) * i));
        state.strides[i] = strides[i];
    }

    auto &y = state.y, &x = state.x;
    For(y = 0, y < state.height, y++)
    {
        For(x = 0, x < state.width, x+=LANES*UNROLL)
        {
            for (int k = 0; k < UNROLL; k++)
                buildOneIter(helpers, state);
        }
    }
    Return();

    Compiled r { mod.acquire("proc"), pa };
#ifdef USE_EXPR_CACHE
    exprCache.insert({ctx.key(), r});
#endif
    return r;
}


static void VS_CC exprInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    ExprData *d = static_cast<ExprData *>(*instanceData);
    vsapi->setVideoInfo(&d->vi, 1, node);
}

static const VSFrameRef *VS_CC exprGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    ExprData *d = static_cast<ExprData *>(*instanceData);
    int numInputs = d->numInputs;

    if (activationReason == arInitial) {
        for (int i = 0; i < numInputs; i++)
            vsapi->requestFrameFilter(n, d->node[i], frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src[MAX_EXPR_INPUTS] = {};
        for (int i = 0; i < numInputs; i++)
            src[i] = vsapi->getFrameFilter(n, d->node[i], frameCtx);

        const VSFormat *fi = d->vi.format;
        int height = vsapi->getFrameHeight(src[0], 0);
        int width = vsapi->getFrameWidth(src[0], 0);
        int planes[3] = { 0, 1, 2 };
        const VSFrameRef *srcf[3] = { d->plane[0] != poCopy ? nullptr : src[0], d->plane[1] != poCopy ? nullptr : src[0], d->plane[2] != poCopy ? nullptr : src[0] };
        VSFrameRef *dst = vsapi->newVideoFrame2(fi, width, height, srcf, planes, src[0], core);

        const uint8_t *srcp[MAX_EXPR_INPUTS] = {};
        int strides[MAX_EXPR_INPUTS + 1] = {};

        for (int plane = 0; plane < d->vi.format->numPlanes; plane++) {
            if (d->plane[plane] != poProcess)
                continue;

            strides[0] = vsapi->getStride(dst, plane);
            for (int i = 0; i < numInputs; i++) {
                if (d->node[i]) {
                    srcp[i] = vsapi->getReadPtr(src[i], plane);
                    strides[i + 1] = vsapi->getStride(src[i], plane);
                }
            }

            uint8_t *dstp = vsapi->getWritePtr(dst, plane);
            int h = vsapi->getFrameHeight(dst, plane);
            int w = vsapi->getFrameWidth(dst, plane);

            uint8_t *rwptrs[MAX_EXPR_INPUTS + 1] = { dstp };
            for (int i = 0; i < numInputs; i++) {
                rwptrs[i + 1] = const_cast<uint8_t *>(srcp[i]);
            }

            union U {
                int i;
                float f;
                U(int i = 0) : i(i) {}
                U(float f) : f(f) {}
            };
            std::vector<U> consts = { n };
            for (const auto &pa : d->compiled[plane].propAccess) {
                auto m = vsapi->getFramePropsRO(src[pa.clip]);
                int err = 0;
                float val = vsapi->propGetInt(m, pa.name.c_str(), 0, &err);
                if (err == peType)
                    val = vsapi->propGetFloat(m, pa.name.c_str(), 0, &err);
                if (err != 0)
                    val = std::nanf(""); // XXX: should we warn the user?
                consts.push_back(val);
            }

            ExprData::ProcessProc proc = reinterpret_cast<ExprData::ProcessProc>(const_cast<void *>(d->compiled[plane].routine->getEntry()));
            proc(rwptrs, strides, reinterpret_cast<float*>(&consts[0]), w, h);
        }

        for (int i = 0; i < MAX_EXPR_INPUTS; i++) {
            vsapi->freeFrame(src[i]);
        }
        return dst;
    }

    return nullptr;
}

static void VS_CC exprFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    ExprData *d = static_cast<ExprData *>(instanceData);
    for (int i = 0; i < MAX_EXPR_INPUTS; i++)
        vsapi->freeNode(d->node[i]);
    delete d;
}

static void VS_CC exprCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    std::unique_ptr<ExprData> d(new ExprData);
    int err;

#define EXPR_F16C_TEST (false)

    try {
        d->numInputs = vsapi->propNumElements(in, "clips");
        if (d->numInputs > 26)
            throw std::runtime_error("More than 26 input clips provided");

        for (int i = 0; i < d->numInputs; i++) {
            d->node[i] = vsapi->propGetNode(in, "clips", i, &err);
        }

        const VSVideoInfo *vi[MAX_EXPR_INPUTS] = {};
        for (int i = 0; i < d->numInputs; i++) {
            if (d->node[i])
                vi[i] = vsapi->getVideoInfo(d->node[i]);
        }

        for (int i = 0; i < d->numInputs; i++) {
            if (!isConstantFormat(vi[i]))
                throw std::runtime_error("Only clips with constant format and dimensions allowed");
            if (vi[0]->format->numPlanes != vi[i]->format->numPlanes
                || vi[0]->format->subSamplingW != vi[i]->format->subSamplingW
                || vi[0]->format->subSamplingH != vi[i]->format->subSamplingH
                || vi[0]->width != vi[i]->width
                || vi[0]->height != vi[i]->height)
            {
                throw std::runtime_error("All inputs must have the same number of planes and the same dimensions, subsampling included");
            }

            int bits = vi[i]->format->bitsPerSample;
            if (EXPR_F16C_TEST) {
                if (((bits > 32 || (bits > 16 && bits < 32)) && vi[i]->format->sampleType == stInteger)
                    || (bits != 16 && bits != 32 && vi[i]->format->sampleType == stFloat))
                    throw std::runtime_error("Input clips must be 8-16/32 bit integer or 16/32 bit float format");
            } else {
                if (((bits > 32 || (bits > 16 && bits < 32)) && vi[i]->format->sampleType == stInteger)
                    || (bits != 32 && vi[i]->format->sampleType == stFloat))
                    throw std::runtime_error("Input clips must be 8-16/32 bit integer or 32 bit float format");
            }
        }

        d->vi = *vi[0];
        int format = int64ToIntS(vsapi->propGetInt(in, "format", 0, &err));
        if (!err) {
            const VSFormat *f = vsapi->getFormatPreset(format, core);
            if (f) {
                if (d->vi.format->colorFamily == cmCompat)
                    throw std::runtime_error("No compat formats allowed");
                if (d->vi.format->numPlanes != f->numPlanes)
                    throw std::runtime_error("The number of planes in the inputs and output must match");
                d->vi.format = vsapi->registerFormat(d->vi.format->colorFamily, f->sampleType, f->bitsPerSample, d->vi.format->subSamplingW, d->vi.format->subSamplingH, core);
            }
        }

        int nexpr = vsapi->propNumElements(in, "expr");
        if (nexpr > d->vi.format->numPlanes)
            throw std::runtime_error("More expressions given than there are planes");

        std::string expr[3];
        for (int i = 0; i < nexpr; i++) {
            expr[i] = vsapi->propGetData(in, "expr", i, nullptr);
        }
        for (int i = nexpr; i < 3; ++i) {
            expr[i] = expr[nexpr - 1];
        }

        int optMask = int64ToIntS(vsapi->propGetInt(in, "opt", 0, &err));
        if (err) optMask = 0;

        int mirror = int64ToIntS(vsapi->propGetInt(in, "boundary", 0, &err));
        if (err) mirror = 0;

        for (int i = 0; i < d->vi.format->numPlanes; i++) {
            if (!expr[i].empty()) {
                d->plane[i] = poProcess;
            } else {
                if (d->vi.format->bitsPerSample == vi[0]->format->bitsPerSample && d->vi.format->sampleType == vi[0]->format->sampleType)
                    d->plane[i] = poCopy;
                else
                    d->plane[i] = poUndefined;
            }

            if (d->plane[i] != poProcess)
                continue;

            Compiler<LANES> comp(expr[i], &d->vi, vi, d->numInputs, optMask, mirror);
            d->compiled[i] = comp.compile();
            d->proc[i] = reinterpret_cast<ExprData::ProcessProc>(const_cast<void *>(d->compiled[i].routine->getEntry()));
        }
    } catch (std::runtime_error &e) {
        for (int i = 0; i < MAX_EXPR_INPUTS; i++) {
            vsapi->freeNode(d->node[i]);
        }
        vsapi->setError(out, (std::string{ "Expr: " } + e.what()).c_str());
        return;
    }

    vsapi->createFilter(in, out, "Expr", exprInit, exprGetFrame, exprFree, fmParallel, 0, d.release(), core);
}

static void initExpr() {
    auto cfg = rr::Config::Edit()
        .set(rr::Optimization::Level::Aggressive)
        .set(rr::Optimization::FMF::FastMath)
        .clearOptimizationPasses()
        .add(rr::Optimization::Pass::ScalarReplAggregates)
        .add(rr::Optimization::Pass::InstructionCombining)
        .add(rr::Optimization::Pass::Reassociate)
        .add(rr::Optimization::Pass::SCCP)
        .add(rr::Optimization::Pass::GVN)
        .add(rr::Optimization::Pass::LICM)
        .add(rr::Optimization::Pass::CFGSimplification)
        .add(rr::Optimization::Pass::EarlyCSEPass)
        .add(rr::Optimization::Pass::CFGSimplification)
        .add(rr::Optimization::Pass::Inline)
        ;

    rr::Nucleus::adjustDefaultConfig(cfg);
}

void VS_CC versionCreate(const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi)
{
    vsapi->propSetData(out, "version", VERSION, -1, paAppend);
    vsapi->propSetData(out, "expr_backend", "llvm", -1, paAppend);
    for (const auto &f : features)
        vsapi->propSetData(out, "expr_features", f.c_str(), -1, paAppend);
}

} // namespace


//////////////////////////////////////////
// Init

void VS_CC exprInitialize(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    //configFunc("com.vapoursynth.expr", "expr", "VapourSynth Expr Filter", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("Expr", "clips:clip[];expr:data[];format:int:opt;opt:int:opt;boundary:int:opt;", exprCreate, nullptr, plugin);
    registerFunc("Version", "", versionCreate, nullptr, plugin);
    initExpr();
}
