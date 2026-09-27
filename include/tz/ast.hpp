// The AST database is the source of program truth (§3.1, D7).
//
// node are data aggregates owned by ast_db. The links between node are indexes
// (node_id), not pointers or names in the text: Each change increases the revision
// - on this is reactive elaboration (§10.1).
//
#pragma once

#include "base.hpp"

#include <variant>

namespace tz {

struct node_tag;
using node_id = id<node_tag>;
using node_list = std::vector<node_id>;

// Parameter mode (§6.2 steak).
enum class param_mode : u8 { let, mut, own, out, expr, ct };

// --- node ---

struct n_module   { symbol name; node_list items; };
struct n_fn       { symbol name; node_list params; node_id result_type; node_id body; bool outline = false; bool expr_body = false; };
struct n_param    { symbol name; node_id type; param_mode mode = param_mode::let; bool pack = false; };
struct n_var      { symbol name; node_id type; node_id init; bool is_const = false; };
struct n_block    { node_list items; bool has_value = false; }; // has_value: Last expression without `;`
struct n_ident    { symbol name; };
struct n_int      { i64 value = 0; };
struct n_float    { f64 value = 0; };
struct n_str      { std::string value; };
struct n_call     { node_id callee; node_list args; };
struct n_op       { symbol op; node_list args; };        // operator = operator entity call (§4.4)
struct n_member   { node_id object; symbol name; };
struct n_index    { node_id object; node_id index; };
struct n_if       { node_id cond; node_id then_; node_id else_; };
struct n_for      { symbol var; node_id range; node_id body; };
struct n_while    { node_id cond; node_id body; };
struct n_return   { node_id value; };
struct n_require  { node_id cond; };
struct n_try_compile { node_id body; };                   // SFINAE block (§5.4)
struct n_ct       { node_id body; };                      // ct e / ct { }
struct n_lambda   { node_list params; node_id body; };
struct n_construct{ node_id type; node_list items; };     // array{1, 2, 3}
struct n_auto     { };                                    // `auto` in type position
struct n_hole     { std::string text; std::string reason; }; // undisassembled fragment (§4.1)

using node_kind = std::variant<n_module, n_fn, n_param, n_var, n_block, n_ident, n_int, n_float, n_str,
                               n_call, n_op, n_member, n_index, n_if, n_for, n_while, n_return,
                               n_require, n_try_compile, n_ct, n_lambda, n_construct, n_auto, n_hole>;

struct node {
    node_kind kind;
    span      where;
    node_id   parent;
};

class ast_db {
public:
    node_id add(node_kind k, span where) {
        ++revision_;
        return nodes_.add(node{std::move(k), where, {}});
    }

    template <class kind>
    node_id add(kind k, span where) {
        return add(node_kind(std::move(k)), where);
    }

    node&       operator[](node_id n)       { return nodes_[n]; }
    const node& operator[](node_id n) const { return nodes_[n]; }

    template <class kind> bool        is(node_id n) const { return n && std::holds_alternative<kind>(nodes_[n].kind); }
    template <class kind> kind&       as(node_id n)       { return std::get<kind>(nodes_[n].kind); }
    template <class kind> const kind& as(node_id n) const { return std::get<kind>(nodes_[n].kind); }

    void replace(node_id n, node_kind k) {
        ++revision_;
        nodes_[n].kind = std::move(k);
    }

    // Assign a parent to all descendants - bypass through reflection of node fields.
    void link_parents(node_id root) {
        for_each_child(root, [&](node_id child) {
            nodes_[child].parent = root;
            link_parents(child);
        });
    }

    // All daughter node_id node: One realization for all species through reflection.
    template <class fn>
    void for_each_child(node_id n, fn&& f) const {
        std::visit([&](const auto& k) { for_each_child_of(k, f); }, nodes_[n].kind);
    }

    u64 revision() const { return revision_; }
    u32 size() const { return nodes_.size(); }

    interner strings;

private:
    template <class kind, class fn>
    static void for_each_child_of(const kind& k, fn& f) {
        template for (constexpr auto m : fields_of<kind>()) {
            using field_t = [:std::meta::type_of(m):];
            if constexpr (std::is_same_v<field_t, node_id>) {
                if (k.[:m:]) f(k.[:m:]);
            } else if constexpr (std::is_same_v<field_t, node_list>) {
                for (node_id c : k.[:m:]) if (c) f(c);
            }
        }
    }

    arena<node, node_tag> nodes_;
    u64                   revision_ = 0;
};

// The name node is for debugging (through reflection, without manual tables).
inline std::string_view kind_name(const node& n) {
    return std::visit([](const auto& k) -> std::string_view {
        return std::meta::identifier_of(std::meta::remove_cvref(^^decltype(k)));
    }, n.kind);
}

} // namespace tz
