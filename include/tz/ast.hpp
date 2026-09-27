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
struct n_return   { node_id value; node_id target; };   // return(f) x is the output from the external function f; return(caller) x is the output from the calling function (§6.1)
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

// journal of base operations (§3.1):
// 
// The base state is the reproduction of journal;
// journal is the history of change.
struct j_intern  { u32 id; std::string text; };
struct j_add     { u32 id; node_kind kind; span where; };
struct j_replace { u32 id; node_kind kind; };
struct j_link    { u32 root; }; // Put the parent in the subtree (deterministically)
struct j_import  { std::string text; std::string label; }; // projection: span node - position in it
using journal_op = std::variant<j_intern, j_add, j_replace, j_link, j_import>;

struct journal_entry {
    u64        revision;
    journal_op op;
};

class ast_db {
public:
    node_id add(node_kind k, span where) {
        ++revision_;
        where.source = u32(sources_.empty() ? 0 : sources_.size() - 1);
        node_id id   = nodes_.add(node{std::move(k), where, {}});
        node_rev_.push_back(revision_);
        log({j_add{id.value(), nodes_[id].kind, where}});
        return id;
    }

    // node revision - when it was last changed (for reactive disability).
    u64 node_revision(node_id n) const { return node_rev_[n.value()]; }

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
        nodes_[n].kind        = std::move(k);
        node_rev_[n.value()]  = revision_;
        log({j_replace{n.value(), nodes_[n].kind}});
    }

    // Assign a parent to all descendants - bypass through reflection of node fields.
    void link_parents(node_id root) {
        ++revision_;
        link_rec(root);
        log({j_link{root.value()}});
    }

    // Text input - operation on the base (§3.1): the text is stored in journal, the
    // positions node (span) indicate it.
    u32 record_import(std::string text, std::string label = {}) {
        sources_.push_back(text);
        labels_.push_back(label);
        log({j_import{std::move(text), std::move(label)}});
        return u32(sources_.size() - 1);
    }

    // Import tag for diagnostics (empty - main file).
    const std::string& source_label(u32 i) const {
        static const std::string empty;
        return i < labels_.size() ? labels_[i] : empty;
    }

    const std::string& source(u32 i = 0) const {
        static const std::string empty;
        return i < sources_.size() ? sources_[i] : empty;
    }

    // journal with new strings before each operation.
    std::vector<journal_entry> journal() const {
        std::vector<journal_entry> out;
        u32                        emitted = 0;
        for (const auto& [strings_count, e] : log_) {
            for (; emitted < strings_count; ++emitted)
                out.push_back({e.revision, j_intern{emitted, std::string(strings.text(symbol(emitted)))}});
            out.push_back(e);
        }
        for (; emitted < strings.size(); ++emitted)
            out.push_back({revision_, j_intern{emitted, std::string(strings.text(symbol(emitted)))}});
        return out;
    }

    // Play journal to an empty database.
    void replay(const std::vector<journal_entry>& entries) {
        for (const auto& e : entries) {
            std::visit([&](const auto& op) {
                using T = std::remove_cvref_t<decltype(op)>;
                if constexpr (std::is_same_v<T, j_intern>) {
                    if (strings.intern(op.text).value() != op.id) throw diagnostic("журнал повреждён: порядок строк", {});
                } else if constexpr (std::is_same_v<T, j_add>) {
                    if (add(op.kind, op.where).value() != op.id) throw diagnostic("журнал повреждён: порядок узлов", {});
                } else if constexpr (std::is_same_v<T, j_replace>) {
                    replace(node_id(op.id), op.kind);
                } else if constexpr (std::is_same_v<T, j_link>) {
                    link_parents(node_id(op.root));
                } else {
                    record_import(op.text, op.label);
                }
            }, e.op);
        }
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
    void log(journal_op op) { log_.push_back({strings.size(), journal_entry{revision_, std::move(op)}}); }

    void link_rec(node_id root) {
        for_each_child(root, [&](node_id child) {
            nodes_[child].parent = root;
            link_rec(child);
        });
    }

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

    arena<node, node_tag>                            nodes_;
    u64                                              revision_ = 0;
    std::vector<std::pair<u32, journal_entry>>       log_; // (line interned at time, operation)
    std::vector<std::string>                         sources_;
    std::vector<std::string>                         labels_;
    std::vector<u64>                                 node_rev_;
};

// Diagnostics on the basis: the text is taken from the import to which the node belongs.
inline std::string render(const diagnostic& d, std::string_view file, const ast_db& db) {
    auto name = [&](span w) -> std::string_view {
        const auto& l = db.source_label(w.source);
        return l.empty() ? file : std::string_view(l);
    };
    auto lc  = locate(db.source(d.where().source), d.where().begin);
    auto out = std::format("{}:{}:{}: ошибка: {}\n", name(d.where()), lc.line, lc.col, d.what());
    for (const auto& f : d.trace()) {
        auto l = locate(db.source(f.where.source), f.where.begin);
        out += std::format("  {}:{}:{}: {}\n", name(f.where), l.line, l.col, f.what);
    }
    return out;
}

// The name node is for debugging (through reflection, without manual tables).
inline std::string_view kind_name(const node& n) {
    return std::visit([](const auto& k) -> std::string_view {
        return std::meta::identifier_of(std::meta::remove_cvref(^^decltype(k)));
    }, n.kind);
}

} // namespace tz
