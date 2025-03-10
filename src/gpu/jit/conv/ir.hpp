/*******************************************************************************
* Copyright 2021-2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef GPU_JIT_CONV_IR_HPP
#define GPU_JIT_CONV_IR_HPP

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

#include "common/optional.hpp"
#include "gpu/jit/conv/ir_core.hpp"
namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

class ir_context_t {
public:
    expr_t create_tmp_var(
            const type_t &type, const std::string &prefix = "tmp") {
        int &id = prefix_ids_[prefix];
        auto name = prefix + "_" + std::to_string(id);
        id++;
        return var_t::make(type, name);
    }

private:
    std::unordered_map<std::string, int> prefix_ids_;
};

class alloc_updater_t : public ir_mutator_t {
public:
    void resize(const expr_t &buf, int new_size) {
        auto ret = resizes_.insert({buf, new_size});
        ir_assert(ret.second) << buf;
        MAYBE_UNUSED(ret);
    }

    void add_attr(const expr_t &buf, const alloc_attr_t &attr) {
        auto ret = attrs_.insert({buf, attr});
        ir_assert(ret.second) << buf;
        MAYBE_UNUSED(ret);
    }

    void remove(const expr_t &buf) {
        auto ret = removes_.insert(buf);
        ir_assert(ret.second) << buf;
        MAYBE_UNUSED(ret);
    }

    stmt_t update(const stmt_t &stmt) { return mutate(stmt); }

    object_t _mutate(const alloc_t &obj) override {
        auto new_obj = ir_mutator_t::_mutate(obj);

        // If removal succeeds, stop any further updates.
        if (try_remove(new_obj)) return new_obj;

        // Otherwise try to apply other modifications one by one.
        try_resize(new_obj);
        try_add_attr(new_obj);

        return new_obj;
    }

private:
    bool try_remove(object_t &obj) {
        auto &alloc = obj.as<alloc_t>();
        auto it = removes_.find(alloc.buf);
        if (it == removes_.end()) return false;

        obj = alloc.body;
        removes_.erase(it);
        return true;
    }

    bool try_resize(object_t &obj) {
        auto &alloc = obj.as<alloc_t>();
        auto it = resizes_.find(alloc.buf);
        if (it == resizes_.end()) return false;

        obj = alloc_t::make(
                alloc.buf, it->second, alloc.kind, alloc.attrs, alloc.body);
        resizes_.erase(it);
        return true;
    }

    bool try_add_attr(object_t &obj) {
        auto &alloc = obj.as<alloc_t>();
        auto it = attrs_.find(alloc.buf);
        if (it == attrs_.end()) return false;

        auto new_attrs = alloc.attrs;
        new_attrs.push_back(it->second);

        obj = alloc_t::make(
                alloc.buf, alloc.size, alloc.kind, new_attrs, alloc.body);
        attrs_.erase(it);
        return true;
    }

    object_set_t<expr_t> removes_;
    object_map_t<expr_t, int> resizes_;
    object_map_t<expr_t, alloc_attr_t> attrs_;
};

// Returns a new statement with injected buffer allocations from `allocs`.
// - If put_innermost is false, then `stmt` is nested to all allocations
// - If put_innermost is true, then every allocation is injected as innermost
//   as possible
stmt_t inject_alloc_stmts(const stmt_t &stmt, const std::vector<stmt_t> &allocs,
        bool put_innermost = false);

// Returns a new statement with injected let statements, `stmt` is nested to
// all let statements.
stmt_t inject_let_stmts(const stmt_t &stmt, const std::vector<stmt_t> &lets);

template <typename T>
struct expr_cast_helper_t {
    static T call(const expr_t &e) { return to_cpp<T>(e); }

    static std::vector<T> call(const std::vector<expr_t> &exprs) {
        std::vector<T> ret;
        for (auto &e : exprs)
            ret.push_back(to_cpp<T>(e));
        return ret;
    }
};

template <>
struct expr_cast_helper_t<expr_t> {
    static expr_t call(const expr_t &e) { return e; }

    static std::vector<expr_t> call(const std::vector<expr_t> &exprs) {
        return exprs;
    }

    template <typename U,
            typename
            = typename std::enable_if<std::is_arithmetic<U>::value>::type>
    static std::vector<expr_t> call(const std::vector<U> &vec) {
        std::vector<expr_t> ret;
        for (auto &v : vec)
            ret.push_back(to_expr(v));
        return ret;
    }
};

template <typename DstT, typename SrcT>
DstT expr_cast(const SrcT &src) {
    return expr_cast_helper_t<DstT>::call(src);
}

template <typename DstT, typename SrcT>
std::vector<DstT> expr_cast(const std::vector<SrcT> &src) {
    return expr_cast_helper_t<DstT>::call(src);
}

// Performs constant folding recursively to an IR tree.
object_t const_fold(const object_t &obj);

// Performs constant folding non-recursively to an expression.
expr_t const_fold_non_recursive(const expr_t &e);

template <typename T>
std::vector<object_t> find_objects(const object_t &root);

class alloc_manager_t {
public:
    alloc_manager_t(const stmt_t &root) {
        auto allocs = find_objects<alloc_t>(root);
        for (auto &_a : allocs) {
            auto &a = _a.as<alloc_t>();
            auto ret = buf2alloc_.insert({a.buf, _a});
            buffers_.push_back(a.buf);
            ir_assert(ret.second) << "Buffer already exists: " << a.buf;
            MAYBE_UNUSED(ret);
        }

        // Sort buffers by name.
        std::sort(buffers_.begin(), buffers_.end(),
                [](const expr_t &a, const expr_t &b) {
                    return a.as<var_t>().name < b.as<var_t>().name;
                });
    }

    const std::vector<expr_t> &buffers() const { return buffers_; }

    expr_t find_buffer(
            const std::string &name, bool allow_empty = false) const {
        for (auto &b : buffers())
            if (b.as<var_t>().name == name) return b;

        if (!allow_empty) ir_error_not_expected() << name;
        return expr_t();
    }

    std::vector<expr_t> find_buffers(alloc_kind_t kind) const {
        std::vector<expr_t> ret;
        for (auto &b : buffers())
            if (alloc_kind(b) == kind) ret.push_back(b);
        return ret;
    }

    int alloc_size(const expr_t &buf) const {
        auto *a = find_alloc(buf);
        ir_assert(a) << buf;
        return a->size;
    }

    alloc_kind_t alloc_kind(const expr_t &buf) const {
        auto *a = find_alloc(buf);
        ir_assert(a) << buf;
        return a->kind;
    }

    int total_size(alloc_kind_t kind) const {
        int ret = 0;
        for (auto &kv : buf2alloc_) {
            auto &a = kv.second.as<alloc_t>();
            if (a.kind == kind) ret += a.size;
        }
        return ret;
    }

private:
    const alloc_t *find_alloc(const expr_t &buf) const {
        auto it = buf2alloc_.find(buf);
        if (it == buf2alloc_.end()) return nullptr;
        return it->second.as_ptr<alloc_t>();
    }

    object_map_t<expr_t, stmt_t> buf2alloc_;
    std::vector<expr_t> buffers_;
    object_map_t<expr_t, stmt_t> alloc_updates_;
};

// IR utility functions.
expr_t abs(const expr_t &e);

expr_t cast(const expr_t &e, const type_t &type, bool saturate = false);

bool is_zero(const expr_t &e);

bool is_one(const expr_t &e);

bool is_minus_one(const expr_t &e);

bool is_const_broadcast(const expr_t &e);

bool is_const_broadcast(const expr_t &e, const expr_t &value);

expr_t make_buffer(const std::string &name);

// Utility functions for nary_op_t.
expr_t nary_op_back_transform(const expr_t &e);
expr_t nary_op_canonicalize(const expr_t &_e);
expr_t make_nary_op(op_kind_t op_kind, const std::vector<expr_t> &args);
std::vector<expr_t> cvt_expr_to_nary_op_args(const expr_t &e);

// Substitutes all occurrences of `from` to `to` in `root.
object_t substitute(const object_t &root, const object_t &from,
        const object_t &to,
        int max_substitutions = std::numeric_limits<int>::max());

// Returns leaf statements of `root`. Uses inorder traversal.
std::vector<stmt_t> flatten_statements(const stmt_t &root);

template <typename T, bool find_unique = false, bool save_objects = true>
class object_finder_t : public ir_visitor_t {
public:
    void _visit(const T &obj) override {
        ir_visitor_t::_visit(obj);
        occurrences++;
        if (!save_objects) return;
        if (find_unique) {
            found_unique.insert(obj);
        } else {
            found.push_back(obj);
        }
    }

    std::vector<object_t> found;
    object_set_t<object_t> found_unique;
    int occurrences = 0;
};

// Returns all IR objects of type `T` found in `root`.
template <typename T>
std::vector<object_t> find_objects(const object_t &root) {
    object_finder_t<T, /*find_unique=*/false> finder;
    finder.visit(root);
    return finder.found;
}

template <typename T>
int count_objects(const object_t &root) {
    object_finder_t<T, /*find_unique=*/false, /*save_objects=*/false> finder;
    finder.visit(root);
    return finder.occurrences;
}

// Returns unique IR objects of type `T` found in `root`.
template <typename T>
object_set_t<object_t> find_unique_objects(const object_t &root) {
    object_finder_t<T, /*find_unique=*/true> finder;
    finder.visit(root);
    return finder.found_unique;
}

// Returns number of occurrences of `obj` in `root` (based on identity
// comparison).
int count_object(const object_t &root, const object_t &obj);

// Returns number of occurrences of `obj` in vector of root objects (based on
// identity comparison).
template <typename T>
int count_object(const std::vector<T> &roots, const object_t &obj) {
    int ret = 0;
    for (auto &root : roots)
        ret += count_object(root, obj);
    return ret;
}

// Checks if `root` contains `obj`.
bool contains_object(const object_t &root, const object_t &obj);

// Returns all statement groups matching the label.
std::vector<stmt_t> find_stmt_groups(
        const object_t &root, const stmt_label_t &label);

// Returns a statement group matching the label. `root` must have exactly one
// occurrence.
utils::optional_t<stmt_t> find_stmt_group(
        const object_t &root, const stmt_label_t &label);

// Removes all statement groups matching the label.
object_t remove_stmt_group(const object_t &root, stmt_label_t label);

class scope_visitor_t : public ir_visitor_t {
public:
    bool is_expr_defined(const expr_t &e) const {
        auto vars = find_unique_objects<var_t>(e);
        for (auto &v : vars) {
            if (def_vars_.count(v) == 0) return false;
        }
        return true;
    }

#define CASE(type, var_field, is_pre) \
    if (obj.type_id() == type::_type_id()) { \
        visit_scope((const type &)obj, ((const type &)obj).var_field, is_pre); \
        return; \
    }

    void pre_visit(const object_impl_t &obj) override {
        CASE(alloc_t, buf, true);
        CASE(let_t, var, true);
        CASE(for_t, var, true);
    }

    void post_visit(const object_impl_t &obj) override {
        CASE(alloc_t, buf, false);
        CASE(let_t, var, false);
        CASE(for_t, var, false);
    }

#undef CASE

private:
    template <typename T>
    void visit_scope(const T &obj, const expr_t &var, bool is_pre_visit) {
        if (is_pre_visit) {
            def_vars_.insert(var);
            return;
        }
        def_vars_.erase(var);
    }

    object_set_t<expr_t> def_vars_;
};

class ir_path_t {
public:
    void push(const object_impl_t *obj) { path_.push_back(obj); }

    void pop() { path_.pop_back(); }

    const object_impl_t *back() const {
        ir_assert(!is_empty());
        return path_.back();
    }

    bool is_empty() const { return path_.empty(); }

    void merge(const ir_path_t &other) {
        size_t idx;
        size_t min_size = std::min(path_.size(), other.path_.size());
        for (idx = 0; idx < min_size; idx++) {
            if (path_[idx] != other.path_[idx]) break;
        }
        path_.resize(idx);
    }

private:
    std::vector<const object_impl_t *> path_;
};

// Only for statements that create scope.
stmt_t get_stmt_body(const stmt_t &stmt);

stmt_t replace_stmt_body(const stmt_t &stmt, const stmt_t &new_body);

int get_peak_grf_usage(const stmt_t &stmt, int grf_size, bool skip_let = false);

struct mem_usage_guard_t {
    mem_usage_guard_t(int *mem_usage, int size) : ptr(mem_usage), size(size) {
        *ptr += size;
    }

    ~mem_usage_guard_t() { *ptr -= size; }

    int *ptr;
    int size;
};

// Describes the linear transformation F(x) for variable x: F(x) = (a * x + b),
// where a and b are integer constants.
struct linear_transform_t {
    expr_t x;
    int a;
    int b;

    bool is_identity() const { return a == 1 && b == 0; }
};

// Relation: (lhs op rhs), where:
// - lhs is a variable
// - rhs is an integer constant
// - op is a comparison operation
class relation_t {
public:
    relation_t(const expr_t &expr) : expr_(normalize(expr)) {}

    const expr_t &expr() const { return expr_; }

    const expr_t &var() const { return expr_.as<binary_op_t>().a; }

    const expr_t &rhs() const { return expr_.as<binary_op_t>().b; }

    op_kind_t op_kind() const { return expr_.as<binary_op_t>().op_kind; }

    bool implies(const relation_t &other) const;

    // Applies linear transformation to left and right hand sides of the relation.
    relation_t transform(const linear_transform_t &t, const expr_t &new_var);

    std::string str() const {
        std::ostringstream oss;
        oss << expr_;
        return oss.str();
    }

    static bool is_relation_constraint(const expr_t &e) {
        auto *binary_op = e.as_ptr<binary_op_t>();
        if (!binary_op) return false;
        if (!is_var(binary_op->a)) return false;
        if (!is_const(binary_op->b)) return false;
        if (!is_cmp_op(binary_op->op_kind)) return false;
        return true;
    }

private:
    static expr_t normalize(const expr_t &e);

    expr_t expr_;
};

inline std::ostream &operator<<(std::ostream &out, const relation_t &rel) {
    out << rel.str();
    return out;
}

// Equality for modulus: (var % mod) == 0, where:
// - var is a variable
// - mod is an integer constant
class modulus_info_t {
public:
    modulus_info_t(const expr_t &expr) : expr_(expr) {}

    const expr_t &expr() const { return expr_; }

    const expr_t &var() const {
        auto &mod_expr = expr_.as<binary_op_t>().a;
        return mod_expr.as<binary_op_t>().a;
    }

    const expr_t &mod() const {
        auto &mod_expr = expr_.as<binary_op_t>().a;
        return mod_expr.as<binary_op_t>().b;
    }

    bool implies(const modulus_info_t &other) const {
        ir_assert(var().is_same(other.var()));

        int64_t this_mod = to_cpp<int64_t>(mod());
        int64_t other_mod = to_cpp<int64_t>(other.mod());

        return this_mod % other_mod == 0;
    }

    std::string str() const {
        std::ostringstream oss;
        oss << expr_;
        return oss.str();
    }

    // Try to match (var % mod) == 0.
    static bool is_modulus_constraint(const expr_t &e);

private:
    expr_t expr_;
};

inline std::ostream &operator<<(std::ostream &out, const modulus_info_t &mod) {
    out << mod.str();
    return out;
}

// Helper class to find constant bounds of integer expressions based on known
// relations.
class bound_finder_base_t {
public:
    int64_t find_low_bound(const expr_t &e) const {
        return find_bound_impl(e, /*is_low=*/true);
    }

    int64_t find_high_bound(const expr_t &e) const {
        return find_bound_impl(e, /*is_low=*/false);
    }

    virtual int64_t get_var_bound(const expr_t &e, bool is_low) const = 0;

    static int64_t unlimited_bound(bool is_low) {
        if (is_low) return std::numeric_limits<int64_t>::min();
        return std::numeric_limits<int64_t>::max();
    }

    static bool is_good_bound(int64_t bound) {
        if (bound == unlimited_bound(true)) return false;
        if (bound == unlimited_bound(false)) return false;
        return true;
    }

protected:
    // If is_low is true, searches for proven low bound, and high bound
    // otherwise.
    virtual int64_t find_bound_impl(const expr_t &e, bool is_low) const;
};

class bound_finder_t : public bound_finder_base_t {
public:
    bound_finder_t(
            const object_map_t<expr_t, std::vector<relation_t>> &relations)
        : relations_(relations) {}

    int64_t get_var_bound(const expr_t &e, bool is_low) const override {
        ir_assert(is_var(e));
        int64_t def_bound = unlimited_bound(is_low);
        auto it = relations_.find(e);
        if (it == relations_.end()) return def_bound;

        int64_t ret = def_bound;
        for (auto &rel : it->second) {
            bool is_ge = (rel.op_kind() == op_kind_t::_ge);
            if (is_ge != is_low) continue;
            if (is_ge) {
                ret = std::max(to_cpp<int64_t>(rel.rhs()), ret);
            } else {
                ret = std::min(to_cpp<int64_t>(rel.rhs()), ret);
            }
        }
        return ret;
    }

private:
    object_map_t<expr_t, std::vector<relation_t>> relations_;
};

// TODO: Add integers check (only integers can be constrained).
class constraint_set_t {
public:
    const object_map_t<expr_t, std::vector<relation_t>> &relations() const {
        return relations_;
    }

    void add_constraint(const expr_t &e);

    bool can_prove(const expr_t &e, bool try_simplify = true) const {
        auto ret = can_prove_impl(e, /*do_simplify=*/false);
        if (ret || !try_simplify) return ret;

        return can_prove_impl(e, /*do_simplify=*/true);
    }

    bool is_single_value(const expr_t &e, expr_t &value) const;

    int max_proven_gcd(const expr_t &var) const;

private:
    bool can_prove_modulus(const expr_t &e) const {
        modulus_info_t unknown(e);
        auto it = modulus_infos_.find(unknown.var());
        if (it == modulus_infos_.end()) return false;

        for (auto &known : it->second) {
            if (known.implies(unknown)) return true;
        }

        return false;
    }

    bool can_prove_relation(const expr_t &e) const {
        relation_t unknown(e);
        auto it = relations_.find(unknown.var());
        if (it == relations_.end()) return false;

        for (auto &known : it->second) {
            if (known.implies(unknown)) return true;
        }

        return false;
    }

    bool try_prove_compound_relation(const expr_t &e) const {
        auto *binary = e.as_ptr<binary_op_t>();
        if (!binary) return false;

        auto op_kind = binary->op_kind;
        auto &a = binary->a;
        auto &_b = binary->b;

        if (!is_const(_b)) return false;

        auto b = to_cpp<int64_t>(_b);

        // Normalize operation kind.
        switch (op_kind) {
            case op_kind_t::_ge:
            case op_kind_t::_le: break;
            case op_kind_t::_gt:
                op_kind = op_kind_t::_ge;
                ir_assert(b < std::numeric_limits<int64_t>::max());
                b += 1;
                break;
            case op_kind_t::_lt:
                op_kind = op_kind_t::_le;
                ir_assert(b > std::numeric_limits<int64_t>::min());
                b -= 1;
                break;
            default: return false;
        }

        bound_finder_t finder(relations_);
        if (op_kind == op_kind_t::_ge) {
            auto lo = finder.find_low_bound(a);
            if (!bound_finder_t::is_good_bound(lo)) return false;
            return lo >= b;
        }

        if (op_kind == op_kind_t::_le) {
            auto hi = finder.find_high_bound(a);
            if (!bound_finder_t::is_good_bound(hi)) return false;
            return hi <= b;
        }

        return false;
    }

    bool can_prove_impl(const expr_t &_e, bool do_simplify) const;

    object_map_t<expr_t, std::vector<relation_t>> relations_;
    object_map_t<expr_t, std::vector<modulus_info_t>> modulus_infos_;
};

// Determine the maximum constant factor of an expression, returns 0 in the
// special case that the expression evaluates to 0.
int64_t get_max_const_factor(const expr_t &e, const constraint_set_t &cset);

// Simplifies expression using rewriting rules.
expr_t simplify_rewrite(const expr_t &e);

// Simplifies expression or statement. An optional constraint set is used to
// pass known equalities and inequalities which may be used for simplification.
object_t simplify(const object_t &obj, const constraint_set_t &cset = {});

// Searches for expression patterns to reduce them to the equivalent ternary
// operations.
expr_t simplify_rewrite_with_ternary(const expr_t &e, bool recursive = true);

// Moves constants to the right hand side of an expression.
// Example: (c0 + x) op c1 -> x op (c1 - c0)
expr_t simplify_cmp_move_const_to_rhs(const expr_t &e);

// Reduces left and right hand sides of an expression.
// Example: A * x < A * B -> x < B (if A > 0).
expr_t simplify_cmp_reduce_lhs_rhs(const expr_t &e);

// Propagates shuffle down the expression tree for more effective vectorization.
expr_t simplify_propagate_shuffle(const expr_t &e);

// Pre-defined functions.
namespace funcs {

inline func_t barrier_func() {
    static thread_local auto f = builtin_t::make("barrier");
    return f;
}

inline stmt_t barrier() {
    return barrier_func().call();
}

inline func_t slm_fence_func() {
    static thread_local auto f = builtin_t::make("slm_fence");
    return f;
}

inline stmt_t slm_fence() {
    return slm_fence_func().call();
}

inline func_t signal_func() {
    static thread_local auto f = builtin_t::make("signal");
    return f;
}

inline stmt_t signal() {
    return signal_func().call();
}

inline func_t barrier_wait_func() {
    static thread_local auto f = builtin_t::make("barrier_wait");
    return f;
}

inline stmt_t barrier_wait() {
    return barrier_wait_func().call();
}

inline func_t continue_func() {
    static thread_local auto f = builtin_t::make("continue");
    return f;
}

inline stmt_t _continue() {
    return continue_func().call();
}

inline func_t swsb_long_sync_func() {
    static thread_local auto f = builtin_t::make("swsb_long_sync");
    return f;
}

inline stmt_t swsb_long_sync() {
    return swsb_long_sync_func().call();
}

} // namespace funcs

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
