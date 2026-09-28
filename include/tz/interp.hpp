// Interpreter: Executes AST base node directly (decision 2026-09-27).
//
//;
//.
//
// Two modes, one code:
// - run: normal performance;
// - check: same interpreter with runtime erased (value approach):
// The value is either known (compile-time) or erased to type (run-time <T>).
// `typeof(f(x))` is a f(x) execution with run-time values erased.
// The stage is output by itself, like a convolution of constants.
// branch on run-time - condition with different types of branches clones
// Continuation: foo => [T0 => bar, T1 => bar], no variant.
// instance - runtime part of arguments; fully folded into compile-time
// The instance call does not give (: ).
//
//
// From the speck: semantic inlining and address return (§6.1), parameters - alias
// (§6.2), overload with demand (§5.4), strict numbers (§5.8).
#pragma once

#include "value.hpp"

#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <unordered_map>
#include <set>
#include <sstream>

namespace tz {

// Lexical scope. Variables are cells; alias indicates the caller cell rather than
// copying it.
struct scope {
    std::shared_ptr<scope>                  parent;
    std::vector<std::pair<symbol, value*>> vars;
    std::deque<value>                       storage;

    value* declare(symbol name, value v) {
        storage.push_back(std::move(v));
        vars.push_back({name, &storage.back()});
        return &storage.back();
    }

    void alias(symbol name, value* target) { vars.push_back({name, target}); }

    value* find(symbol name) {
        for (auto it = vars.rbegin(); it != vars.rend(); ++it)
            if (it->first == name) return it->second;
        return parent ? parent->find(name) : nullptr;
    }

    // Copy of the area for cloned continuation (checkout mode).
    std::shared_ptr<scope> clone() const {
        auto c    = std::make_shared<scope>();
        c->parent = parent;
        for (auto& [name, cell] : vars) c->declare(name, *cell);
        return c;
    }
};

using scope_ptr = std::shared_ptr<scope>;

enum class mode : uint8_t { run, check };

// --- reactive cache of static parsing (§10.1) ---
//
// Granularity is functions, not files. instance functions (key: function + compile-time /
// run-time -arguments + continuation) are remembered along with the result, dependencies
// (whose bodies were executed, with node revisions) and report events. After editing,
// only instance with changed dependencies are recalculated, the rest are reproduced. Cash
// lives in a long-lived VM session (tz vm).
//
struct report_event {
    enum kind_t : uint8_t { instance, folded, stage, split, excluded, live, proven } kind;
    std::string              a, b, c; // meaning depends on the species
    span                     where;
    std::vector<std::string> alts;
    uint32_t                      fn = 0;
};

struct check_cache {
    struct entry {
        value                     result;
        std::map<uint32_t, uint64_t>        deps; // Node of function → revision at parsing
        std::vector<report_event> events;
        std::vector<std::tuple<uint32_t, value, bool, bool, bool>> journal; // observation
        bool                      effect = false;
    };
    std::map<std::string, entry> entries;
    uint32_t                          hits = 0, recomputed = 0;
    std::vector<std::string>     recomputed_names;
};

struct interpreter {
    // residual program inputs: known arguments and strings of stdin (§13.2).
    struct known_inputs {
        std::vector<std::string> stdin_lines;
    };

    interpreter(ast_db& db, node_id module, std::vector<std::string> args, mode m = mode::run,
                std::optional<known_inputs> known = std::nullopt, check_cache* cache = nullptr)
        : db(db), mod(module), run_mode(m), cache(cache), inputs(std::move(known)) {
        globals = std::make_shared<scope>();
        array_v a;
        for (auto& s : args) a.push(value(s));
        value args_v(std::move(a));
        if (checking() && !inputs) args_v = erase(value(array_v::of({value(std::string("x"))}))); // arguments - runtime
        globals->declare(sym("args"), std::move(args_v));
        globals->declare(sym("stdin"), value(agg_v{types.named("stream"), {}})); // descriptor, no value
        for (node_id item : db.as<n_module>(module).items)
            if (db.is<n_fn>(item)) functions[db.as<n_fn>(item).name].push_back(item);
    }

    // Launch main(). Returns the exit code.
    int run_main() {
        auto it = functions.find(sym("main"));
        if (it == functions.end()) throw diagnostic("нет функции main", {});
        call_site site{db[it->second.front()].where, "main"};
        flow r = call_overloaded(it->second, {}, globals, 0, site);
        return r.v.is<int64_t>() ? int(r.v.as<int64_t>()) : 0;
    }

    // Observations of node mode (for residual program).
    struct observation {
        value v;
        bool  seen     = false; // knot was executed and gave meaning
        bool  diverted = false; // At least go back/break.
        bool  effect   = false; // The result was a runtime effect.
        bool  present  = false; // knot
    };

    // Observing node in One Embedding Context
//.
    const observation* observed(node_id n, uint64_t at) const {
        build_observed();
        auto it = observations_ctx.find({n.value, at});
        return it == observations_ctx.end() ? nullptr : &it->second;
    }

    // The body context of a function called from a node call in a ctx context.
    static uint64_t inline_context(uint64_t ctx, uint32_t call) { return (ctx ^ (uint64_t(call) + 0x9e3779b97f4a7c15ull)) * 0x100000001b3ull; }

    const observation* observed(node_id n) const {
        build_observed();
        auto it = observations.find(n.value);
        return it == observations.end() ? nullptr : &it->second;
    }

    bool live(node_id fn) const { return live_fns.contains(fn.value); }

    // Show the meaning as literal, if possible.
    std::optional<std::string> literal(const value& v) const {
        if (!v.known()) return std::nullopt;
        if (v.is<int64_t>() || v.is<double>() || v.is<bool>()) return show_v(v);
        if (v.is<std::string>()) return std::format("{:?}", v.as<std::string>());
        if (v.is<array_v>()) {
            std::string s = "array{";
            for (uint32_t i = 0; i < v.as<array_v>().size(); ++i) {
                auto item = literal(v.as<array_v>().get(i));
                if (!item) return std::nullopt;
                s += (i ? ", " : "") + *item;
            }
            return s + "}";
        }
        return std::nullopt;
    }

    // Static parsing of one instance (for JIT): parameters are erased values of given
    // types; the result is the type (or compile-time) of the result.
    value analyze_instance(node_id f, std::vector<value> args) {
        const auto& fn    = db.as<n_fn>(f);
        auto        inner = std::make_shared<scope>();
        inner->parent     = globals;
        for (uint32_t i = 0; i < fn.params.size() && i < args.size(); ++i) inner->declare(db.as<n_param>(fn.params[i]).name, args[i]);
        flow r = run_frame(fn.body, inner, fn.name, fn.outline);
        return r.v;
    }

    // Static parsing step budget (for JIT: compiling calculations should not be
    // unlimited).
    void set_fuel(uint64_t steps) { fuel = steps; }

    // Connect JIT (run mode): instance is executed by machine code, if JIT supports
    // them, otherwise by the interpreter.
    void use_jit(struct jit_engine* engine) { jit = engine; }

    // All print output is in line (for tests), otherwise in stdout.
    void capture_output(std::string* to) { captured = to; }

    // instance: function + (runtime-parts) of arguments → how many times.
    std::string instances() const {
        std::string out;
        for (const auto& [key, count] : instance_counts) out += std::format("  {}  ×{}\n", key, count);
        return out;
    }

    // Verification mode report.
    std::string report(std::string_view file, std::string_view src) const {
        std::string out;
        (void)src;
        auto loc = [&](span w) {
            auto        lc    = locate(db.source(w.source), w.begin);
            const auto& label = db.source_label(w.source);
            return std::format("{}:{}", label.empty() ? file : std::string_view(label), lc.line);
        };
        out += "стадии:\n";
        for (const auto& s : stages) out += std::format("  {} {}{} = {}\n", loc(s.where), s.path, s.name, s.what);
        if (!splits.empty()) {
            out += "расщепления:\n";
            for (const auto& s : splits) out += std::format("  {} {} → {} продолжения: {}\n", loc(s.where), s.name, s.alts.size(), join(s.alts));
        }
        if (!excluded.empty()) {
            out += "исключённые блоки try_compile:\n";
            for (const auto& e : excluded) out += std::format("  {} {}{}\n", loc(e.where), e.path, e.why);
        }
        if (!folded.empty()) {
            out += "свёрнуто в кт:\n";
            for (const auto& [name, count] : folded) out += std::format("  {}  ×{}\n", name, count);
        }
        if (!param_writes.empty()) {
            out += "функции меняют параметры (место вызывающего):\n";
            for (const auto& w : param_writes) out += std::format("  {}\n", w);
        }
        if (!proven_list.empty()) {
            out += "индексы, доказанные в границах (проверка снята):\n";
            for (const auto& [w, what] : proven_list) out += std::format("  {} {}\n", loc(w), what);
        }
        out += "экземпляры (рантайм-часть):\n" + instances();
        return out;
    }

    // Indexing proved by parsing within the array boundaries ("top-up optimization": the
    // language knows the check is off and shows it in the report).
    bool bounds_proven(node_id n) const { return proven.contains(n.value); }
    // Does the function change its k parameter (inference, see writes_param).
    bool param_written(node_id f, uint32_t k) const { return writes_param(f, k); }
    // --- flow ---

    enum class flow_kind : uint8_t { normal, ret, brk, cont };

    struct flow {
        flow_kind kind = flow_kind::normal;
        value     v;
        uint32_t       target = 0; // a frame from which comes the return

        bool normal() const { return kind == flow_kind::normal; }
    };

    static flow ok(value v = {}) { return flow{flow_kind::normal, std::move(v), 0}; }

    struct call_site {
        span        where;
        std::string what;
        uint32_t         node = 0; // call node: from the chain of such node is formed the context of embedding
    };

    bool checking() const { return run_mode == mode::check; }

    // --- call-trace errors ---

    [[noreturn]] void fail(std::string msg, span where, diag_kind kind = diag_kind::instantiation) {
        diagnostic d(std::move(msg), where, kind);
        if (!path.empty()) d.note(std::format("в продолжении {}", path), where);
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(std::format("в {}", it->what), it->where);
        throw d;
    }

    symbol           sym(std::string_view s) { return db.strings.intern(s); }
    std::string_view str(symbol s) const { return db.strings.text(s); }
    std::string      show_v(const value& x) const { return show(x, types, db.strings); }

    static std::string join(const std::vector<std::string>& xs) {
        std::string s;
        for (auto& x : xs) s += (s.empty() ? "" : " | ") + x;
        return s;
    }

#define TZ_TRY(var, expr)                    \
    flow var##_f = (expr);                   \
    if (!var##_f.normal()) return var##_f;   \
    value var = std::move(var##_f.v)

    flow eval(node_id n, const scope_ptr& env, uint32_t frame) {
        if (!checking()) return std::visit([&](const auto& k) { return eval_node(n, k, env, frame); }, db[n].kind);
        if (fuel && --*fuel == 0) fail("бюджет статического разбора исчерпан", db[n].where);
        // Check mode: observation - what value the node received and whether there was a
        // runtime effect (necessary for residual program, §13.2)
        uint64_t  effects_before = effects;
        flow f = std::visit([&](const auto& k) { return eval_node(n, k, env, frame); }, db[n].kind);
        observe(n, f, effects != effects_before);
        return f;
    }

    // All changes to the report go through emit: so they can be written to the
    // instance cache and reproduced when hit.
    void emit(report_event ev) {
        for (auto* rec : recording) rec->push_back(ev);
        switch (ev.kind) {
        case report_event::instance: ++instance_counts[ev.a]; break;
        case report_event::folded: ++folded[ev.a]; break;
        case report_event::stage: stages.push_back({ev.where, ev.a, ev.b, ev.c}); break;
        case report_event::split: splits.push_back({ev.where, ev.a, ev.alts}); break;
        case report_event::excluded: excluded.push_back({ev.where, ev.a, ev.b}); break;
        case report_event::live: live_fns.insert(ev.fn); break;
        case report_event::proven:
            if (proven.insert(ev.fn).second) proven_list.push_back({ev.where, ev.a});
            break;
        }
    }

    // Observations are written in journal: the call, completely folded into compile-time,
    // rolls back its part - it will not be in runtime, and its values should not interfere
    // with the convolution of living code.
    struct obs_entry {
        uint32_t   node;
        value v;
        bool  seen, diverted, effect;
        uint64_t   ctx = 0; // Embedded context (node chain)
    };

    void observe(node_id n, const flow& f, bool effect) {
        if (!f.normal()) journal.push_back({n.value, {}, false, true, effect, ctx});
        else journal.push_back({n.value, f.v, true, false, effect, ctx});
        observations_dirty = true;
    }

    void mark_executed(node_id n) {
        journal.push_back({n.value, {}, false, false, false, ctx});
        observations_dirty = true;
    }

    void build_observed() const {
        if (!observations_dirty) return;
        observations.clear();
        observations_ctx.clear();
        auto add = [&](observation& o, const obs_entry& e) {
            o.effect |= e.effect;
            o.diverted |= e.diverted;
            o.present = true;
            if (e.seen) {
                o.v    = o.seen ? const_cast<interpreter*>(this)->merge(o.v, e.v) : e.v;
                o.seen = true;
            }
        };
        for (const auto& e : journal) {
            add(observations[e.node], e);
            add(observations_ctx[{e.node, e.ctx}], e);
        }
        observations_dirty = false;
    }

    // The value of the variable; thunk (expr parameter) is calculated when mentioned.
    flow read(value* cell) {
        if (cell->is<thunk_v>()) {
            thunk_v t     = cell->as<thunk_v>();
            uint64_t     saved = std::exchange(ctx, t.ctx); // expression of the caller in its context
            flow    r;
            try {
                r = eval(t.expr, t.env, t.frame);
            } catch (...) {
                ctx = saved;
                throw;
            }
            ctx = saved;
            return r;
        }
        return ok(*cell);
    }

    // --- Merge and splitting (Verification Mode) ---

    // Merge the values of two paths: the same compile-time - compile-time; one type
    // - run-time; different types - splitting (without variant).
    value merge(const value& a, const value& b) {
        std::vector<value> alts;
        auto add = [&](const value& x) {
            if (x.is<split_v>()) { for (auto& y : x.as<split_v>().alts) add_alt(alts, y); }
            else add_alt(alts, x);
        };
        add(a);
        add(b);
        return alts.size() == 1 ? alts[0] : value(split_v{std::move(alts)});
    }

    void add_alt(std::vector<value>& alts, const value& x) {
        if (unknown(x)) { if (alts.empty()) alts.push_back(x); return; } // ⊥ ⊔ T = T
        if (alts.size() == 1 && unknown(alts[0])) { alts[0] = x; return; }
        for (auto& y : alts) {
            if (types.describe(y) != types.describe(x)) continue;
            if (y.known() && x.known() && show_v(y) == show_v(x)) return; // calculus
            y = erase(y);                                                   // same type, run-time
            return;
        }
        alts.push_back(x);
    }

    // Apply f to each split value alternative.
    value lift(const value& x, auto&& f) {
        if (!x.is<split_v>()) return f(x);
        value out;
        bool  first = true;
        for (const auto& a : x.as<split_v>().alts) {
            value r = f(a);
            out     = first ? r : merge(out, r);
            first   = false;
        }
        return out;
    }

    // bool, known statically; nullopt is run-time-bool (branch by runtime).
    std::optional<bool> known_bool(const value& v, span where) {
        if (v.is<bool>()) return v.as<bool>();
        if (unknown(v)) return std::nullopt;
        if (v.is<rt_v>() && types.describe(v) == "bool") return std::nullopt;
        if (v.is<split_v>()) return std::nullopt;
        fail(std::format("условие должно быть bool, получено {}", types.describe(v)), where);
    }

    bool truthy(const value& v, span where) {
        auto b = known_bool(v, where);
        if (!b) fail("условие неизвестно статически", where);
        return *b;
    }

    // Returns accumulated by branches under run-time-condition: frame → values.
    void record_return(const flow& f) {
        may_return[f.target].push_back(f.v);
        // return to external frame (return(caller) over run-time-branch): calls for which the
        // target is external cannot be cached
        for (auto& [floor, outer] : frame_floors)
            if (f.target <= floor) outer = true;
    }

    // Only instance is cached with no side effect on the caller: no expr-arguments
    // (inline-function is part of the caller) and no mut/own/out-parameters (changes
    // of the caller cells cannot be reproduced).
    bool memoizable(node_id f, const std::vector<value>& vals) const {
        for (const auto& v : vals) if (v.is<thunk_v>()) return false;
        for (node_id p : db.as<n_fn>(f).params) {
            auto m = db.as<n_param>(p).mode;
            if (m == param_mode::mut || m == param_mode::own || m == param_mode::out || m == param_mode::expr) return false;
        }
        return true;
    }

    // The result of the branching under run-time-condition: the branch with the return is
    // registered as a possible return and does not continue.
    flow join_branches(flow a, flow b) {
        bool a_cont = a.normal(), b_cont = b.normal();
        if (a.kind == flow_kind::ret && b.kind == flow_kind::ret && a.target == b.target)
            return flow{flow_kind::ret, merge(a.v, b.v), a.target};
        if (!a_cont && a.kind == flow_kind::ret) record_return(a);
        if (!b_cont && b.kind == flow_kind::ret) record_return(b);
        if (a_cont && b_cont) return ok(merge(a.v, b.v));
        if (a_cont) return a;
        if (b_cont) return b;
        return a; // Break/continue in both - M0 does not distinguish between
    }

    // --- node: Literaries and names ---

    flow eval_node(node_id, const n_int& x, const scope_ptr&, uint32_t) { return ok(value(x.value)); }
    flow eval_node(node_id, const n_float& x, const scope_ptr&, uint32_t) { return ok(value(x.value)); }
    flow eval_node(node_id, const n_str& x, const scope_ptr&, uint32_t) { return ok(value(x.value)); }

    flow eval_node(node_id n, const n_ident& x, const scope_ptr& env, uint32_t) {
        if (value* cell = env->find(x.name)) return read(cell);
        if (str(x.name) == "true") return ok(value(true));
        if (str(x.name) == "false") return ok(value(false));
        if (auto t = types.builtin(str(x.name))) return ok(value(type_v{t}));
        if (auto it = functions.find(x.name); it != functions.end()) {
            for (node_id f : it->second)
                if (inline_required(f))
                    fail(std::format("функция {} использует семантику инлайна (expr-параметр или return(…)) и не может "
                                     "быть значением; вызывайте её напрямую",
                                     str(x.name)),
                         db[n].where);
            return ok(value(fn_v{x.name, {}, nullptr}));
        }
        fail(std::format("неизвестное имя {}", str(x.name)), db[n].where);
    }

    // --- blockages ---

    flow eval_node(node_id n, const n_block& b, const scope_ptr& env, uint32_t frame) {
        auto inner    = std::make_shared<scope>();
        inner->parent = env;
        return run_block(b, 0, inner, frame);
    }

    // Execute the block instructions starting with from. In verification mode,
    // declaring a split-value variable clones a continuation.
    flow run_block(const n_block& b, uint32_t from, const scope_ptr& inner, uint32_t frame) {
        value last;
        for (uint32_t i = from; i < b.items.size(); ++i) {
            node_id s = b.items[i];
            if (checking() && db.is<n_var>(s)) {
                const auto& v = db.as<n_var>(s);
                mark_executed(s); // instruction
                TZ_TRY(init, eval(v.init, inner, frame));
                if (init.is<split_v>()) return split_continuation(b, i, s, init, inner, frame);
                declare_var(s, v, std::move(init), inner);
                continue;
            }
            flow f = eval(s, inner, frame);
            if (!f.normal()) return f;
            last = std::move(f.v);
        }
        return ok(b.has_value ? std::move(last) : value());
    }

    flow split_continuation(const n_block& b, uint32_t i, node_id s, const value& init, const scope_ptr& inner, uint32_t frame) {
        const auto&              v = db.as<n_var>(s);
        std::vector<std::string> names;
        for (const auto& a : init.as<split_v>().alts) names.push_back(types.describe(a));
        emit({report_event::split, std::string(str(v.name)), {}, {}, db[s].where, names});

        flow result;
        bool first = true;
        for (const auto& alt : init.as<split_v>().alts) {
            auto        cont = inner->clone();
            std::string saved = path;
            path += std::format("[{}: {}] ", str(v.name), types.describe(alt));
            declare_var(s, v, alt, cont);
            flow f = run_block(b, i + 1, cont, frame);
            path  = saved;
            result = first ? f : join_branches(result, f);
            first  = false;
        }
        return result;
    }

    void declare_var(node_id s, const n_var& v, value init, const scope_ptr& env) {
        if (v.type && db.is<n_ident>(v.type)) check_type(init, db.as<n_ident>(v.type).name, db[v.init].where);
        if (checking())
            emit({report_event::stage, path, std::string(str(v.name)),
                  init.known() ? std::format("кт {}", show_v(init)) : std::format("рт {}", types.describe(init)), db[s].where, {}});
        env->declare(v.name, std::move(init));
    }

    flow eval_node(node_id n, const n_var& v, const scope_ptr& env, uint32_t frame) {
        TZ_TRY(init, eval(v.init, env, frame));
        declare_var(n, v, std::move(init), env);
        return ok();
    }

    flow eval_node(node_id n, const n_fn& f, const scope_ptr& env, uint32_t) {
        env->declare(f.name, value(fn_v{{}, n, env})); // local function = lambda with a name
        return ok();
    }

    flow eval_node(node_id n, const n_return& r, const scope_ptr& env, uint32_t frame) {
        value v;
        if (r.value) {
            TZ_TRY(x, eval(r.value, env, frame));
            v = std::move(x);
        }
        uint32_t target = r.target ? return_target(db.as<n_ident>(r.target).name, frame, db[n].where) : frame;
        return flow{flow_kind::ret, std::move(v), target};
    }

    // return(f) is the output from the external function f in the call chain;
    // return(caller) is the output from the calling function.
// The outline boundary is not allowed: there is no inline.
    uint32_t return_target(symbol name, uint32_t frame, span where) {
        int64_t here = -1;
        for (int64_t i = int64_t(frames.size()) - 1; i >= 0; --i)
            if (frames[i].id == frame) { here = i; break; }
        if (here < 0) fail("return вне функции", where);
        bool to_caller = str(name) == "caller";
        for (int64_t i = here - 1; i >= 0; --i) {
            if (frames[i + 1].outline)
                fail(std::format("return({}) через outline-функцию {}: у неё нет семантики инлайна", str(name),
                                 str(frames[i + 1].name)),
                     where);
            if (to_caller || frames[i].name == name) return frames[i].id;
        }
        fail(std::format("return({}): нет такой внешней функции в цепочке вызовов", str(name)), where);
    }

    flow eval_node(node_id n, const n_require& r, const scope_ptr& env, uint32_t frame) {
        TZ_TRY(c, eval(r.cond, env, frame));
        auto b = known_bool(c, db[r.cond].where);
        if (b && !*b) fail("нарушено требование require", db[n].where, diag_kind::runtime);
        return ok(); // run-time - Condition: runtime (§5.4)
    }

    // --- branching ---

    flow eval_node(node_id n, const n_if& i, const scope_ptr& env, uint32_t frame) {
        TZ_TRY(c, eval(i.cond, env, frame));
        auto b = known_bool(c, db[i.cond].where);
        if (b) {
            if (*b) return eval(i.then_, env, frame);
            return i.else_ ? eval(i.else_, env, frame) : ok();
        }
        // run-time - Condition (only in check mode): both branches are disassembled
        flow t = eval(i.then_, env, frame);
        flow e = i.else_ ? eval(i.else_, env, frame) : ok();
        return join_branches(std::move(t), std::move(e));
    }

    flow eval_node(node_id n, const n_while& w, const scope_ptr& env, uint32_t frame) {
        for (uint32_t guard = 0;; ++guard) {
            TZ_TRY(c, eval(w.cond, env, frame));
            auto b = known_bool(c, db[w.cond].where);
            if (b && !*b) return ok();
            if (!b) return abstract_loop(w.body, env, frame, {}, {});
            flow f = eval(w.body, env, frame);
            if (f.kind == flow_kind::brk) return ok();
            if (f.kind == flow_kind::ret) return f;
            if (checking() && guard > 100000) fail("цикл не сошёлся при статическом разборе", db[n].where);
        }
    }

    flow eval_node(node_id n, const n_for& f, const scope_ptr& env, uint32_t frame) {
        TZ_TRY(r, eval(f.range, env, frame));
        if (r.is<rt_v>()) {
            const value* shape = shape_of(r);
            if (!shape || !shape->is<array_v>()) fail(std::format("for ожидает массив, получено {}", types.describe(r)), db[f.range].where);
            value elem = shape->as<array_v>().empty() ? value(rt_v{}) : shape->as<array_v>().get(0);
            const value* src = index_source(f, env);
            return abstract_loop(f.body, env, frame, f.var, elem, src,
                                 src ? db.as<n_ident>(db.as<n_member>(db.as<n_call>(f.range).args[0]).object).name : symbol{});
        }
        if (!r.is<array_v>()) fail(std::format("for ожидает массив, получено {}", types.describe(r)), db[f.range].where);
        const auto& arr = r.as<array_v>();
        for (uint32_t k = 0; k < arr.size(); ++k) {
            auto inner    = std::make_shared<scope>();
            inner->parent = env;
            inner->declare(f.var, arr.get(k));
            flow b = eval(f.body, inner, frame);
            if (b.kind == flow_kind::brk) break;
            if (b.kind == flow_kind::ret) return b;
        }
        return ok();
    }

    // A cycle whose number of iterations is known only in runtime: the variables that
    // the body assigns are erased to type; the body is disassembled to a fixed point
    // of types (usually 1-2 passes).
    flow abstract_loop(node_id body, const scope_ptr& env, uint32_t frame, symbol var, const value& elem,
                       const value* indexes = nullptr, symbol indexes_name = {}) {
        std::set<symbol> assigned;
        collect_assigned(body, assigned);
        for (symbol s : assigned)
            if (value* c = env->find(s)) *c = erase(*c);
        for (uint32_t pass = 0; pass < 4; ++pass) {
            std::vector<std::string> before;
            for (symbol s : assigned) if (value* c = env->find(s)) before.push_back(types.describe(*c));
            auto inner    = std::make_shared<scope>();
            inner->parent = env;
            if (var) inner->declare(var, elem);
            if (indexes) index_proofs.push_back({inner->find(var), indexes, indexes_name});
            flow b = eval(body, inner, frame);
            if (indexes) index_proofs.pop_back();
            if (b.kind == flow_kind::ret) record_return(b);
            std::vector<std::string> after;
            for (symbol s : assigned) if (value* c = env->find(s)) { *c = erase(*c); after.push_back(types.describe(*c)); }
            if (before == after) return ok();
        }
        fail("типы переменных цикла не сошлись", db[body].where);
    }

    // Safe index
// Cycle variable for(i: range(a.size)) is the array index a, while
    // The body does not change either i or a completely and does not give a to change.
    // The growth of a (push) proof does not break. Returns cell a or nullptr.
    void note_index_proof(node_id n, const scope_ptr& env, bool write = false) {
        if (!checking() || index_proofs.empty()) return;
        const auto&  i  = db.as<n_index>(n);
        const value* ix = peek(i.index, env);
        const value* ar = peek(i.object, env);
        // both cell and name: another name of the same cell (two aliases of the same
        // array for this call) proves nothing for the instance as a whole.
        for (const auto& [var_cell, arr_cell, arr_name] : index_proofs)
            if (ix && ix == var_cell && ar == arr_cell && db.is<n_ident>(i.object) && db.as<n_ident>(i.object).name == arr_name)
                emit({report_event::proven, std::format("{}[{}]{}", str(db.as<n_ident>(i.object).name), str(db.as<n_ident>(i.index).name), write ? " (запись)" : ""),
                      {}, {}, db[n].where, {}, n.value});
    }

    const value* index_source(const n_for& f, const scope_ptr& env) {
        if (!db.is<n_call>(f.range)) return nullptr;
        const auto& c = db.as<n_call>(f.range);
        if (!db.is<n_ident>(c.callee) || str(db.as<n_ident>(c.callee).name) != "range" || c.args.size() != 1) return nullptr;
        if (!db.is<n_member>(c.args[0]) || str(db.as<n_member>(c.args[0]).name) != "size") return nullptr;
        node_id obj = db.as<n_member>(c.args[0]).object;
        if (!db.is<n_ident>(obj)) return nullptr;
        symbol       arr  = db.as<n_ident>(obj).name;
        const value* cell = peek(obj, env);
        if (!cell || !shape_of(*cell) || !shape_of(*cell)->is<array_v>()) return nullptr;
        return keeps_index(f.body, f.var, arr) ? cell : nullptr;
    }

    bool keeps_index(node_id n, symbol var, symbol arr) const {
        if (!n) return true;
        if (db.is<n_lambda>(n)) return false; // The capture could change later.
        if (db.is<n_var>(n) && (db.as<n_var>(n).name == var || db.as<n_var>(n).name == arr)) return false; // shadow
        if (db.is<n_op>(n)) {
            const auto& o  = db.as<n_op>(n);
            auto        op = str(o.op);
            if ((op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") && !o.args.empty() && db.is<n_ident>(o.args[0])) {
                symbol t = db.as<n_ident>(o.args[0]).name;
                if (t == var || t == arr) return false;
            }
        }
        if (db.is<n_call>(n)) { // a as an argument for a changing parameter
            const auto& c = db.as<n_call>(n);
            for (uint32_t k = 0; k < c.args.size(); ++k) {
                if (!db.is<n_ident>(c.args[k])) continue;
                symbol a = db.as<n_ident>(c.args[k]).name;
                if (a != arr && a != var) continue;
                if (!db.is<n_ident>(c.callee)) return false;
                auto it = functions.find(db.as<n_ident>(c.callee).name);
                if (it == functions.end()) continue; // Built-in functions don't change arguments
                for (node_id fn : it->second)
                    if (writes_param(fn, k)) return false;
            }
        }
        bool ok_ = true;
        db.for_each_child(n, [&](node_id ch) { ok_ = ok_ && keeps_index(ch, var, arr); });
        return ok_;
    }

    // Does the function change its k-th parameter (place of caller)? It is not forbidden
    // and does not require marking - it is displayed.
//  и
    // Seen in the check report. Mut is an optional explicit mark.
    bool writes_param(node_id f, uint32_t k) const {
        const auto& fn = db.as<n_fn>(f);
        if (k >= fn.params.size()) return false;
        auto m = db.as<n_param>(fn.params[k]).mode;
        if (m == param_mode::mut || m == param_mode::out || m == param_mode::own || m == param_mode::expr) return true;
        if (m == param_mode::ct) return false;
        uint64_t key = (uint64_t(f.value) << 32) | k;
        if (auto it = writes.find(key); it != writes.end()) {
            if (writes_open.contains(key)) writes_guess_used = true;
            return it->second;
        }
        // Recursion: fixed point at the bottom. The counted key is “not changing” yet;
        // “changing” only grows, so the top call is recalculated while the guesses were
        // read and the set “changes” grew.
        auto compute = [&] {
            writes[key] = false;
            writes_open.insert(key);
            bool w = writes_name(fn.body, db.as<n_param>(fn.params[k]).name);
            writes_open.erase(key);
            writes[key] = w;
        };
        if (!writes_open.empty()) { compute(); return writes[key]; }
        auto trues = [&] { return std::ranges::count_if(writes, [](const auto& e) { return e.second; }); };
        for (;;) {
            auto before        = trues();
            writes_guess_used = false;
            std::erase_if(writes, [](const auto& e) { return !e.second; });
            compute();
            if (!writes_guess_used || trues() == before) return writes[key];
        }
    }

    // Does the code change the place with that name: assigning (including to an element/field)
    // or transferring to a parameter that changes the function being called? Shadow, stop.
    bool writes_name(node_id n, symbol name) const {
        if (!n || db.is<n_fn>(n) || db.is<n_lambda>(n)) return false;
        if (db.is<n_block>(n)) {
            for (node_id k : db.as<n_block>(n).items) {
                if (writes_name(k, name)) return true;
                if (db.is<n_var>(k) && db.as<n_var>(k).name == name) return false;
            }
            return false;
        }
        if (db.is<n_for>(n)) {
            if (writes_name(db.as<n_for>(n).range, name)) return true;
            return db.as<n_for>(n).var != name && writes_name(db.as<n_for>(n).body, name);
        }
        if (db.is<n_op>(n)) {
            const auto& o  = db.as<n_op>(n);
            auto        op = str(o.op);
            if ((op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") && !o.args.empty()) {
                node_id t = o.args[0];
                while (db.is<n_index>(t) || db.is<n_member>(t))
                    t = db.is<n_index>(t) ? db.as<n_index>(t).object : db.as<n_member>(t).object;
                if (db.is<n_ident>(t) && db.as<n_ident>(t).name == name) return true;
            }
        }
        if (db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) {
            const auto& c = db.as<n_call>(n);
            if (auto it = functions.find(db.as<n_ident>(c.callee).name); it != functions.end())
                for (uint32_t k = 0; k < c.args.size(); ++k) {
                    node_id a = c.args[k];
                    while (db.is<n_index>(a) || db.is<n_member>(a))
                        a = db.is<n_index>(a) ? db.as<n_index>(a).object : db.as<n_member>(a).object;
                    if (!db.is<n_ident>(a) || db.as<n_ident>(a).name != name) continue;
                    for (node_id fn : it->second)
                        if (writes_param(fn, k)) return true;
                }
        }
        bool w = false;
        db.for_each_child(n, [&](node_id c) { w = w || writes_name(c, name); });
        return w;
    }

    void collect_assigned(node_id n, std::set<symbol>& out) const {
        if (!n) return;
        if (db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) { // change-place
            const auto& c = db.as<n_call>(n);
            if (auto it = functions.find(db.as<n_ident>(c.callee).name); it != functions.end())
                for (uint32_t k = 0; k < c.args.size(); ++k) {
                    node_id a = c.args[k];
                    while (db.is<n_index>(a)) a = db.as<n_index>(a).object;
                    if (!db.is<n_ident>(a)) continue;
                    for (node_id fn : it->second)
                        if (writes_param(fn, k)) out.insert(db.as<n_ident>(a).name);
                }
        }
        if (db.is<n_op>(n)) {
            const auto& o  = db.as<n_op>(n);
            auto        op = str(o.op);
            if ((op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") && !o.args.empty()) {
                node_id t = o.args[0];
                while (db.is<n_index>(t)) t = db.as<n_index>(t).object;
                if (db.is<n_ident>(t)) out.insert(db.as<n_ident>(t).name);
            }
        }
        db.for_each_child(n, [&](node_id c) { collect_assigned(c, out); });
    }

    flow eval_node(node_id n, const n_ct& c, const scope_ptr& env, uint32_t frame) {
        TZ_TRY(v, eval(c.body, env, frame));
        if (checking() && !v.known()) fail(std::format("ct: значение не вычислимо при компиляции (рт {})", types.describe(v)), db[n].where);
        return ok(std::move(v));
    }

    // SFINAE block (§5.4):
// runtime - errors don't get jammed.
    flow eval_node(node_id n, const n_try_compile& t, const scope_ptr& env, uint32_t frame) {
        auto saved_may = may_return;
        try {
            return eval(t.body, env, frame);
        } catch (const diagnostic& d) {
            if (d.kind != diag_kind::instantiation) throw;
            may_return = std::move(saved_may);
            emit({report_event::excluded, path, d.what(), {}, db[n].where, {}});
            return ok();
        }
    }

    flow eval_node(node_id n, const n_lambda& l, const scope_ptr& env, uint32_t) { return ok(value(fn_v{{}, n, env})); }

    flow eval_node(node_id n, const n_construct& c, const scope_ptr& env, uint32_t frame) {
        std::vector<value> items;
        for (node_id item : c.items) {
            TZ_TRY(x, eval(item, env, frame));
            items.push_back(std::move(x));
        }
        auto tname = str(db.as<n_ident>(c.type).name);
        if (tname != "array") fail(std::format("конструктор {}{{}} пока не поддержан", tname), db[n].where);
        for (const auto& x : items)
            if (!x.known()) return ok(erase(value(array_v::of({x})))); // At least one run-time element is an array of run-time
        return ok(value(array_v::of(std::move(items))));
    }

    // --- field access ---

    flow eval_node(node_id n, const n_member& m, const scope_ptr& env, uint32_t frame) {
        if (const value* o = peek(m.object, env); o && o->is<array_v>()) return ok(member(*o, m.name, db[n].where));
        TZ_TRY(obj, eval(m.object, env, frame));
        return ok(lift(obj, [&](const value& o) { return member(o, m.name, db[n].where); }));
    }

    value member(const value& obj, symbol name, span where) {
        if (obj.is<rt_v>()) {
            const value* shape = shape_of(obj);
            if (!shape) return value(rt_v{});
            return erase(member(*shape, name, where));
        }
        if (str(name) == "size") {
            if (obj.is<array_v>()) return value(int64_t(obj.as<array_v>().size()));
            if (obj.is<std::string>()) return value(int64_t(obj.as<std::string>().size()));
        }
        if (obj.is<agg_v>())
            for (auto& [k, v] : obj.as<agg_v>().fields)
                if (k == name) return v;
        fail(std::format("у значения типа {} нет поля {}", types.describe(obj), str(name)), where);
    }

    // A variable cell without copying a value (an array by name is not copied for a
    // single element or size).
    const value* peek(node_id n, const scope_ptr& env) {
        if (!db.is<n_ident>(n)) return nullptr;
        const value* c = env->find(db.as<n_ident>(n).name);
        return c && !c->is<thunk_v>() ? c : nullptr;
    }

    flow eval_node(node_id n, const n_index& i, const scope_ptr& env, uint32_t frame) {
        span where = db[n].where;
        note_index_proof(n, env);
        if (const value* arr = peek(i.object, env); arr && arr->is<array_v>()) {
            TZ_TRY(idx, eval(i.index, env, frame));
            if (idx.known()) return ok(arr->as<array_v>().get(checked_index(*arr, idx, where)));
        }
        TZ_TRY(obj, eval(i.object, env, frame));
        TZ_TRY(idx, eval(i.index, env, frame));
        return ok(lift(obj, [&](const value& o) {
            if (o.is<rt_v>() && idx.known()) { // tuple by compile-time - the exact type of element
                const value* shape = shape_of(o);
                if (shape && shape->is<array_v>() && is_tuple(shape->as<array_v>()))
                    return erase(shape->as<array_v>().get(checked_index(*shape, idx, where)));
            }
            if (o.is<rt_v>() || !idx.known()) return element_type(o, where);
            return o.as<array_v>().get(checked_index(o, idx, where));
        }));
    }

    // Type of element according to the run-time index. tuple - splitting by element
    // type: the sequel is understood for each.
    value element_type(const value& arr, span where) {
        if (unknown(arr)) return arr;
        const value* shape = shape_of(arr);
        if (!shape || !shape->is<array_v>()) fail(std::format("индексация значения типа {}", types.describe(arr)), where);
        const auto& a = shape->as<array_v>();
        if (a.empty()) return value(rt_v{});
        value t = erase(a.get(0));
        if (is_tuple(a))
            for (uint32_t i = 1; i < a.size(); ++i) t = merge(t, erase(a.get(i)));
        return t;
    }

    // Proven index of the array element: OutOfBounds (§12.1) - runtime - error.
    uint32_t checked_index(const value& arr, const value& idx, span where) {
        if (!arr.is<array_v>()) fail(std::format("индексация значения типа {}", types.describe(arr)), where);
        if (!idx.is<int64_t>()) fail("индекс должен быть i64", where);
        int64_t k = idx.as<int64_t>();
        uint32_t n = arr.as<array_v>().size();
        if (k < 0 || uint64_t(k) >= n) fail(std::format("индекс {} вне границ массива размера {}", k, n), where, diag_kind::runtime);
        return uint32_t(k);
    }

    flow eval_node(node_id n, const n_hole& h, const scope_ptr&, uint32_t) {
        fail(std::format("исполнение неразобранного фрагмента: {}", h.reason), db[n].where);
    }

    flow eval_node(node_id n, const auto&, const scope_ptr&, uint32_t) {
        fail(std::format("узел {} нельзя исполнить", kind_name(db[n])), db[n].where);
    }

    // --- seats (lvalue) ---

    value* place(node_id n, const scope_ptr& env, uint32_t frame) {
        if (db.is<n_ident>(n)) {
            if (value* c = env->find(db.as<n_ident>(n).name)) return c;
            fail(std::format("неизвестная переменная {}", str(db.as<n_ident>(n).name)), db[n].where);
        }
        if (db.is<n_index>(n)) { // cell of the element is only in a common array
            const auto& i   = db.as<n_index>(n);
            value*      arr = place(i.object, env, frame);
            flow        idx = eval(i.index, env, frame);
            if (!arr || arr->is<rt_v>() || !idx.v.known()) return nullptr;
            return arr->as<array_v>().generic_cell(checked_index(*arr, idx.v, db[n].where));
        }
        return nullptr;
    }

    // --- operator ---

    flow eval_node(node_id n, const n_op& o, const scope_ptr& env, uint32_t frame) {
        auto op    = str(o.op);
        span where = db[n].where;
        if (op == "?") return call_named(sym("try"), o.args, env, frame, where, n.value);
        if (o.args.size() == 1) {
            TZ_TRY(x, eval(o.args[0], env, frame));
            return ok(lift(x, [&](const value& a) { return unary(op, a, where); }));
        }
        if (op == "&&" || op == "||") {
            TZ_TRY(a, eval(o.args[0], env, frame));
            auto av = known_bool(a, db[o.args[0]].where);
            if (av && (op == "&&" ? !*av : *av)) return ok(value(*av));
            TZ_TRY(b, eval(o.args[1], env, frame));
            auto bv = known_bool(b, db[o.args[1]].where);
            if (av && bv) return ok(value(*bv));
            return ok(erase(value(true)));
        }
        if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") {
            TZ_TRY(rhs, eval(o.args[1], env, frame));
            if (db.is<n_index>(o.args[0])) note_index_proof(o.args[0], env, true);
            if (!checking() && db.is<n_index>(o.args[0])) { // entry into an element: set, not cell
                const auto& ix  = db.as<n_index>(o.args[0]);
                value*      arr = place(ix.object, env, frame);
                if (!arr) fail("присваивание не в переменную", db[o.args[0]].where);
                TZ_TRY(idx, eval(ix.index, env, frame));
                uint32_t   k   = checked_index(*arr, idx, db[o.args[0]].where);
                value cur = arr->as<array_v>().get(k);
                arr->as<array_v>().set(k, op == "=" ? rhs : binary(std::string_view(op).substr(0, 1), cur, rhs, where));
                return ok();
            }
            value* target = place(o.args[0], env, frame);
            if (!target) {
                if (checking() && db.is<n_index>(o.args[0])) { // entry into the run-time-array element: type reconciliation
                    TZ_TRY(cur, eval(o.args[0], env, frame));
                    value res = op == "=" ? rhs : binary(std::string_view(op).substr(0, 1), cur, rhs, where);
                    if (!unknown(cur) && !unknown(res) && types.describe(cur) != types.describe(res))
                        fail(std::format("элемент массива типа {} нельзя заменить значением типа {}", types.describe(cur),
                                         types.describe(res)),
                             where);
                    return ok();
                }
                fail("присваивание не в переменную", db[o.args[0]].where);
            }
            if (op == "=") *target = std::move(rhs);
            else *target = binary(std::string_view(op).substr(0, 1), *target, rhs, where);
            return ok();
        }
        TZ_TRY(a, eval(o.args[0], env, frame));
        TZ_TRY(b, eval(o.args[1], env, frame));
        return ok(binary(op, a, b, where));
    }

    value unary(std::string_view op, const value& x, span where) {
        if (unknown(x)) return x;
        if (!x.known()) return erase(unary(op, *shape_or_fail(x, where), where));
        if (op == "!") return value(!truthy(x, where));
        if (op == "-") {
            if (x.is<int64_t>()) return value(int64_t(0 - uint64_t(x.as<int64_t>())));
            if (x.is<double>()) return value(-x.as<double>());
        }
        fail(std::format("оператор {} не определён для {}", op, types.describe(x)), where);
    }

    const value* shape_or_fail(const value& x, span where) {
        const value* s = shape_of(x);
        if (!s) fail("операция над значением неизвестного типа", where);
        return s;
    }

    static bool unknown(const value& x) { return x.is<rt_v>() && !x.as<rt_v>().shape; }

    // Binary operation; with run-time-operand, the result type is calculated on the samples.
    value binary(std::string_view op, const value& a, const value& b, span where) {
        if (a.is<split_v>()) return lift(a, [&](const value& x) { return binary(op, x, b, where); });
        if (b.is<split_v>()) return lift(b, [&](const value& y) { return binary(op, a, y, where); });
        if (unknown(a) || unknown(b)) return value(rt_v{}); // type is still being removed (recursion)
        if (!a.known() || !b.known())
            return erase(arith(op, *shape_or_fail(erase(a), where), *shape_or_fail(erase(b), where), where));
        return arith(op, a, b, where);
    }

    value arith(std::string_view op, const value& a, const value& b, span where) {
        // Strictly: without implicit i64 <-> f64 (§5.8)
        if (a.is<int64_t>() && b.is<int64_t>()) return int_op(op, a.as<int64_t>(), b.as<int64_t>(), where);
        if (a.is<double>() && b.is<double>()) return float_op(op, a.as<double>(), b.as<double>(), where);
        if (a.is<std::string>() && b.is<std::string>()) {
            if (op == "+") return value(a.as<std::string>() + b.as<std::string>());
            if (op == "==") return value(a.as<std::string>() == b.as<std::string>());
            if (op == "!=") return value(a.as<std::string>() != b.as<std::string>());
        }
        if (a.is<bool>() && b.is<bool>()) {
            if (op == "==") return value(a.as<bool>() == b.as<bool>());
            if (op == "!=") return value(a.as<bool>() != b.as<bool>());
        }
        fail(std::format("оператор {} не определён для {} и {}", op, types.describe(a), types.describe(b)), where);
    }

    value int_op(std::string_view op, int64_t x, int64_t y, span where) {
        auto w = [](uint64_t r) { return value(int64_t(r)); }; // Modular arithmetic (§5.8)
        if (op == "+") return w(uint64_t(x) + uint64_t(y));
        if (op == "-") return w(uint64_t(x) - uint64_t(y));
        if (op == "*") return w(uint64_t(x) * uint64_t(y));
        if (op == "/" || op == "%") {
            if (y == 0) fail("деление на ноль", where, diag_kind::runtime); // certain exception, not UB
            return value(op == "/" ? x / y : x % y);
        }
        return compare(op, x, y, where);
    }

    value float_op(std::string_view op, double x, double y, span where) {
        if (op == "+") return value(x + y);
        if (op == "-") return value(x - y);
        if (op == "*") return value(x * y);
        if (op == "/") return value(x / y);
        return compare(op, x, y, where);
    }

    value compare(std::string_view op, auto x, decltype(x) y, span where) {
        if (op == "==") return value(x == y);
        if (op == "!=") return value(x != y);
        if (op == "<") return value(x < y);
        if (op == ">") return value(x > y);
        if (op == "<=") return value(x <= y);
        if (op == ">=") return value(x >= y);
        fail(std::format("неизвестный оператор {}", op), where);
    }

    void check_type(const value& v, symbol tname, span where) {
        auto want = str(tname);
        if (want == "auto") return;
        auto check1 = [&](const value& x) {
            if (types.describe(x) != want && types.name(types.of(x)) != want)
                fail(std::format("ожидался тип {}, получен {}", want, types.describe(x)), where);
        };
        if (v.is<split_v>()) for (const auto& a : v.as<split_v>().alts) check1(a);
        else check1(v);
    }

    // --- challenge ---

    flow eval_node(node_id n, const n_call& c, const scope_ptr& env, uint32_t frame) {
        span where = db[n].where;
        if (db.is<n_member>(c.callee)) { // a.f(b) ≡ f(a, b) (§4.5)
            const auto& m = db.as<n_member>(c.callee);
            node_list   args{m.object};
            args.insert(args.end(), c.args.begin(), c.args.end());
            return call_named(m.name, args, env, frame, where, n.value);
        }
        if (db.is<n_ident>(c.callee)) {
            symbol name = db.as<n_ident>(c.callee).name;
            if (value* cell = env->find(name)) {
                TZ_TRY(fv, read(cell));
                return call_value(fv, c.args, env, frame, where, n.value);
            }
            return call_named(name, c.args, env, frame, where, n.value);
        }
        TZ_TRY(fv, eval(c.callee, env, frame));
        return call_value(fv, c.args, env, frame, where, n.value);
    }

    flow call_value(const value& fv, const node_list& args, const scope_ptr& env, uint32_t frame, span where, uint32_t node = 0) {
        if (!fv.is<fn_v>()) fail(std::format("значение типа {} нельзя вызвать", types.describe(fv)), where);
        const fn_v& f = fv.as<fn_v>();
        if (f.name) return call_named(f.name, args, env, frame, where, node);
        const node_list& params = db.is<n_lambda>(f.lambda) ? db.as<n_lambda>(f.lambda).params : db.as<n_fn>(f.lambda).params;
        node_id          body   = db.is<n_lambda>(f.lambda) ? db.as<n_lambda>(f.lambda).body : db.as<n_fn>(f.lambda).body;
        if (params.size() != args.size()) fail(std::format("ожидалось {} аргументов, передано {}", params.size(), args.size()), where);
        auto inner    = std::make_shared<scope>();
        inner->parent = f.env;
        for (uint32_t i = 0; i < params.size(); ++i) {
            const auto& p = db.as<n_param>(params[i]);
            if (p.mode == param_mode::expr) { inner->declare(p.name, value(thunk_v{args[i], env, frame, ctx})); continue; }
            if (value* cell = place_or_null(args[i], env)) { inner->alias(p.name, cell); continue; }
            TZ_TRY(v, eval(args[i], env, frame));
            inner->declare(p.name, std::move(v));
        }
        return run_frame(body, inner, db.is<n_fn>(f.lambda) ? db.as<n_fn>(f.lambda).name : symbol{}, false);
    }

    // Execute body function in a new frame. The result of the verification mode is a
    // merger of the usual result and all possible returns over run-time branches.
    flow run_frame(node_id body, const scope_ptr& inner, symbol name, bool outline) {
        uint32_t me = ++frame_counter;
        frames.push_back({me, name, outline, false});
        flow r = eval(body, inner, me);
        bool effect = frames.back().rt_effect;
        frames.pop_back();
        if (!frames.empty()) frames.back().rt_effect |= effect;
        last_rt_effect = effect;

        if (r.kind == flow_kind::ret && r.target == me) r = ok(std::move(r.v));
        if (auto it = may_return.find(me); it != may_return.end()) {
            value merged = r.normal() ? r.v : it->second.front();
            for (const auto& x : it->second) merged = merge(merged, x);
            may_return.erase(it);
            if (r.normal()) r.v = merged;
            else if (r.kind == flow_kind::ret) record_return(r), r = ok(merged); // path
        }
        return r;
    }

    flow call_named(symbol name, const node_list& args, const scope_ptr& env, uint32_t frame, span where, uint32_t node = 0) {
        if (auto it = functions.find(name); it != functions.end())
            return call_overloaded(it->second, args, env, frame, {where, std::string(str(name)), node});
        std::vector<value> vals;
        for (node_id a : args) {
            TZ_TRY(v, eval(a, env, frame));
            vals.push_back(std::move(v));
        }
        return ok(builtin_lifted(name, vals, 0, where));
    }

    // Fragmented Built-in Argument: A Challenge to Each Alternative
    value builtin_lifted(symbol name, std::vector<value>& vals, uint32_t from, span where) {
        for (uint32_t i = from; i < vals.size(); ++i) {
            if (!vals[i].is<split_v>()) continue;
            value orig = vals[i], out;
            bool  first = true;
            for (const auto& alt : orig.as<split_v>().alts) {
                vals[i] = alt;
                value r = builtin_lifted(name, vals, i + 1, where);
                out     = first ? r : merge(out, r);
                first   = false;
            }
            vals[i] = orig;
            return out;
        }
        return builtin(name, vals, where);
    }

    // overload: Candidates by arity and types; require at the beginning of the body -
    // candidate restriction (§5.4); choose the most specific.
    flow call_overloaded(const std::vector<node_id>& fns, const node_list& args, const scope_ptr& env, uint32_t frame, call_site site) {
        std::vector<value> vals(args.size());
        for (uint32_t i = 0; i < args.size(); ++i) {
            bool expr_everywhere = true;
            for (node_id f : fns) {
                const auto& ps = db.as<n_fn>(f).params;
                if (i >= ps.size() || db.as<n_param>(ps[i]).mode != param_mode::expr) expr_everywhere = false;
            }
            if (expr_everywhere) vals[i] = value(thunk_v{args[i], env, frame, ctx});
            else if (value* cell = checking() ? nullptr : place_or_null(args[i], env); cell && cell->is<array_v>())
                vals[i] = value(array_v::borrow(cell->as<array_v>())); // place-link, not copy
            else {
                TZ_TRY(v, eval(args[i], env, frame));
                vals[i] = std::move(v);
            }
        }
        return dispatch_split(fns, vals, 0, args, env, frame, site);
    }

    // The split argument: Continuation-challenge is cloned by alternatives.
    flow dispatch_split(const std::vector<node_id>& fns, std::vector<value>& vals, uint32_t from, const node_list& args,
                        const scope_ptr& env, uint32_t frame, const call_site& site) {
        for (uint32_t i = from; i < vals.size(); ++i) {
            if (!vals[i].is<split_v>()) continue;
            value orig = vals[i];
            flow  out;
            bool  first = true;
            for (const auto& alt : orig.as<split_v>().alts) {
                vals[i]   = alt;
                auto copy = vals;
                flow r    = dispatch_split(fns, copy, i + 1, args, env, frame, site);
                out       = first ? r : join_branches(out, r);
                first     = false;
            }
            vals[i] = orig;
            return out;
        }
        return select_and_invoke(fns, vals, args, env, frame, site);
    }

    flow select_and_invoke(const std::vector<node_id>& fns, std::vector<value>& vals, const node_list& args,
                           const scope_ptr& env, uint32_t frame, const call_site& site) {
        struct candidate {
            node_id fn;
            int     score;
        };
        std::vector<candidate>   good;
        std::vector<std::string> rejected;
        for (node_id f : fns) {
            const auto& fn = db.as<n_fn>(f);
            if (fn.params.size() != args.size()) {
                rejected.push_back(std::format("{}: ожидает {} аргументов", describe_fn(f), fn.params.size()));
                continue;
            }
            int         score = 0;
            std::string why;
            for (uint32_t i = 0; i < fn.params.size() && why.empty(); ++i) {
                const auto& p = db.as<n_param>(fn.params[i]);
                if (p.type && db.is<n_ident>(p.type)) {
                    auto want = str(db.as<n_ident>(p.type).name);
                    if (types.describe(vals[i]) != want && types.name(types.of(vals[i])) != want)
                        why = std::format("параметр {} ожидает {}, передано {}", str(p.name), want, types.describe(vals[i]));
                    else ++score;
                }
            }
            if (why.empty()) {
                auto [passed, n_req, reason] = leading_requires(f, vals, args, env);
                if (!passed) why = reason;
                score += n_req;
            }
            if (why.empty()) good.push_back({f, score});
            else rejected.push_back(std::format("{}: {}", describe_fn(f), why));
        }

        if (good.empty()) {
            diagnostic d(std::format("нет подходящей перегрузки {}", site.what), site.where);
            for (auto& r : rejected) d.note(std::format("кандидат {}", r), site.where);
            if (!path.empty()) d.note(std::format("в продолжении {}", path), site.where);
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(std::format("в {}", it->what), it->where);
            throw d;
        }
        std::ranges::sort(good, std::greater{}, &candidate::score);
        if (good.size() > 1 && good[0].score == good[1].score) {
            diagnostic d(std::format("неоднозначный вызов {}", site.what), site.where);
            for (auto& c : good)
                if (c.score == good[0].score) d.note(std::format("кандидат {}", describe_fn(c.fn)), db[c.fn].where);
            throw d;
        }
        return invoke(good[0].fn, vals, args, env, site);
    }

    // require at the beginning of the body: run-time - Candidate condition does not
    // cut off - it is checked in runtime (§5.4).
    std::tuple<bool, int, std::string> leading_requires(node_id f, std::vector<value>& vals, const node_list& args, const scope_ptr& env) {
        const auto& fn = db.as<n_fn>(f);
        if (fn.expr_body || !db.is<n_block>(fn.body)) return {true, 0, ""};
        auto probe    = std::make_shared<scope>();
        probe->parent = globals;
        bind_values(fn.params, vals, args, env, probe);
        int n = 0;
        for (node_id s : db.as<n_block>(fn.body).items) {
            if (!db.is<n_require>(s)) break;
            ++n;
            flow c = eval(db.as<n_require>(s).cond, probe, 0);
            if (c.v.is<bool>() && !c.v.as<bool>()) return {false, n, "require не выполнено"};
            if (!c.v.is<bool>() && c.v.known()) return {false, n, "require не bool"};
        }
        return {true, n, ""};
    }

    flow invoke(node_id f, std::vector<value>& vals, const node_list& args, const scope_ptr& env, const call_site& site) {
        const auto& fn = db.as<n_fn>(f);
        if (fn.outline && inline_required(f))
            fail(std::format("{} помечена outline, но использует семантику инлайна", site.what), db[f].where);
        if (checking() && param_writes_checked.insert(f.value).second)
            for (uint32_t k = 0; k < fn.params.size(); ++k)
                if (db.as<n_param>(fn.params[k]).mode != param_mode::expr && writes_param(f, k))
                    param_writes.push_back(std::format("{}: меняет {}", describe_fn(f), str(db.as<n_param>(fn.params[k]).name)));

        // instance key : only runtime is part of the argument
        bool        all_known = true;
        std::string key       = std::format("{}(", str(fn.name));
        for (uint32_t i = 0; i < vals.size(); ++i) {
            if (i) key += ", ";
            if (vals[i].is<thunk_v>()) key += "expr";
            else if (!checking()) key += types.describe(vals[i]);
            else if (vals[i].known()) key += "кт";
            else { key += "рт " + types.describe(vals[i]); all_known = false; }
        }
        key += ")";

        // Recursion in static parsing. According to known values, the usual execution:
        // “recursion stops the condition”; the activity key includes CT values. By mouth
        // values is a fixed point: the recursive call receives the current approximation
        // of the type of result, the body is disassembled again until the type ceases to
        // change.
//).
        std::string active_key = key;
        if (checking()) {
            active_key += "|";
            for (auto& v : vals) active_key += v.known() && !v.is<thunk_v>() ? show_v(v) + "|" : "·|";
            if (active.contains(active_key)) {
                recursed.insert(active_key);
                auto it = approx.find(active_key);
                return ok(it != approx.end() ? it->second : value(rt_v{}));
            }
            if (frames.size() > 2000) fail("рекурсия не останавливается при статическом разборе", site.where);
        }

        // Reactive cache: instance with unchanged dependencies is not rediscovered
        std::string memo_key  = active_key + "#" + path;
        bool        memoize   = checking() && cache && approx.empty() && memoizable(f, vals);
        if (memoize) {
            if (auto it = cache->entries.find(memo_key); it != cache->entries.end() && valid(it->second)) {
                ++cache->hits;
                for (const auto& ev : it->second.events) emit(ev);
                for (const auto& [node, v, seen, div, eff] : it->second.journal) journal.push_back({node, v, seen, div, eff, ctx});
                observations_dirty = true;
                if (it->second.effect) { ++effects; if (!frames.empty()) frames.back().rt_effect = true; }
                last_rt_effect = it->second.effect;
                if (!deps.empty()) deps.back().insert(it->second.deps.begin(), it->second.deps.end());
                return ok(it->second.result);
            }
        }
        std::vector<report_event> recorded;
        if (memoize) { recording.push_back(&recorded); deps.emplace_back(); }
        if (checking() && !deps.empty()) deps.back()[f.value] = db.node_revision(f);

        if (jit && !checking()) { // JIT: machine code instance, if any
            if (auto r = try_jit(f, vals, args, env, key)) return *r;
        }

        stack.push_back(site);
        uint64_t ctx_saved = std::exchange(ctx, inline_context(ctx, site.node)); // body in its context of embedding
        std::size_t journal_mark  = journal.size();
        frame_floors.push_back({frame_counter, false});
        flow r;
        for (uint32_t pass = 0;; ++pass) {
            auto inner    = std::make_shared<scope>();
            inner->parent = globals;
            bind_values(fn.params, vals, args, env, inner);
            if (checking()) active.insert(active_key);
            try {
                r = run_frame(fn.body, inner, fn.name, fn.outline);
            } catch (...) {
                stack.pop_back();
                ctx = ctx_saved;
                frame_floors.pop_back();
                if (memoize) { recording.pop_back(); deps.pop_back(); }
                if (checking()) { active.erase(active_key); approx.erase(active_key); recursed.erase(active_key); }
                throw;
            }
            if (checking()) active.erase(active_key);
            if (!checking() || !recursed.contains(active_key) || !r.normal()) break;
            value prev = approx.contains(active_key) ? approx[active_key] : value(rt_v{});
            value next = erase(merge(prev, r.v));
            if (types.describe(next) == types.describe(prev) && !unknown(next)) break;
            if (pass > 8) fail(std::format("тип результата рекурсивной {} не сходится: {}", str(fn.name), types.describe(next)), site.where);
            approx[active_key] = next;
        }
        stack.pop_back();
        ctx = ctx_saved;
        if (checking()) { approx.erase(active_key); recursed.erase(active_key); }

        // curled up in compile-time: all arguments and result known, runtime no effects
        bool folded_now = checking() && all_known && r.normal() && r.v.known() && !last_rt_effect;
        if (folded_now) {
            emit({report_event::folded, std::string(str(fn.name)), {}, {}, {}, {}});
            journal.resize(journal_mark); // There is no folded call in runtime.
            observations_dirty = true;
        } else {
            emit({report_event::instance, path.empty() ? key : std::format("{}   {}", key, path), {}, {}, {}, {}});
            emit({report_event::live, {}, {}, {}, {}, {}, f.value}); // I need runtime.
        }
        bool outer_return = frame_floors.back().second;
        frame_floors.pop_back();
        if (memoize) {
            recording.pop_back();
            auto mine = std::move(deps.back());
            deps.pop_back();
            if (!deps.empty()) deps.back().insert(mine.begin(), mine.end());
            mine[mod.value] = db.node_revision(mod); // set of functions (overload)
            if (r.normal() && approx.empty() && !outer_return) {
                ++cache->recomputed;
                cache->recomputed_names.push_back(std::string(str(fn.name)));
                check_cache::entry e{r.v, std::move(mine), std::move(recorded), {}, last_rt_effect};
                for (std::size_t i = journal_mark; i < journal.size(); ++i)
                    e.journal.emplace_back(journal[i].node, journal[i].v, journal[i].seen, journal[i].diverted, journal[i].effect);
                cache->entries[memo_key] = std::move(e);
            }
        }
        return r; // return, addressed to the caller (semantic inlining), goes further
    }

    std::optional<flow> try_jit(node_id f, std::vector<value>& vals, const node_list& args, const scope_ptr& env,
                                const std::string& key);

    bool valid(const check_cache::entry& e) const {
        for (const auto& [node, rev] : e.deps)
            if (db.node_revision(node_id(node)) != rev) return false;
        return true;
    }

    // Parameters: alias per caller cell, if the argument is a place; otherwise its own cell.
    void bind_values(const node_list& params, std::vector<value>& vals, const node_list& args, const scope_ptr& env, const scope_ptr& inner) {
        for (uint32_t i = 0; i < params.size(); ++i) {
            const auto& p = db.as<n_param>(params[i]);
            if (p.mode != param_mode::expr && p.mode != param_mode::own && !vals[i].is<split_v>()) {
                if (value* cell = place_or_null(args[i], env); cell && !cell->is<split_v>()) {
                    inner->alias(p.name, cell);
                    continue;
                }
            }
            inner->declare(p.name, vals[i]);
        }
    }

    value* place_or_null(node_id n, const scope_ptr& env) {
        if (!db.is<n_ident>(n)) return nullptr;
        value* c = env->find(db.as<n_ident>(n).name);
        return c && !c->is<thunk_v>() ? c : nullptr;
    }

    // The function uses inline semantics (§6.1): expr-parameter or address return.
    bool inline_required(node_id f) const {
        for (node_id p : db.as<n_fn>(f).params)
            if (db.as<n_param>(p).mode == param_mode::expr) return true;
        return has_addressed_return(db.as<n_fn>(f).body);
    }

    bool has_addressed_return(node_id n) const {
        if (!n) return false;
        if (db.is<n_return>(n) && db.as<n_return>(n).target) return true;
        if (db.is<n_lambda>(n) || db.is<n_fn>(n)) return false;
        bool found = false;
        db.for_each_child(n, [&](node_id c) { found = found || has_addressed_return(c); });
        return found;
    }

    std::string describe_fn(node_id f) const {
        const auto& fn = db.as<n_fn>(f);
        std::string s  = std::format("{}(", str(fn.name));
        for (uint32_t i = 0; i < fn.params.size(); ++i) {
            if (i) s += ", ";
            const auto& p = db.as<n_param>(fn.params[i]);
            if (p.type && db.is<n_ident>(p.type)) s += std::format("{} ", str(db.as<n_ident>(p.type).name));
            s += str(p.name);
        }
        return s + ")";
    }

    // --- embedded functions (M0; later VM and stdlib services in language) ---

    void rt_effect() {
        ++effects;
        if (!frames.empty()) frames.back().rt_effect = true;
    }

    value builtin(symbol name, std::vector<value>& a, span where) {
        auto n = str(name);
        if (n == "print") {
            rt_effect();
            if (checking()) return {};
            std::string line;
            for (uint32_t i = 0; i < a.size(); ++i) line += (i ? " " : "") + show_v(a[i]);
            if (captured) *captured += line + "\n";
            else std::println("{}", line);
            return {};
        }
        if (n == "typeof" && a.size() == 1) { // Value Approach: Type is also known for run-time
            if (a[0].is<rt_v>() && !a[0].as<rt_v>().shape) fail("тип неизвестен", where);
            return value(type_v{types.of(a[0])});
        }
        if (n == "zero" && a.size() == 1 && a[0].is<type_v>()) {
            auto t = types.name(a[0].as<type_v>().type);
            if (t == "i64") return value(int64_t(0));
            if (t == "f64") return value(double(0));
            fail(std::format("zero не определён для {}", t), where);
        }
        if (n == "range" && a.size() == 1) {
            if (!a[0].known() && types.describe(a[0]) == "i64") return erase(value(array_v::of({value(int64_t(0))})));
            if (a[0].is<int64_t>()) {
                array_v r;
                for (int64_t i = 0; i < a[0].as<int64_t>(); ++i) r.push(value(i));
                return value(std::move(r));
            }
        }
        if (n == "fill" && a.size() == 2 && types.describe(a[0]) == "i64") { // massif
            if (!a[0].known() || !a[1].known()) return erase(value(array_v::of({a[1].known() ? a[1] : *shape_of(a[1])})));
            array_v r;
            for (int64_t i = 0; i < a[0].as<int64_t>(); ++i) r.push(a[1]);
            return value(std::move(r));
        }
        if (n == "f64" && a.size() == 1 && types.describe(a[0]) == "i64")
            return a[0].known() ? value(double(a[0].as<int64_t>())) : erase(value(double(1)));
        if (n == "i64" && a.size() == 1 && types.describe(a[0]) == "f64")
            return a[0].known() ? value(int64_t(a[0].as<double>())) : erase(value(int64_t(1)));
        if (n == "read_floats") {
            rt_effect();
            std::string line;
            bool        have = false;
            if (checking() && (!inputs || inputs->stdin_lines.empty()))
                return erase(value(array_v::of({value(double(1))})));
            if (inputs) { // Entry is set (statically known or submitted by session)
                if (!inputs->stdin_lines.empty()) {
                    line = inputs->stdin_lines.front();
                    inputs->stdin_lines.erase(inputs->stdin_lines.begin());
                }
                have = true;
            }
            array_v r;
            if (have || std::getline(std::cin, line)) {
                std::stringstream ss(line);
                double               x;
                while (ss >> x) r.push(value(x));
            }
            return value(std::move(r));
        }
        if (n == "parse_int" && a.size() == 1 && types.describe(a[0]) == "str") {
            auto make = [&](bool good, int64_t v, std::string err) {
                agg_v r{types.named("result"), {}};
                r.fields.push_back({sym("ok"), value(good)});
                r.fields.push_back({sym("value"), value(v)});
                r.fields.push_back({sym("error"), value(std::move(err))});
                return value(std::move(r));
            };
            if (!a[0].known()) return erase(make(true, 0, ""));
            const auto& s    = a[0].as<std::string>();
            int64_t         v    = 0;
            bool        good = !s.empty();
            for (char ch : s) {
                if (ch < '0' || ch > '9') { good = false; break; }
                v = v * 10 + (ch - '0');
            }
            return make(good, good ? v : 0, good ? "" : std::format("не число: {:?}", s));
        }
        if (n == "is_err" && a.size() == 1 && types.describe(a[0]) == "result") {
            value okf = member(a[0], sym("ok"), where);
            return okf.known() ? value(!okf.as<bool>()) : erase(value(true));
        }
        fail(std::format("неизвестная функция {}({})", n, [&] {
            std::string s;
            for (uint32_t i = 0; i < a.size(); ++i) s += (i ? ", " : "") + types.describe(a[i]);
            return s;
        }()), where);
    }

#undef TZ_TRY

    // --- condition ---

    struct frame_rec {
        uint32_t    id;
        symbol name;
        bool   outline;
        bool   rt_effect; // in the frame was runtime - effect (I/O) - the call does not fold
    };

    struct stage_rec {
        span        where;
        std::string path, name, what;
    };

    struct split_rec {
        span                     where;
        std::string              name;
        std::vector<std::string> alts;
    };

    ast_db&                                 db;
    node_id                                 mod;
    mode                                    run_mode;
    scope_ptr                               globals;
    std::map<symbol, std::vector<node_id>>  functions;
    type_registry                           types;
    std::vector<call_site>                  stack;
    std::vector<frame_rec>                  frames;
    uint32_t                                     frame_counter = 0;
    bool                                    last_rt_effect = false;
    std::map<uint32_t, std::vector<value>>       may_return;
    std::map<std::string, uint32_t>              instance_counts;
    std::map<std::string, uint32_t>              folded;
    std::set<std::string>                   active;
    std::set<std::string>                   recursed;
    std::map<std::string, value>            approx;

    struct excluded_rec {
        span        where;
        std::string path, why;
    };
    std::vector<excluded_rec>               excluded;
    std::vector<stage_rec>                  stages;
    std::vector<split_rec>                  splits;
    std::string                             path; // current continuation of splitting
    std::string*                            captured = nullptr;
    check_cache*                            cache = nullptr;
    struct jit_engine*                       jit   = nullptr;
    std::optional<uint64_t>                      fuel;
    std::vector<std::pair<uint32_t, bool>>       frame_floors; // (The first frame of the call was a return to the outside)
    std::vector<std::vector<report_event>*> recording;
    std::vector<std::map<uint32_t, uint64_t>>         deps;
    std::optional<known_inputs>             inputs;
    std::vector<obs_entry>                  journal;
    mutable std::unordered_map<uint32_t, observation> observations;
    mutable std::map<std::pair<uint32_t, uint64_t>, observation> observations_ctx; // insertion
    uint64_t                                     ctx = 0;          // current context
    mutable bool                            observations_dirty = false;
    std::set<uint32_t>                           live_fns;
    std::set<uint32_t>                           param_writes_checked;
    std::vector<std::string>                param_writes; // What functions change the parameters
    mutable std::map<uint64_t, bool>             writes;
    mutable std::set<uint64_t>                   writes_open;
    mutable bool                            writes_guess_used = false;
    std::set<uint32_t>                           proven;      // node Indexations Proven within Boundaries
    std::vector<std::pair<span, std::string>> proven_list;
    std::vector<std::tuple<const value*, const value*, symbol>> index_proofs; // (cell variable cycle, cell and array name)
    uint64_t                                     effects = 0;
};

} // namespace tz
