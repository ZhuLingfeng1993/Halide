#ifndef HALIDE_IR_MATCH_H
#define HALIDE_IR_MATCH_H

/** \file
 * Defines a method to match a fragment of IR against a pattern containing wildcards
 */

#include "IR.h"
#include "IREquality.h"
#include "IROperator.h"
#include "ModulusRemainder.h"

namespace Halide {
namespace Internal {

/** Does the first expression have the same structure as the second?
 * Variables in the first expression with the name * are interpreted
 * as wildcards, and their matching equivalent in the second
 * expression is placed in the vector give as the third argument.
 * Wildcards require the types to match. For the type bits and width,
 * a 0 indicates "match anything". So an Int(8, 0) will match 8-bit
 * integer vectors of any width (including scalars), and a UInt(0, 0)
 * will match any unsigned integer type.
 *
 * For example:
 \code
 Expr x = Variable::make(Int(32), "*");
 match(x + x, 3 + (2*k), result)
 \endcode
 * should return true, and set result[0] to 3 and
 * result[1] to 2*k.
 */
bool expr_match(Expr pattern, Expr expr, std::vector<Expr> &result);

/** Does the first expression have the same structure as the second?
 * Variables are matched consistently. The first time a variable is
 * matched, it assumes the value of the matching part of the second
 * expression. Subsequent matches must be equal to the first match.
 *
 * For example:
 \code
 Var x("x"), y("y");
 match(x*(x + y), a*(a + b), result)
 \endcode
 * should return true, and set result["x"] = a, and result["y"] = b.
 */
bool expr_match(Expr pattern, Expr expr, std::map<std::string, Expr> &result);

void expr_match_test();

/** An alternative template-metaprogramming approach to expression
 * matching. Potentially more efficient. We lift the expression
 * pattern into a type, and then use force-inlined functions to
 * generate efficient matching and reconstruction code for any
 * pattern. Pattern elements are either one of the classes in the
 * namespace IRMatcher, or are non-null Exprs (represented as
 * BaseExprNode &).
 *
 * Pattern elements that are fully specified by their pattern can be
 * built into an expression using the ::make method. Some patterns,
 * such as a broadcast that matches any number of lanes, don't have
 * enough information to recreate an Expr.
 */
namespace IRMatcher {

constexpr int max_wild = 5;

/** To save stack space, the matcher objects are largely stateless and
 * immutable. This state object is built up during matching and then
 * consumed when constructing a replacement Expr.
 */
struct MatcherState {
    const BaseExprNode *bindings[max_wild];
    halide_scalar_value_t bound_const[max_wild];

    // values of the lanes field with special meaning.
    static constexpr uint16_t signed_integer_overflow = 0x8000;
    static constexpr uint16_t indeterminate_expression = 0x4000;
    static constexpr uint16_t special_values_mask = 0xc000;

    halide_type_t bound_const_type[max_wild];

    HALIDE_ALWAYS_INLINE
    void set_binding(int i, const BaseExprNode &n) noexcept {
        bindings[i] = &n;
    }

    HALIDE_ALWAYS_INLINE
    const BaseExprNode *get_binding(int i) const noexcept {
        return bindings[i];
    }

    HALIDE_ALWAYS_INLINE
    void set_bound_const(int i, int64_t s, halide_type_t t) noexcept {
        bound_const[i].u.i64 = s;
        bound_const_type[i] = t;
    }

    HALIDE_ALWAYS_INLINE
    void set_bound_const(int i, uint64_t u, halide_type_t t) noexcept {
        bound_const[i].u.u64 = u;
        bound_const_type[i] = t;
    }

    HALIDE_ALWAYS_INLINE
    void set_bound_const(int i, double f, halide_type_t t) noexcept {
        bound_const[i].u.f64 = f;
        bound_const_type[i] = t;
    }

    HALIDE_ALWAYS_INLINE
    void set_bound_const(int i, halide_scalar_value_t val, halide_type_t t) noexcept {
        bound_const[i] = val;
        bound_const_type[i] = t;
    }

    HALIDE_ALWAYS_INLINE
    void get_bound_const(int i, halide_scalar_value_t &val, halide_type_t &type) const noexcept {
        val = bound_const[i];
        type = bound_const_type[i];
    }

    HALIDE_ALWAYS_INLINE
    MatcherState() noexcept {}

    HALIDE_ALWAYS_INLINE
    void reset() noexcept {
        // TODO: delete me
    }
};

template<typename T,
         typename = typename std::remove_reference<T>::type::pattern_tag>
struct enable_if_pattern {
    struct type {};
};

template<typename T>
struct bindings {
    constexpr static uint32_t mask = std::remove_reference<T>::type::binds;
};

template<>
struct bindings<const BaseExprNode &> {
    constexpr static uint32_t mask = 0;
};

template<typename Pattern,
         typename = typename enable_if_pattern<Pattern>::type>
HALIDE_ALWAYS_INLINE
Expr to_expr(Pattern &&p, MatcherState & __restrict__ state) {
    return p.make(state);
}

HALIDE_ALWAYS_INLINE
Expr to_expr(const BaseExprNode &e, MatcherState & __restrict__ state) {
    return Expr(&e);
}

inline HALIDE_NEVER_INLINE
Expr to_special_expr(halide_type_t ty) {
    const uint16_t flags = ty.lanes & MatcherState::special_values_mask;
    ty.lanes &= ~MatcherState::special_values_mask;
    static std::atomic<int> counter;
    if (flags & MatcherState::indeterminate_expression) {
        return Call::make(ty, Call::indeterminate_expression, {counter++}, Call::Intrinsic);
    } else if (flags & MatcherState::signed_integer_overflow) {
        return Call::make(ty, Call::signed_integer_overflow, {counter++}, Call::Intrinsic);
    }
    // unreachable
    return Expr();
}

HALIDE_ALWAYS_INLINE
Expr to_expr(halide_scalar_value_t val, halide_type_t ty, MatcherState & __restrict__ state) {
    halide_type_t scalar_type = ty;
    if (scalar_type.lanes & MatcherState::special_values_mask) {
        return to_special_expr(scalar_type);
    }

    const int lanes = scalar_type.lanes;
    scalar_type.lanes = 1;

    Expr e;
    switch (scalar_type.code) {
    case halide_type_int:
        e = IntImm::make(scalar_type, val.u.i64);
        break;
    case halide_type_uint:
        e = UIntImm::make(scalar_type, val.u.u64);
        break;
    case halide_type_float:
        e = FloatImm::make(scalar_type, val.u.f64);
        break;
    default:
        // Unreachable
        return Expr();
    }
    if (lanes > 1) {
        e = Broadcast::make(e, lanes);
    }
    return e;
}

inline std::ostream &operator<<(std::ostream &s, const BaseExprNode &n) {
    s << Expr(&n);
    return s;
}

bool equal_helper(const BaseExprNode &a, const BaseExprNode &b) noexcept;

// A fast version of expression equality that assumes a well-typed non-null expression tree.
HALIDE_ALWAYS_INLINE
bool equal(const BaseExprNode &a, const BaseExprNode &b) noexcept {
    // Early out
    return (&a == &b) ||
        ((a.type == b.type) &&
         (a.node_type == b.node_type) &&
         equal_helper(a, b));
}

template<int i>
struct WildConstInt {
    struct pattern_tag {};

    constexpr static uint32_t binds = 1 << i;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        static_assert(i >= 0 && i < max_wild, "Wild with out-of-range index");
        const BaseExprNode *op = &e;
        if (op->node_type == IRNodeType::Broadcast) {
            op = ((const Broadcast *)op)->value.get();
        }
        if (op->node_type != IRNodeType::IntImm) {
            return false;
        }
        int64_t value = ((const IntImm *)op)->value;
        if (bound & binds) {
            halide_scalar_value_t val;
            halide_type_t type;
            state.get_bound_const(i, val, type);
            return op->type == type && value == val.u.i64;
        }
        state.set_bound_const(i, value, e.type);
        return true;
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        halide_scalar_value_t val;
        halide_type_t type;
        state.get_bound_const(i, val, type);
        return to_expr(val, type, state);
    }

    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const {
        state.get_bound_const(i, val, ty);
    }
};

template<int i>
std::ostream &operator<<(std::ostream &s, const WildConstInt<i> &c) {
    s << "ci" << i;
    return s;
}

template<int i>
struct WildConstUInt {
    struct pattern_tag {};

    constexpr static uint32_t binds = 1 << i;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        static_assert(i >= 0 && i < max_wild, "Wild with out-of-range index");
        const BaseExprNode *op = &e;
        if (op->node_type == IRNodeType::Broadcast) {
            op = ((const Broadcast *)op)->value.get();
        }
        if (op->node_type != IRNodeType::UIntImm) {
            return false;
        }
        uint64_t value = ((const UIntImm *)op)->value;
        if (bound & binds) {
            halide_scalar_value_t val;
            halide_type_t type;
            state.get_bound_const(i, val, type);
            return op->type == type && value == val.u.u64;
        }
        state.set_bound_const(i, value, e.type);
        return true;
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        halide_scalar_value_t val;
        halide_type_t type;
        state.get_bound_const(i, val, type);
        return to_expr(val, type, state);
    }

    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const noexcept {
        state.get_bound_const(i, val, ty);
    }
};

template<int i>
std::ostream &operator<<(std::ostream &s, const WildConstUInt<i> &c) {
    s << "cu" << i;
    return s;
}

template<int i>
struct WildConstFloat {
    struct pattern_tag {};

    constexpr static uint32_t binds = 1 << i;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        static_assert(i >= 0 && i < max_wild, "Wild with out-of-range index");
        halide_type_t ty = e.type;
        const BaseExprNode *op = &e;
        if (op->node_type == IRNodeType::Broadcast) {
            op = ((const Broadcast *)op)->value.get();
        }
        if (op->node_type != IRNodeType::FloatImm) {
            return false;
        }
        double value = ((const FloatImm *)op)->value;
        if (bound & binds) {
            halide_scalar_value_t val;
            halide_type_t type;
            state.get_bound_const(i, val, type);
            return op->type == type && value == val.u.f64;
        }
        state.set_bound_const(i, value, ty);
        return true;
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        halide_scalar_value_t val;
        halide_type_t type;
        state.get_bound_const(i, val, type);
        return to_expr(val, type, state);
    }

    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const noexcept {
        state.get_bound_const(i, val, ty);
    }
};

template<int i>
std::ostream &operator<<(std::ostream &s, const WildConstFloat<i> &c) {
    s << "cf" << i;
    return s;
}

// Matches and binds to any constant Expr. Does not support constant-folding.
template<int i>
struct WildConst {
    struct pattern_tag {};

    constexpr static uint32_t binds = 1 << i;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        static_assert(i >= 0 && i < max_wild, "Wild with out-of-range index");
        const BaseExprNode *op = &e;
        if (op->node_type == IRNodeType::Broadcast) {
            op = ((const Broadcast *)op)->value.get();
        }
        switch (op->node_type) {
        case IRNodeType::IntImm:
            return WildConstInt<i>().template match<bound>(e, state);
        case IRNodeType::UIntImm:
            return WildConstUInt<i>().template match<bound>(e, state);
        case IRNodeType::FloatImm:
            return WildConstFloat<i>().template match<bound>(e, state);
        default:
            return false;
        }
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        halide_scalar_value_t val;
        halide_type_t type;
        state.get_bound_const(i, val, type);
        return to_expr(val, type, state);
    }

    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const noexcept {
        state.get_bound_const(i, val, ty);
    }
};

template<int i>
std::ostream &operator<<(std::ostream &s, const WildConst<i> &c) {
    s << "c" << i;
    return s;
}

// Matches and binds to any Expr
template<int i>
struct Wild {
    struct pattern_tag {};

    constexpr static uint32_t binds = 1 << (i + 16);

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        if (bound & binds) {
            return equal(*state.get_binding(i), e);
        }
        state.set_binding(i, e);
        return true;
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        return state.get_binding(i);
    }
};

template<int i>
std::ostream &operator<<(std::ostream &s, const Wild<i> &op) {
    s << "_" << i;
    return s;
}

// Matches a specific constant or broadcast of that constant. The
// constant must be representable as an int.
struct Const {
    struct pattern_tag {};
    int val;

    constexpr static uint32_t binds = 0;

    HALIDE_ALWAYS_INLINE
    Const(int v) : val(v) {}

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        const BaseExprNode *op = &e;
        if (e.node_type == IRNodeType::Broadcast) {
            op = ((const Broadcast *)op)->value.get();
        }
        switch (op->node_type) {
        case IRNodeType::IntImm:
            return ((const IntImm *)op)->value == (int64_t)val;
        case IRNodeType::UIntImm:
            return ((const UIntImm *)op)->value == (uint64_t)val;
        case IRNodeType::FloatImm:
            return ((const FloatImm *)op)->value == (double)val;
        default:
            return false;
        }
    }

    HALIDE_ALWAYS_INLINE
    bool match(const Const &b, MatcherState & __restrict__ state) const noexcept {
        return val == b.val;
    }
};

inline std::ostream &operator<<(std::ostream &s, const Const &op) {
    s << op.val;
    return s;
}

template<typename Op>
int64_t constant_fold_bin_op(halide_type_t &, int64_t, int64_t) noexcept;

template<typename Op>
uint64_t constant_fold_bin_op(halide_type_t &, uint64_t, uint64_t) noexcept;

template<typename Op>
double constant_fold_bin_op(halide_type_t &, double, double) noexcept;

template<typename Op, typename A, typename B>
struct BinOpFolder {
    HALIDE_ALWAYS_INLINE
    static void make_folded_const(const A &a, const B &b, halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) noexcept {
        halide_scalar_value_t val_a, val_b;
        halide_type_t type_a, type_b;
        a.make_folded_const(val_a, type_a, state);
        if ((std::is_same<Op, And>::value && val_a.u.u64 == 0) ||
            (std::is_same<Op, Or>::value && val_a.u.u64 == 1)) {
            // Short circuit
            ty = type_a;
            val = val_a;
            return;
        }
        b.make_folded_const(val_b, type_b, state);
        // The types are known to match except possibly for overflow flags in the lanes field
        ty = type_a;
        ty.lanes |= type_b.lanes;
        switch (type_a.code) {
        case halide_type_int:
            val.u.i64 = constant_fold_bin_op<Op>(ty, val_a.u.i64, val_b.u.i64);
            break;
        case halide_type_uint:
            val.u.u64 = constant_fold_bin_op<Op>(ty, val_a.u.u64, val_b.u.u64);
            break;
        case halide_type_float:
            val.u.f64 = constant_fold_bin_op<Op>(ty, val_a.u.f64, val_b.u.f64);
            break;
        default:
            // unreachable
            ;
        }
    }

    HALIDE_ALWAYS_INLINE
    static Expr make(const A &a, const B &b, MatcherState & __restrict__ state) noexcept {
        Expr ea = to_expr(a, state), eb = to_expr(b, state);
        // We sometimes mix vectors and scalars in the rewrite rules,
        // so insert a broadcast if necessary.
        if (ea.type().is_vector() && !eb.type().is_vector()) {
            eb = Broadcast::make(eb, ea.type().lanes());
        }
        if (eb.type().is_vector() && !ea.type().is_vector()) {
            ea = Broadcast::make(ea, eb.type().lanes());
        }
        return Op::make(std::move(ea), std::move(eb));
    }
};

template<typename Op, typename A>
struct BinOpFolder<Op, A, Const> {
    HALIDE_ALWAYS_INLINE
    static void make_folded_const(const A &a, Const b, halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) noexcept {
        a.make_folded_const(val, ty, state);
        switch (ty.code) {
        case halide_type_int:
            val.u.i64 = constant_fold_bin_op<Op>(ty, val.u.i64, (int64_t)b.val);
            break;
        case halide_type_uint:
            val.u.u64 = constant_fold_bin_op<Op>(ty, val.u.u64, (uint64_t)b.val);
            break;
        case halide_type_float:
            val.u.f64 = constant_fold_bin_op<Op>(ty, val.u.f64, (double)b.val);
            break;
        default:
            // unreachable
            ;
        }
    }

    HALIDE_ALWAYS_INLINE
    static Expr make(const A &a, Const b, MatcherState & __restrict__ state) {
        Expr ea = to_expr(a, state);
        Expr eb = make_const(ea.type(), b.val);
        return Op::make(std::move(ea), std::move(eb));
    }
};

template<typename Op, typename B>
struct BinOpFolder<Op, Const, B> {
    HALIDE_ALWAYS_INLINE
    static void make_folded_const(Const a, const B &b, halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) noexcept {
        b.make_folded_const(val, ty, state);
        switch (ty.code) {
        case halide_type_int:
            val.u.i64 = constant_fold_bin_op<Op>(ty, (int64_t)a.val, val.u.i64);
            break;
        case halide_type_uint:
            val.u.u64 = constant_fold_bin_op<Op>(ty, (uint64_t)a.val, val.u.u64);
            break;
        case halide_type_float:
            val.u.f64 = constant_fold_bin_op<Op>(ty, (double)a.val, val.u.f64);
            break;
        default:
            // unreachable
            ;
        }
    }

    HALIDE_ALWAYS_INLINE
    static Expr make(Const a, const B &b, MatcherState & __restrict__ state) {
        Expr eb = to_expr(b, state);
        Expr ea = make_const(eb.type(), a.val);
        return Op::make(std::move(ea), std::move(eb));
    }
};


// Matches one of the binary operators
template<typename Op, typename A, typename B>
struct BinOp {
    struct pattern_tag {};
    A a;
    B b;

    constexpr static uint32_t binds = bindings<A>::mask | bindings<B>::mask;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        if (e.node_type != Op::_node_type) {
            return false;
        }
        const Op &op = (const Op &)e;
        return (a.template match<bound>(*op.a.get(), state) &&
                b.template match<bound | bindings<A>::mask>(*op.b.get(), state));
    }

    template<uint32_t bound, typename Op2, typename A2, typename B2>
    HALIDE_ALWAYS_INLINE
    bool match(const BinOp<Op2, A2, B2> &op, MatcherState & __restrict__ state) const noexcept {
        return (std::is_same<Op, Op2>::value &&
                a.template match<bound>(op.a, state) &&
                b.template match<bound | bindings<A>::mask>(op.b, state));
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        return BinOpFolder<Op, A, B>::make(a, b, state);
    }


    template<typename A1 = A, typename B1 = B>
    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const {
        BinOpFolder<Op, A1, B1>::make_folded_const(a, b, val, ty, state);
    }
};

template<typename Op>
uint64_t constant_fold_cmp_op(int64_t, int64_t) noexcept;

template<typename Op>
uint64_t constant_fold_cmp_op(uint64_t, uint64_t) noexcept;

template<typename Op>
uint64_t constant_fold_cmp_op(double, double) noexcept;

template<typename Op, typename A, typename B>
struct CmpOpFolder {
    HALIDE_ALWAYS_INLINE
    static void make_folded_const(const A &a, const B &b, halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) noexcept {
        halide_scalar_value_t val_a, val_b;
        halide_type_t type_a, type_b;
        a.make_folded_const(val_a, type_a, state);
        b.make_folded_const(val_b, type_b, state);
        ty.code = halide_type_uint;
        ty.bits = 1;
        ty.lanes = type_a.lanes | type_b.lanes;
        switch (type_a.code) {
        case halide_type_int:
            val.u.u64 = constant_fold_cmp_op<Op>(val_a.u.i64, val_b.u.i64);
            break;
        case halide_type_uint:
            val.u.u64 = constant_fold_cmp_op<Op>(val_a.u.u64, val_b.u.u64);
            break;
        case halide_type_float:
            val.u.u64 = constant_fold_cmp_op<Op>(val_a.u.f64, val_b.u.f64);
            break;
        default:
            // unreachable
            ;
        }
    }

    HALIDE_ALWAYS_INLINE
    static Expr make(const A &a, const B &b, MatcherState & __restrict__ state) {
        Expr ea = to_expr(a, state), eb = to_expr(b, state);
        // We sometimes mix vectors and scalars in the rewrite rules,
        // so insert a broadcast if necessary.
        if (ea.type().is_vector() && !eb.type().is_vector()) {
            eb = Broadcast::make(eb, ea.type().lanes());
        }
        if (eb.type().is_vector() && !ea.type().is_vector()) {
            ea = Broadcast::make(ea, eb.type().lanes());
        }
        return Op::make(std::move(ea), std::move(eb));
    }
};

template<typename Op, typename B>
struct CmpOpFolder<Op, Const, B> {
    HALIDE_ALWAYS_INLINE
    static void make_folded_const(Const a, const B &b, halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) noexcept {
        b.make_folded_const(val, ty, state);
        switch (ty.code) {
        case halide_type_int:
            val.u.u64 = constant_fold_cmp_op<Op>((int64_t)a.val, val.u.i64);
            break;
        case halide_type_uint:
            val.u.u64 = constant_fold_cmp_op<Op>((uint64_t)a.val, val.u.u64);
            break;
        case halide_type_float:
            val.u.u64 = constant_fold_cmp_op<Op>((double)a.val, val.u.f64);
            break;
        default:
            // unreachable
            ;
        }
        ty.bits = 1;
        ty.code = halide_type_uint;
    }

    HALIDE_ALWAYS_INLINE
    static Expr make(Const a, const B &b, MatcherState & __restrict__ state) {
        Expr eb = to_expr(b, state);
        Expr ea = make_const(eb.type(), a.val);
        return Op::make(std::move(ea), std::move(eb));
    }
};

template<typename Op, typename A>
struct CmpOpFolder<Op, A, Const> {
    HALIDE_ALWAYS_INLINE
    static void make_folded_const(const A &a, Const b, halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) noexcept {
        a.make_folded_const(val, ty, state);
        switch (ty.code) {
        case halide_type_int:
            val.u.u64 = constant_fold_cmp_op<Op>(val.u.i64, (int64_t)b.val);
            break;
        case halide_type_uint:
            val.u.u64 = constant_fold_cmp_op<Op>(val.u.u64, (uint64_t)b.val);
            break;
        case halide_type_float:
            val.u.u64 = constant_fold_cmp_op<Op>(val.u.f64, (double)b.val);
            break;
        default:
            // unreachable
            ;
        }
        ty.bits = 1;
        ty.code = halide_type_uint;
    }

    HALIDE_ALWAYS_INLINE
    static Expr make(const A &a, Const b, MatcherState & __restrict__ state) {
        Expr ea = to_expr(a, state);
        Expr eb = make_const(ea.type(), b.val);
        return Op::make(std::move(ea), std::move(eb));
    }
};

// Matches one of the comparison operators
template<typename Op, typename A, typename B>
struct CmpOp {
    struct pattern_tag {};
    A a;
    B b;

    constexpr static uint32_t binds = bindings<A>::mask | bindings<B>::mask;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        if (e.node_type != Op::_node_type) {
            return false;
        }
        const Op &op = (const Op &)e;
        return (a.template match<bound>(*op.a.get(), state) &&
                b.template match<bound | bindings<A>::mask>(*op.b.get(), state));
    }

    template<uint32_t bound, typename Op2, typename A2, typename B2>
    HALIDE_ALWAYS_INLINE
    bool match(const CmpOp<Op2, A2, B2> &op, MatcherState & __restrict__ state) const noexcept {
        return (std::is_same<Op, Op2>::value &&
                a.template match<bound>(op.a, state) &&
                b.template match<bound | bindings<A>::mask>(op.b, state));
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        return CmpOpFolder<Op, A, B>::make(a, b, state);
    }

    template<typename A1 = A,
             typename B1 = B>
    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const noexcept {
        CmpOpFolder<Op, A, B>::make_folded_const(a, b, val, ty, state);
    }
};

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const BinOp<Add, A, B> &op) {
    s << "(" << op.a << " + " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const BinOp<Sub, A, B> &op) {
    s << "(" << op.a << " - " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const BinOp<Mul, A, B> &op) {
    s << "(" << op.a << " * " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const BinOp<Div, A, B> &op) {
    s << "(" << op.a << " / " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const BinOp<And, A, B> &op) {
    s << "(" << op.a << " && " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const BinOp<Or, A, B> &op) {
    s << "(" << op.a << " || " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const BinOp<Min, A, B> &op) {
    s << "min(" << op.a << ", " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const BinOp<Max, A, B> &op) {
    s << "max(" << op.a << ", " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const CmpOp<LE, A, B> &op) {
    s << "(" << op.a << " <= " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const CmpOp<LT, A, B> &op) {
    s << "(" << op.a << " < " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const CmpOp<GE, A, B> &op) {
    s << "(" << op.a << " >= " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const CmpOp<GT, A, B> &op) {
    s << "(" << op.a << " > " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const CmpOp<EQ, A, B> &op) {
    s << "(" << op.a << " == " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const CmpOp<NE, A, B> &op) {
    s << "(" << op.a << " != " << op.b << ")";
    return s;
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const BinOp<Mod, A, B> &op) {
    s << "(" << op.a << " % " << op.b << ")";
    return s;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Add, A, B> operator+(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<Add, A, Const> operator+(A a, int b) noexcept {
    return {a, Const(b)};
}

template<typename B,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Add, Const, B> operator+(int a, B b) noexcept {
    return {Const(a), b};
}

HALIDE_ALWAYS_INLINE
BinOp<Add, const BaseExprNode &, const BaseExprNode &> add(const Expr &a, const Expr &b) noexcept {
    return {*a.get(), *b.get()};
}

template<>
HALIDE_ALWAYS_INLINE
int64_t constant_fold_bin_op<Add>(halide_type_t &t, int64_t a, int64_t b) noexcept {
    t.lanes |= ((t.bits >= 32) && add_would_overflow(t.bits, a, b)) ? MatcherState::signed_integer_overflow : 0;
    int dead_bits = 64 - t.bits;
    // Drop the high bits then sign-extend them back
    return ((a + b) << dead_bits) >> dead_bits;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_bin_op<Add>(halide_type_t &t, uint64_t a, uint64_t b) noexcept {
    uint64_t ones = (uint64_t)(-1);
    return (a + b) & (ones >> (64 - t.bits));
}

template<>
HALIDE_ALWAYS_INLINE
double constant_fold_bin_op<Add>(halide_type_t &t, double a, double b) noexcept {
    return a + b;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Sub, A, B> operator-(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<Sub, A, Const> operator-(A a, int b) noexcept {
    return {a, Const(b)};
}

template<typename B,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Sub, Const, B> operator-(int a, B b) noexcept {
    return {Const(a), b};
}

HALIDE_ALWAYS_INLINE
BinOp<Sub, const BaseExprNode &, const BaseExprNode &> sub(const Expr &a, const Expr &b) noexcept {
    return {*a.get(), *b.get()};
}

template<>
HALIDE_ALWAYS_INLINE
int64_t constant_fold_bin_op<Sub>(halide_type_t &t, int64_t a, int64_t b) noexcept {
    t.lanes |= ((t.bits >= 32) && sub_would_overflow(t.bits, a, b)) ? MatcherState::signed_integer_overflow : 0;
    int dead_bits = 64 - t.bits;
    // Drop the high bits then sign-extend them back
    return ((a - b) << dead_bits) >> dead_bits;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_bin_op<Sub>(halide_type_t &t, uint64_t a, uint64_t b) noexcept {
    uint64_t ones = (uint64_t)(-1);
    return (a - b) & (ones >> (64 - t.bits));
}

template<>
HALIDE_ALWAYS_INLINE
double constant_fold_bin_op<Sub>(halide_type_t &t, double a, double b) noexcept {
    return a - b;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Mul, A, B> operator*(A a, B b) noexcept {
    return {a, b};
}


template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<Mul, A, Const> operator*(A a, int b) noexcept {
    return {a, Const(b)};
}

template<typename B,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Mul, Const, B> operator*(int a, B b) noexcept {
    return {Const(a), b};
}

HALIDE_ALWAYS_INLINE
BinOp<Mul, const BaseExprNode &, const BaseExprNode &> mul(const Expr &a, const Expr &b) noexcept {
    return {*a.get(), *b.get()};
}

template<>
HALIDE_ALWAYS_INLINE
int64_t constant_fold_bin_op<Mul>(halide_type_t &t, int64_t a, int64_t b) noexcept {
    t.lanes |= ((t.bits >= 32) && mul_would_overflow(t.bits, a, b)) ? MatcherState::signed_integer_overflow : 0;
    int dead_bits = 64 - t.bits;
    // Drop the high bits then sign-extend them back
    return ((a * b) << dead_bits) >> dead_bits;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_bin_op<Mul>(halide_type_t &t, uint64_t a, uint64_t b) noexcept {
    uint64_t ones = (uint64_t)(-1);
    return (a * b) & (ones >> (64 - t.bits));
}

template<>
HALIDE_ALWAYS_INLINE
double constant_fold_bin_op<Mul>(halide_type_t &t, double a, double b) noexcept {
    return a * b;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Div, A, B> operator/(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<Div, A, Const> operator/(A a, int b) noexcept {
    return {a, Const(b)};
}

template<typename B,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Div, Const, B> operator/(int a, B b) noexcept {
    return {Const(a), b};
}

HALIDE_ALWAYS_INLINE
BinOp<Div, const BaseExprNode &, const BaseExprNode &> div(const Expr &a, const Expr &b) noexcept {
    return {*a.get(), *b.get()};
}

template<>
HALIDE_ALWAYS_INLINE
int64_t constant_fold_bin_op<Div>(halide_type_t &t, int64_t a, int64_t b) noexcept {
    if (b == 0) {
        t.lanes |= MatcherState::indeterminate_expression;
        return 0;
    } else {
        return div_imp(a, b);
    }
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_bin_op<Div>(halide_type_t &t, uint64_t a, uint64_t b) noexcept {
    if (b == 0) {
        t.lanes |= MatcherState::indeterminate_expression;
        return 0;
    } else {
        return a / b;
    }
}

template<>
HALIDE_ALWAYS_INLINE
double constant_fold_bin_op<Div>(halide_type_t &t, double a, double b) noexcept {
    return a / b;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Mod, A, B> operator%(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<Mod, A, Const> operator%(A a, int b) noexcept {
    return {a, Const(b)};
}

template<typename B,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Mod, Const, B> operator%(int a, B b) noexcept {
    return {Const(a), b};
}

HALIDE_ALWAYS_INLINE
BinOp<Mod, const BaseExprNode &, const BaseExprNode &> mod(const Expr &a, const Expr &b) noexcept {
    return {*a.get(), *b.get()};
}

template<>
HALIDE_ALWAYS_INLINE
int64_t constant_fold_bin_op<Mod>(halide_type_t &t, int64_t a, int64_t b) noexcept {
    if (b == 0) {
        t.lanes |= MatcherState::indeterminate_expression;
        return 0;
    } else {
        return mod_imp(a, b);
    }
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_bin_op<Mod>(halide_type_t &t, uint64_t a, uint64_t b) noexcept {
    if (b == 0) {
        t.lanes |= MatcherState::indeterminate_expression;
        return 0;
    } else {
        return a % b;
    }
}

template<>
HALIDE_ALWAYS_INLINE
double constant_fold_bin_op<Mod>(halide_type_t &t, double a, double b) noexcept {
    return mod_imp(a, b);
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Min, A, B> min(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<Min, A, Const> min(A a, int b) noexcept {
    return {a, Const(b)};
}

template<typename B,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Min, Const, B> min(int a, B b) noexcept {
    return {Const(a), b};
}

template<>
HALIDE_ALWAYS_INLINE
int64_t constant_fold_bin_op<Min>(halide_type_t &t, int64_t a, int64_t b) noexcept {
    return std::min(a, b);
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_bin_op<Min>(halide_type_t &t, uint64_t a, uint64_t b) noexcept {
    return std::min(a, b);
}

template<>
HALIDE_ALWAYS_INLINE
double constant_fold_bin_op<Min>(halide_type_t &t, double a, double b) noexcept {
    return std::min(a, b);
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Max, A, B> max(A a, B b) noexcept {
    return {a, b};
}


template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<Max, A, Const> max(A a, int b) noexcept {
    return {a, Const(b)};
}

template<typename B,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Max, Const, B> max(int a, B b) noexcept {
    return {Const(a), b};
}

template<>
HALIDE_ALWAYS_INLINE
int64_t constant_fold_bin_op<Max>(halide_type_t &t, int64_t a, int64_t b) noexcept {
    return std::max(a, b);
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_bin_op<Max>(halide_type_t &t, uint64_t a, uint64_t b) noexcept {
    return std::max(a, b);
}

template<>
HALIDE_ALWAYS_INLINE
double constant_fold_bin_op<Max>(halide_type_t &t, double a, double b) noexcept {
    return std::max(a, b);
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
CmpOp<LT, A, B> operator<(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
CmpOp<LT, A, Const> operator<(A a, int b) noexcept {
    return {a, b};
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<LT>(int64_t a, int64_t b) noexcept {
    return a < b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<LT>(uint64_t a, uint64_t b) noexcept {
    return a < b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<LT>(double a, double b) noexcept {
    return a < b;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
CmpOp<GT, A, B> operator>(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
CmpOp<GT, A, Const> operator>(A a, int b) noexcept {
    return {a, Const(b)};
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<GT>(int64_t a, int64_t b) noexcept {
    return a > b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<GT>(uint64_t a, uint64_t b) noexcept {
    return a > b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<GT>(double a, double b) noexcept {
    return a > b;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
CmpOp<LE, A, B> operator<=(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
CmpOp<LE, A, Const> operator<=(A a, int b) noexcept {
    return {a, Const(b)};
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<LE>(int64_t a, int64_t b) noexcept {
    return a <= b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<LE>(uint64_t a, uint64_t b) noexcept {
    return a <= b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<LE>(double a, double b) noexcept {
    return a <= b;
}


template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
CmpOp<GE, A, B> operator>=(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
CmpOp<GE, A, Const> operator>=(A a, int b) noexcept {
    return {a, Const(b)};
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<GE>(int64_t a, int64_t b) noexcept {
    return a >= b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<GE>(uint64_t a, uint64_t b) noexcept {
    return a >= b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<GE>(double a, double b) noexcept {
    return a >= b;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
CmpOp<EQ, A, B> operator==(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
CmpOp<EQ, A, Const> operator==(A a, int b) noexcept {
    return {a, Const(b)};
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<EQ>(int64_t a, int64_t b) noexcept {
    return a == b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<EQ>(uint64_t a, uint64_t b) noexcept {
    return a == b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<EQ>(double a, double b) noexcept {
    return a == b;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
CmpOp<NE, A, B> operator!=(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
CmpOp<NE, A, Const> operator!=(A a, int b) noexcept {
    return {a, Const(b)};
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<NE>(int64_t a, int64_t b) noexcept {
    return a != b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<NE>(uint64_t a, uint64_t b) noexcept {
    return a != b;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_cmp_op<NE>(double a, double b) noexcept {
    return a != b;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<Or, A, B> operator||(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<Or, A, Const> operator||(A a, int b) noexcept {
    return {a, Const(b)};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<Or, A, Const> operator||(int b, A a) noexcept {
    return {a, Const(b)};
}

template<>
HALIDE_ALWAYS_INLINE
int64_t constant_fold_bin_op<Or>(halide_type_t &t, int64_t a, int64_t b) noexcept {
    return 0;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_bin_op<Or>(halide_type_t &t, uint64_t a, uint64_t b) noexcept {
    return a | b;
}

template<>
HALIDE_ALWAYS_INLINE
double constant_fold_bin_op<Or>(halide_type_t &t, double a, double b) noexcept {
    return 0;
}

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
BinOp<And, A, B> operator&&(A a, B b) noexcept {
    return {a, b};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<And, A, Const> operator&&(A a, int b) noexcept {
    return {a, Const(b)};
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BinOp<And, A, Const> operator&&(int b, A a) noexcept {
    return {a, Const(b)};
}

template<>
HALIDE_ALWAYS_INLINE
int64_t constant_fold_bin_op<And>(halide_type_t &t, int64_t a, int64_t b) noexcept {
    return 0;
}

template<>
HALIDE_ALWAYS_INLINE
uint64_t constant_fold_bin_op<And>(halide_type_t &t, uint64_t a, uint64_t b) noexcept {
    return a & b;
}

template<>
HALIDE_ALWAYS_INLINE
double constant_fold_bin_op<And>(halide_type_t &t, double a, double b) noexcept {
    return 0;
}

constexpr inline uint32_t bitwise_or_reduce() {
    return 0;
}

template<typename... Args>
constexpr uint32_t bitwise_or_reduce(uint32_t first, Args... rest) {
    return first | bitwise_or_reduce(rest...);
}

template<typename... Args>
struct Intrin {
    struct pattern_tag {};
    Call::ConstString intrin;
    std::tuple<Args...> args;

    static constexpr uint32_t binds = bitwise_or_reduce((bindings<Args>::mask)...);

    template<int i,
             uint32_t bound,
             typename = typename std::enable_if<(i < sizeof...(Args))>::type>
    HALIDE_ALWAYS_INLINE
    bool match_args(int, const Call &c, MatcherState & __restrict__ state) const noexcept {
        using T = decltype(std::get<i>(args));
        return (std::get<i>(args).template match<bound>(*c.args[i].get(), state) &&
                match_args<i + 1, bound | bindings<T>::mask>(0, c, state));
    }

    template<int i, uint32_t binds>
    HALIDE_ALWAYS_INLINE
    bool match_args(double, const Call &c, MatcherState & __restrict__ state) const noexcept {
        return true;
    }

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        if (e.node_type != IRNodeType::Call) {
            return false;
        }
        const Call &c = (const Call &)e;
        return (c.is_intrinsic(intrin) && match_args<0, bound>(0, c, state));
    }

    template<int i,
             typename = typename std::enable_if<(i < sizeof...(Args))>::type>
    HALIDE_ALWAYS_INLINE
    void print_args(int, std::ostream &s) const {
        s << std::get<i>(args);
        if (i + 1 < sizeof...(Args)) {
            s << ", ";
        }
        print_args<i+1>(0, s);
    }

    template<int i>
    HALIDE_ALWAYS_INLINE
    void print_args(double, std::ostream &s) const {
    }

    HALIDE_ALWAYS_INLINE
    void print_args(std::ostream &s) const {
        print_args<0>(0, s);
    }

    HALIDE_ALWAYS_INLINE
    Intrin(Call::ConstString intrin, Args... args) noexcept : intrin(intrin), args(args...) {}
};

template<typename... Args>
inline std::ostream &operator<<(std::ostream &s, const Intrin<Args...> &op) {
    s << op.intrin << "(";
    op.print_args(s);
    s << ")";
    return s;
}

template<typename... Args>
HALIDE_ALWAYS_INLINE
Intrin<Args...> intrin(Call::ConstString name, Args&&... args) noexcept {
    return Intrin<Args...>(name, std::forward<Args>(args)...);
}

template<typename A>
struct NotOp {
    struct pattern_tag {};
    A a;

    constexpr static uint32_t binds = bindings<A>::mask;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        const Not &op = (const Not &)e;
        return (e.node_type == IRNodeType::Not &&
                a.template match<bound>(*op.a.get(), state));
    }

    template<uint32_t bound, typename A2>
    HALIDE_ALWAYS_INLINE
    bool match(const NotOp<A2> &op, MatcherState & __restrict__ state) const noexcept {
        return a.template match<bound>(op.a, state);
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        return Not::make(to_expr(a, state));
    }

    template<typename A1 = A>
    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const noexcept {
        a.make_folded_const(val, ty, state);
        val.u.u64 = (val.u.u64 == 0) ? 1 : 0;
    }
};

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
NotOp<A> operator!(A a) noexcept {
    return {a};
}

template<typename A>
inline std::ostream &operator<<(std::ostream &s, const NotOp<A> &op) {
    s << "!(" << op.a << ")";
    return s;
}

template<typename C, typename T, typename F>
struct SelectOp {
    struct pattern_tag {};
    C c;
    T t;
    F f;

    constexpr static uint32_t binds = bindings<C>::mask | bindings<T>::mask | bindings<F>::mask;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        const Select &op = (const Select &)e;
        return (e.node_type == Select::_node_type &&
                c.template match<bound>(*op.condition.get(), state) &&
                t.template match<bound | bindings<C>::mask>(*op.true_value.get(), state) &&
                f.template match<bound | bindings<C>::mask | bindings<T>::mask>(*op.false_value.get(), state));
    }
    template<uint32_t bound, typename C2, typename T2, typename F2>
    HALIDE_ALWAYS_INLINE
    bool match(const SelectOp<C2, T2, F2> &instance, MatcherState & __restrict__ state) const noexcept {
        return (c.template match<bound>(instance.c, state) &&
                t.template match<bound | bindings<C>::mask>(instance.t, state) &&
                f.template match<bound | bindings<C>::mask | bindings<T>::mask>(instance.f, state));
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        return Select::make(to_expr(c, state), to_expr(t, state), to_expr(f, state));
    }
};

template<typename C, typename T, typename F>
std::ostream &operator<<(std::ostream &s, const SelectOp<C, T, F> &op) {
    s << "select(" << op.c << ", " << op.t << ", " << op.f << ")";
    return s;
}

template<typename C,
         typename T,
         typename F,
         typename = typename enable_if_pattern<C>::type,
         typename = typename enable_if_pattern<T>::type,
         typename = typename enable_if_pattern<F>::type>
HALIDE_ALWAYS_INLINE
SelectOp<C, T, F> select(C c, T t, F f) noexcept {
    return {c, t, f};
}

HALIDE_ALWAYS_INLINE
SelectOp<const BaseExprNode &, const BaseExprNode &, const BaseExprNode &> select(const Expr &c, const Expr &t, const Expr &f) noexcept {
    return {*c.get(), *t.get(), *f.get()};
}

template<typename A>
struct BroadcastOp {
    struct pattern_tag {};
    A a;
    int lanes;

    constexpr static uint32_t binds = bindings<A>::mask;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        if (e.node_type == Broadcast::_node_type) {
            const Broadcast &op = (const Broadcast &)e;
            if ((lanes == -1 || lanes == op.lanes) &&
                a.template match<bound>(*op.value.get(), state)) {
                return true;
            }
        }
        return false;
    }

    template<uint32_t bound, typename A2>
    HALIDE_ALWAYS_INLINE
    bool match(const BroadcastOp<A2> &op, MatcherState & __restrict__ state) const noexcept {
        return (a.template match<bound>(op.a, state) &&
                (lanes == op.lanes || lanes == -1 || op.lanes == -1));
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        return Broadcast::make(to_expr(a, state), lanes);
    }

};

template<typename A>
inline std::ostream &operator<<(std::ostream &s, const BroadcastOp<A> &op) {
    s << "broadcast(" << op.a << ")";
    return s;
}

template<typename A>
HALIDE_ALWAYS_INLINE
BroadcastOp<A> broadcast(A &&a, int lanes = -1) noexcept { // -1 => matches any number of lanes
    return BroadcastOp<A>{std::forward<A>(a), lanes};
}

template<typename A, typename B>
struct RampOp {
    struct pattern_tag {};
    A a;
    B b;
    int lanes;

    constexpr static uint32_t binds = bindings<A>::mask | bindings<B>::mask;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        const Ramp &op = (const Ramp &)e;
        if (op.node_type == Ramp::_node_type &&
            a.template match<bound>(*op.base.get(), state) &&
            b.template match<bound | bindings<A>::mask>(*op.stride.get(), state)) {
            return true;
        } else {
            return false;
        }
    }

    template<uint32_t bound, typename A2, typename B2>
    HALIDE_ALWAYS_INLINE
    bool match(const RampOp<A2, B2> &op, MatcherState & __restrict__ state) const noexcept {
        return (a.template match<bound>(op.a, state) &&
                b.template match<bound | bindings<A>::mask>(op.b, state));
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        return Ramp::make(to_expr(a, state), to_expr(b, state), lanes);
    }
};

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const RampOp<A, B> &op) {
    s << "ramp(" << op.a << ", " << op.b << ")";
    return s;
}

template<typename A, typename B>
HALIDE_ALWAYS_INLINE
RampOp<A, B> ramp(A a, B b, int lanes = -1) noexcept {
    return {a, b, lanes};
}

template<typename A>
struct NegateOp {
    struct pattern_tag {};
    A a;

    constexpr static uint32_t binds = bindings<A>::mask;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        const Sub &op = (const Sub &)e;
        return (op.node_type == Sub::_node_type &&
                a.template match<bound>(*op.b.get(), state) &&
                is_zero(op.a));
    }

    template<uint32_t bound, typename A2>
    HALIDE_ALWAYS_INLINE
    bool match(NegateOp<A2> &&p, MatcherState & __restrict__ state) const noexcept {
        return a.template match<bound>(p.a, state);
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        Expr ea = to_expr(a, state);
        Expr z = make_zero(ea.type());
        return Sub::make(std::move(z), std::move(ea));
    }

    template<typename A1 = A>
    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const noexcept {
        a.make_folded_const(val, ty, state);
        int dead_bits = 64 - ty.bits;
        switch (ty.code) {
        case halide_type_int:
            if (ty.bits >= 32 && val.u.i64 && !(val.u.i64 << (65 - ty.bits))) {
                // Trying to negate the most negative signed int for a no-overflow type.
                ty.lanes |= MatcherState::signed_integer_overflow;
            } else {
                // Negate, drop the high bits, and then sign-extend them back
                val.u.i64 = ((-val.u.i64) << dead_bits) >> dead_bits;
            }
            break;
        case halide_type_uint:
            val.u.u64 = ((-val.u.u64) << dead_bits) >> dead_bits;
            break;
        case halide_type_float:
            val.u.f64 = -val.u.f64;
            break;
        default:
            // unreachable
            ;
        }
    }
};

template<typename A>
std::ostream &operator<<(std::ostream &s, const NegateOp<A> &op) {
    s << "-" << op.a;
    return s;
}

template<typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
NegateOp<A> operator-(A a) noexcept {
    return {a};
}

template<typename A>
struct IsConstOp {
    struct pattern_tag {};

    constexpr static uint32_t binds = bindings<A>::mask;

    A a;
    template<typename A1 = A>
    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const noexcept {
        Expr e = a.make(state);
        ty.code = halide_type_uint;
        ty.bits = 64;
        ty.lanes = 1;
        val.u.u64 = is_const(e) ? 1 : 0;
    }
};

template<typename A,
         typename = typename enable_if_pattern<A>::type>
IsConstOp<A> is_const(A a) noexcept {
    return {a};
}

template<typename A>
std::ostream &operator<<(std::ostream &s, const IsConstOp<A> &op) {
    s << "is_const(" << op.a << ")";
    return s;
}

template<typename A>
struct CastOp {
    struct pattern_tag {};
    Type type;
    A a;

    constexpr static uint32_t binds = bindings<A>::mask;

    template<uint32_t bound>
    HALIDE_ALWAYS_INLINE
    bool match(const BaseExprNode &e, MatcherState & __restrict__ state) const noexcept {
        const Cast &op = (const Cast &)e;
        return (op.node_type == Cast::_node_type &&
                a.template match<bound>(*op.value.get(), state));
    }
    template<uint32_t bound, typename A2>
    HALIDE_ALWAYS_INLINE
    bool match(const CastOp<A2> &op, MatcherState & __restrict__ state) const noexcept {
        return a.template match<bound>(op.a, state);
    }

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const {
        return cast(type, to_expr(a, state));
    }
};

template<typename A>
std::ostream &operator<<(std::ostream &s, const CastOp<A> &op) {
    s << "cast(" << op.type << ", " << op.a << ")";
    return s;
}

template<typename A>
HALIDE_ALWAYS_INLINE
CastOp<A> cast(Type t, A &&a) noexcept {
    return CastOp<A>{t, std::forward<A>(a)};
}

template<typename A>
struct FoldOp {
    struct pattern_tag {};
    A a;

    constexpr static uint32_t binds = bindings<A>::mask;

    HALIDE_ALWAYS_INLINE
    Expr make(MatcherState & __restrict__ state) const noexcept {
        halide_scalar_value_t c;
        halide_type_t ty;
        a.make_folded_const(c, ty, state);
        return to_expr(c, ty, state);
    }
};

template<typename A>
HALIDE_ALWAYS_INLINE
FoldOp<A> fold(A a) noexcept {
    return {a};
}

template<typename A>
std::ostream &operator<<(std::ostream &s, const FoldOp<A> &op) {
    s << "fold(" << op.a << ")";
    return s;
}

template<typename A, typename Prover>
struct CanProveOp {
    struct pattern_tag {};
    A a;
    Prover *prover;  // An existing simplifying mutator

    constexpr static uint32_t binds = bindings<A>::mask;

    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const {
        Expr condition = a.make(state);
        condition = prover->mutate(condition);
        val.u.u64 = is_one(condition);
        ty.code = halide_type_uint;
        ty.bits = 1;
        ty.lanes = condition.type().lanes();
    };
};

template<typename A,
         typename Prover,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
CanProveOp<A, Prover> can_prove(A a, Prover *s) noexcept {
    return {a, s};
}

template<typename A, typename Prover>
std::ostream &operator<<(std::ostream &s, const CanProveOp<A, Prover> &op) {
    s << "can_prove(" << op.a << ")";
    return s;
}

template<typename A, typename B>
struct GCDOp {
    struct pattern_tag {};
    A a;
    B b;

    constexpr static uint32_t binds = bindings<A>::mask | bindings<B>::mask;

    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const noexcept {
        halide_scalar_value_t val_a, val_b;
        halide_type_t type_a, type_b;
        a.make_folded_const(val_a, type_a, state);
        b.make_folded_const(val_b, type_b, state);
        ty = type_a;
        ty.lanes |= type_b.lanes;
        internal_assert(ty.code == halide_type_int && ty.bits >= 32);
        val.u.i64 = Halide::Internal::gcd(val_a.u.i64, val_b.u.i64);
    };
};

template<typename A,
         typename B,
         typename = typename enable_if_pattern<A>::type,
         typename = typename enable_if_pattern<B>::type>
HALIDE_ALWAYS_INLINE
GCDOp<A, B> gcd(A a, B b) noexcept {
    return {a, b};
}

template<typename A, typename B>
std::ostream &operator<<(std::ostream &s, const GCDOp<A, B> &op) {
    s << "gcd(" << op.a << ", " << op.b << ")";
    return s;
}

template<int i, typename A>
struct BindOp {
    struct pattern_tag {};
    A a;

    constexpr static uint32_t binds = bindings<A>::mask | (1 << i);

    HALIDE_ALWAYS_INLINE
    void make_folded_const(halide_scalar_value_t &val, halide_type_t &ty, MatcherState & __restrict__ state) const noexcept {
        a.make_folded_const(val, ty, state);
        state.set_bound_const(i, val, ty);
        // The bind node evaluates to true
        val.u.u64 = 1;
        ty.code = halide_type_uint;
        ty.bits = 1;
        ty.lanes = 1;
    };
};

template<int i,
         typename A,
         typename = typename enable_if_pattern<A>::type>
HALIDE_ALWAYS_INLINE
BindOp<i, A> bind(WildConst<i> c, A a) noexcept {
    return {a};
}

template<int i, typename A>
std::ostream &operator<<(std::ostream &s, const BindOp<i, A> &op) {
    s << "bind(_" << i << " = " << op.a << ")";
    return s;
}

// Statically verify properties of each rewrite rule
template<typename Before, typename After>
HALIDE_ALWAYS_INLINE
void validate_rule() noexcept {
    // TODO
}

HALIDE_ALWAYS_INLINE
bool evaluate_predicate(bool x, MatcherState & __restrict__ ) noexcept {
    return x;
}

template<typename Pattern,
         typename = typename enable_if_pattern<Pattern>::type>
HALIDE_ALWAYS_INLINE
bool evaluate_predicate(Pattern &&p, MatcherState & __restrict__ state) {
    halide_scalar_value_t c;
    halide_type_t ty;
    p.make_folded_const(c, ty, state);
    return (c.u.u64 != 0) && ((ty.lanes & MatcherState::special_values_mask) == 0);
}

template<typename Instance>
struct Rewriter {
    Instance instance;
    Expr result;
    MatcherState state;

    HALIDE_ALWAYS_INLINE
    Rewriter(Instance &&instance) : instance(std::forward<Instance>(instance)) {}

    template<typename Before,
             typename After,
             typename = typename enable_if_pattern<Before>::type,
             typename = typename enable_if_pattern<After>::type>
    HALIDE_ALWAYS_INLINE
    bool operator()(Before &&before, After &&after) {
        state.reset();
        if (before.template match<0>(instance, state)) {
            result = after.make(state);
            // debug(0) << instance << " -> " << result << " via " << before << " -> " << after << "\n";
            return true;
        } else {
            // debug(0) << "No match: " << instance << " vs " << before << "\n";
            return false;
        }
    }

    template<typename Before,
             typename = typename enable_if_pattern<Before>::type>
    HALIDE_ALWAYS_INLINE
    bool operator()(Before &&before, const Expr &after) noexcept {
        state.reset();
        if (before.template match<0>(instance, state)) {
            result = after;
            // debug(0) << instance << " -> " << result << " via " << before << " -> " << after << "\n";
            return true;
        } else {
            // debug(0) << "No match: " << instance << " vs " << before << "\n";
            return false;
        }
    }

    template<typename Before,
             typename = typename enable_if_pattern<Before>::type>
    HALIDE_ALWAYS_INLINE
    bool operator()(Before &&before, Expr &&after) noexcept {
        state.reset();
        if (before.template match<0>(instance, state)) {
            result = std::move(after);
            // debug(0) << instance << " -> " << result << " via " << before << "\n";
            return true;
        } else {
            // debug(0) << "No match: " << instance << " vs " << before << "\n";
            return false;
        }
    }

    template<typename Before,
             typename After,
             typename Predicate,
             typename = typename enable_if_pattern<Before>::type,
             typename = typename enable_if_pattern<After>::type,
             typename = typename enable_if_pattern<Predicate>::type>
    HALIDE_ALWAYS_INLINE
    bool operator()(Before &&before, After &&after, Predicate &&pred) {
        state.reset();
        if (before.template match<0>(instance, state) &&
            evaluate_predicate(std::forward<Predicate>(pred), state)) {
            result = after.make(state);
            // debug(0) << instance << " -> " << result << " via " << before << " -> " << after << " when " << pred << "\n";
            return true;
        } else {
            // debug(0) << "No match: " << instance << " vs " << before << "\n";
            return false;
        }
    }

    template<typename Before,
             typename Predicate,
             typename = typename enable_if_pattern<Before>::type,
             typename = typename enable_if_pattern<Predicate>::type>
    HALIDE_ALWAYS_INLINE
    bool operator()(Before &&before, const Expr &after, Predicate &&pred) {
        state.reset();
        if (before.template match<0>(instance, state) &&
            evaluate_predicate(std::forward<Predicate>(pred), state)) {
            result = after;
            // debug(0) << instance << " -> " << result << " via " << before << " -> " << after << " when " << pred << "\n";
            return true;
        } else {
            // debug(0) << "No match: " << instance << " vs " << before << "\n";
            return false;
        }
    }

    template<typename Before,
             typename Predicate,
             typename = typename enable_if_pattern<Before>::type,
             typename = typename enable_if_pattern<Predicate>::type>
    HALIDE_ALWAYS_INLINE
    bool operator()(Before &&before, Expr &&after, Predicate &&pred) {
        state.reset();
        if (before.template match<0>(instance, state) &&
            evaluate_predicate(pred, state)) {
            result = std::move(after);
            // debug(0) << instance << " -> " << result << " via " << before << " when " << pred << "\n";
            return true;
        } else {
            // debug(0) << "No match: " << instance << " vs " << before << "\n";
            return false;
        }
    }
};

template<typename Instance>
HALIDE_ALWAYS_INLINE
Rewriter<Instance> rewriter(Instance &&instance) noexcept {
    return Rewriter<Instance>(std::forward<Instance>(instance));
}

}

}
}

#endif
