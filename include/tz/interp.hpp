// Interpreter: Executes AST base node directly (decision 2026-09-27).
//
//;
//.
//
// What's come out of the mix here:
// semantic inlining (§6.1): expr parameters are uncomputed expressions.
// computed in the context of the caller; `return` within them comes from
// function with expr-parameter - inline_required and cannot
// be meaningful;
// - parameters - alias arguments of the caller (§6.2);
// overload with require at the beginning of the body as a candidate restriction (§5.4)
// strict numbers without implicit transformations i64 <-> f64 (§5.8)
// Table instance: (function, types of arguments) - visible through --instances.
#pragma once

#include "value.hpp"

#include <deque>
#include <sstream>
#include <functional>
#include <iostream>
#include <map>

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
};

using scope_ptr = std::shared_ptr<scope>;

class interpreter {
public:
    interpreter(ast_db& db, node_id module, std::vector<std::string> args) : db_(db), module_(module) {
        globals_ = std::make_shared<scope>();
        array_v a;
        for (auto& s : args) a.items.push_back(value(s));
        globals_->declare(sym("args"), value(std::move(a)));
        globals_->declare(sym("stdin"), value(std::string("<stdin>")));
        for (node_id item : db_.as<n_module>(module).items)
            if (db_.is<n_fn>(item)) functions_[db_.as<n_fn>(item).name].push_back(item);
    }

    // Launch main(). Returns the exit code.
    int run_main() {
        auto it = functions_.find(sym("main"));
        if (it == functions_.end()) throw diagnostic("нет функции main", {});
        call_site site{db_[module_].where, "main"};
        flow r = call_overloaded(it->second, {}, globals_, 0, site);
        return r.v.is<i64>() ? int(r.v.as<i64>()) : 0;
    }

    // All print output is in line (for tests), otherwise in stdout.
    void capture_output(std::string* to) { captured_ = to; }

    // instance: function + types of arguments → how many times is invoked
    std::string instances() const {
        std::string out;
        for (const auto& [key, count] : instances_) out += std::format("  {}  ×{}\n", key, count);
        return out;
    }

private:
    // --- flow ---

    enum class flow_kind : u8 { normal, ret, brk, cont };

    struct flow {
        flow_kind kind = flow_kind::normal;
        value     v;
        u32       target = 0; // a frame from which comes the return

        bool normal() const { return kind == flow_kind::normal; }
    };

    static flow ok(value v = {}) { return flow{flow_kind::normal, std::move(v), 0}; }

    struct call_site {
        span        where;
        std::string what;
    };

    // --- call-trace errors ---

    [[noreturn]] void fail(std::string msg, span where) {
        diagnostic d(std::move(msg), where);
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) d.note(std::format("в {}", it->what), it->where);
        throw d;
    }

    symbol           sym(std::string_view s) { return db_.strings.intern(s); }
    std::string_view str(symbol s) const { return db_.strings.text(s); }
    std::string      show_v(const value& x) const { return show(x, types_, db_.strings); }

    // --- calculation ---

    // Calculate the expression; control-flow (return/break) creeps up.
#define TZ_TRY(var, expr)                    \
    flow var##_f = (expr);                   \
    if (!var##_f.normal()) return var##_f;   \
    value var = std::move(var##_f.v)

    flow eval(node_id n, const scope_ptr& env, u32 frame) {
        return std::visit([&](const auto& k) { return eval_node(n, k, env, frame); }, db_[n].kind);
    }

    // The value of the variable; thunk (expr parameter) is calculated when mentioned.
    flow read(value* cell, span where) {
        if (cell->is<thunk_v>()) {
            thunk_v t = cell->as<thunk_v>();
            return eval(t.expr, t.env, t.frame);
        }
        return ok(*cell);
    }

    flow eval_node(node_id n, const n_int& x, const scope_ptr&, u32) { return ok(value(x.value)); }
    flow eval_node(node_id n, const n_float& x, const scope_ptr&, u32) { return ok(value(x.value)); }
    flow eval_node(node_id n, const n_str& x, const scope_ptr&, u32) { return ok(value(x.value)); }

    flow eval_node(node_id n, const n_ident& x, const scope_ptr& env, u32) {
        if (value* cell = env->find(x.name)) return read(cell, db_[n].where);
        if (str(x.name) == "true") return ok(value(true));
        if (str(x.name) == "false") return ok(value(false));
        if (auto t = types_.builtin(str(x.name))) return ok(value(type_v{t}));
        if (auto it = functions_.find(x.name); it != functions_.end()) {
            // function as value: prohibited for inline_required (§6.1)
            for (node_id f : it->second)
                if (inline_required(f))
                    fail(std::format("функция {} использует семантику инлайна (expr-параметр) и не может быть значением; "
                                     "вызывайте её напрямую",
                                     str(x.name)),
                         db_[n].where);
            return ok(value(fn_v{x.name, {}, nullptr}));
        }
        fail(std::format("неизвестное имя {}", str(x.name)), db_[n].where);
    }

    flow eval_node(node_id n, const n_block& b, const scope_ptr& env, u32 frame) {
        auto  inner = std::make_shared<scope>();
        inner->parent = env;
        value last;
        for (node_id s : b.items) {
            flow f = eval(s, inner, frame);
            if (!f.normal()) return f;
            last = std::move(f.v);
        }
        return ok(b.has_value ? std::move(last) : value());
    }

    flow eval_node(node_id n, const n_var& v, const scope_ptr& env, u32 frame) {
        TZ_TRY(init, eval(v.init, env, frame));
        if (v.type && db_.is<n_ident>(v.type)) check_type(init, db_.as<n_ident>(v.type).name, db_[v.init].where);
        env->declare(v.name, std::move(init));
        return ok();
    }

    flow eval_node(node_id n, const n_fn& f, const scope_ptr& env, u32) {
        // local function = lambda with a name
        env->declare(f.name, value(fn_v{{}, n, env}));
        return ok();
    }

    flow eval_node(node_id n, const n_return& r, const scope_ptr& env, u32 frame) {
        value v;
        if (r.value) {
            TZ_TRY(x, eval(r.value, env, frame));
            v = std::move(x);
        }
        u32 target = r.target ? return_target(db_.as<n_ident>(r.target).name, frame, db_[n].where) : frame;
        return flow{flow_kind::ret, std::move(v), target};
    }

    // return(f) is the output from the external function f in the call chain;
    // return(caller) is the output from the calling function.
// The outline boundary is not allowed: there is no inline.
    u32 return_target(symbol name, u32 frame, span where) {
        i64 here = -1;
        for (i64 i = i64(frames_stack_.size()) - 1; i >= 0; --i)
            if (frames_stack_[i].id == frame) { here = i; break; }
        if (here < 0) fail("return вне функции", where);
        bool to_caller = str(name) == "caller";
        for (i64 i = here - 1; i >= 0; --i) {
            if (frames_stack_[i + 1].outline)
                fail(std::format("return({}) через outline-функцию {}: у неё нет семантики инлайна", str(name),
                                 str(frames_stack_[i + 1].name)),
                     where);
            if (to_caller || frames_stack_[i].name == name) return frames_stack_[i].id;
        }
        fail(std::format("return({}): нет такой внешней функции в цепочке вызовов", str(name)), where);
    }

    flow eval_node(node_id n, const n_require& r, const scope_ptr& env, u32 frame) {
        TZ_TRY(c, eval(r.cond, env, frame));
        if (!truthy(c, db_[r.cond].where)) fail("нарушено требование require", db_[n].where);
        return ok();
    }

    flow eval_node(node_id n, const n_if& i, const scope_ptr& env, u32 frame) {
        TZ_TRY(c, eval(i.cond, env, frame));
        if (truthy(c, db_[i.cond].where)) return eval(i.then_, env, frame);
        if (i.else_) return eval(i.else_, env, frame);
        return ok();
    }

    flow eval_node(node_id n, const n_while& w, const scope_ptr& env, u32 frame) {
        for (;;) {
            TZ_TRY(c, eval(w.cond, env, frame));
            if (!truthy(c, db_[w.cond].where)) return ok();
            flow f = eval(w.body, env, frame);
            if (f.kind == flow_kind::brk) return ok();
            if (f.kind == flow_kind::ret) return f;
        }
    }

    flow eval_node(node_id n, const n_for& f, const scope_ptr& env, u32 frame) {
        TZ_TRY(r, eval(f.range, env, frame));
        if (!r.is<array_v>()) fail(std::format("for ожидает массив, получено {}", types_.describe(r)), db_[f.range].where);
        for (auto& item : r.as<array_v>().items) {
            auto inner = std::make_shared<scope>();
            inner->parent = env;
            inner->declare(f.var, item);
            flow b = eval(f.body, inner, frame);
            if (b.kind == flow_kind::brk) break;
            if (b.kind == flow_kind::ret) return b;
        }
        return ok();
    }

    flow eval_node(node_id n, const n_ct& c, const scope_ptr& env, u32 frame) {
        // M0: compile-time and runtime run by a single interpreter (§7.2);
        // verification will appear along with step analysis.
        return eval(c.body, env, frame);
    }

    flow eval_node(node_id n, const n_try_compile& t, const scope_ptr& env, u32 frame) {
        // M0: A block is deleted if no name or overload is found.
        try {
            return eval(t.body, env, frame);
        } catch (const diagnostic&) {
            return ok();
        }
    }

    flow eval_node(node_id n, const n_lambda& l, const scope_ptr& env, u32) { return ok(value(fn_v{{}, n, env})); }

    flow eval_node(node_id n, const n_construct& c, const scope_ptr& env, u32 frame) {
        array_v a;
        for (node_id item : c.items) {
            TZ_TRY(x, eval(item, env, frame));
            a.items.push_back(std::move(x));
        }
        auto tname = str(db_.as<n_ident>(c.type).name);
        if (tname != "array") fail(std::format("конструктор {}{{}} пока не поддержан", tname), db_[n].where);
        return ok(value(std::move(a)));
    }

    flow eval_node(node_id n, const n_member& m, const scope_ptr& env, u32 frame) {
        TZ_TRY(obj, eval(m.object, env, frame));
        return ok(member(obj, m.name, db_[n].where));
    }

    value member(const value& obj, symbol name, span where) {
        if (str(name) == "size") {
            if (obj.is<array_v>()) return value(i64(obj.as<array_v>().items.size()));
            if (obj.is<std::string>()) return value(i64(obj.as<std::string>().size()));
        }
        if (obj.is<agg_v>())
            for (auto& [k, v] : obj.as<agg_v>().fields)
                if (k == name) return v;
        fail(std::format("у значения типа {} нет поля {}", types_.describe(obj), str(name)), where);
    }

    flow eval_node(node_id n, const n_index& i, const scope_ptr& env, u32 frame) {
        TZ_TRY(obj, eval(i.object, env, frame));
        TZ_TRY(idx, eval(i.index, env, frame));
        return ok(*element(obj, idx, db_[n].where));
    }

    value* element(value& arr, const value& idx, span where) {
        if (!arr.is<array_v>()) fail(std::format("индексация значения типа {}", types_.describe(arr)), where);
        if (!idx.is<i64>()) fail("индекс должен быть i64", where);
        auto& items = arr.as<array_v>().items;
        i64   k     = idx.as<i64>();
        if (k < 0 || u64(k) >= items.size())
            fail(std::format("индекс {} вне границ массива размера {}", k, items.size()), where); // throw OutOfBounds (§12.1)
        return &items[k];
    }

    flow eval_node(node_id n, const n_hole& h, const scope_ptr&, u32) {
        fail(std::format("исполнение неразобранного фрагмента: {}", h.reason), db_[n].where);
    }

    template <class other>
    flow eval_node(node_id n, const other&, const scope_ptr&, u32) {
        fail(std::format("узел {} нельзя исполнить", kind_name(db_[n])), db_[n].where);
    }

    // --- Lvalue for assignment and alias ---

    value* place(node_id n, const scope_ptr& env, u32 frame) {
        if (db_.is<n_ident>(n)) {
            if (value* c = env->find(db_.as<n_ident>(n).name)) return c;
            fail(std::format("неизвестная переменная {}", str(db_.as<n_ident>(n).name)), db_[n].where);
        }
        if (db_.is<n_index>(n)) {
            const auto& i   = db_.as<n_index>(n);
            value*      arr = place(i.object, env, frame);
            flow        idx = eval(i.index, env, frame);
            return element(*arr, idx.v, db_[n].where);
        }
        return nullptr; // not the place: the time value
    }

    // --- operator ---

    flow eval_node(node_id n, const n_op& o, const scope_ptr& env, u32 frame) {
        auto op = str(o.op);
        if (op == "?") return call_named(sym("try"), o.args, env, frame, db_[n].where);
        if (o.args.size() == 1) {
            TZ_TRY(x, eval(o.args[0], env, frame));
            if (op == "!") return ok(value(!truthy(x, db_[n].where)));
            if (op == "-") {
                if (x.is<i64>()) return ok(value(-x.as<i64>()));
                if (x.is<f64>()) return ok(value(-x.as<f64>()));
            }
            fail(std::format("оператор {} не определён для {}", op, types_.describe(x)), db_[n].where);
        }
        if (op == "&&" || op == "||") {
            TZ_TRY(a, eval(o.args[0], env, frame));
            bool av = truthy(a, db_[o.args[0]].where);
            if (op == "&&" ? !av : av) return ok(value(av));
            TZ_TRY(b, eval(o.args[1], env, frame));
            return ok(value(truthy(b, db_[o.args[1]].where)));
        }
        if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") {
            TZ_TRY(rhs, eval(o.args[1], env, frame));
            value* target = place(o.args[0], env, frame);
            if (!target) fail("присваивание не в переменную", db_[o.args[0]].where);
            if (op == "=") *target = std::move(rhs);
            else *target = arith(std::string_view(op).substr(0, 1), *target, rhs, db_[n].where);
            return ok();
        }
        TZ_TRY(a, eval(o.args[0], env, frame));
        TZ_TRY(b, eval(o.args[1], env, frame));
        return ok(arith(op, a, b, db_[n].where));
    }

    value arith(std::string_view op, const value& a, const value& b, span where) {
        // strictly: without implicit i64 <-> f64 (§5.8)
        if (a.is<i64>() && b.is<i64>()) return int_op(op, a.as<i64>(), b.as<i64>(), where);
        if (a.is<f64>() && b.is<f64>()) return float_op(op, a.as<f64>(), b.as<f64>(), where);
        if (a.is<std::string>() && b.is<std::string>()) {
            if (op == "+") return value(a.as<std::string>() + b.as<std::string>());
            if (op == "==") return value(a.as<std::string>() == b.as<std::string>());
            if (op == "!=") return value(a.as<std::string>() != b.as<std::string>());
        }
        if (a.is<bool>() && b.is<bool>()) {
            if (op == "==") return value(a.as<bool>() == b.as<bool>());
            if (op == "!=") return value(a.as<bool>() != b.as<bool>());
        }
        fail(std::format("оператор {} не определён для {} и {}", op, types_.describe(a), types_.describe(b)), where);
    }

    value int_op(std::string_view op, i64 x, i64 y, span where) {
        // Modular arithmetic (§5.8): overflow defined
        auto w = [](u64 r) { return value(i64(r)); };
        if (op == "+") return w(u64(x) + u64(y));
        if (op == "-") return w(u64(x) - u64(y));
        if (op == "*") return w(u64(x) * u64(y));
        if (op == "/" || op == "%") {
            if (y == 0) fail("деление на ноль", where); // certain exception, not UB
            return value(op == "/" ? x / y : x % y);
        }
        return compare(op, x, y, where);
    }

    value float_op(std::string_view op, f64 x, f64 y, span where) {
        if (op == "+") return value(x + y);
        if (op == "-") return value(x - y);
        if (op == "*") return value(x * y);
        if (op == "/") return value(x / y);
        return compare(op, x, y, where);
    }

    template <class T>
    value compare(std::string_view op, T x, T y, span where) {
        if (op == "==") return value(x == y);
        if (op == "!=") return value(x != y);
        if (op == "<") return value(x < y);
        if (op == ">") return value(x > y);
        if (op == "<=") return value(x <= y);
        if (op == ">=") return value(x >= y);
        fail(std::format("неизвестный оператор {}", op), where);
    }

    bool truthy(const value& v, span where) {
        if (!v.is<bool>()) fail(std::format("условие должно быть bool, получено {}", types_.describe(v)), where);
        return v.as<bool>();
    }

    void check_type(const value& v, symbol tname, span where) {
        auto want = str(tname);
        if (want == "auto") return;
        if (types_.describe(v) != want && types_.name(types_.of(v)) != want)
            fail(std::format("ожидался тип {}, получен {}", want, types_.describe(v)), where);
    }

    // --- challenge ---

    flow eval_node(node_id n, const n_call& c, const scope_ptr& env, u32 frame) {
        span where = db_[n].where;
        // a.f(b) ) f(a, b) if a has no field f (§4.5 )
        if (db_.is<n_member>(c.callee)) {
            const auto& m = db_.as<n_member>(c.callee);
            node_list   args{m.object};
            args.insert(args.end(), c.args.begin(), c.args.end());
            return call_named(m.name, args, env, frame, where);
        }
        if (db_.is<n_ident>(c.callee)) {
            symbol name = db_.as<n_ident>(c.callee).name;
            if (value* cell = env->find(name)) { // variable
                TZ_TRY(fv, read(cell, where));
                return call_value(fv, c.args, env, frame, where);
            }
            return call_named(name, c.args, env, frame, where);
        }
        TZ_TRY(fv, eval(c.callee, env, frame));
        return call_value(fv, c.args, env, frame, where);
    }

    flow call_value(const value& fv, const node_list& args, const scope_ptr& env, u32 frame, span where) {
        if (!fv.is<fn_v>()) fail(std::format("значение типа {} нельзя вызвать", types_.describe(fv)), where);
        const fn_v& f = fv.as<fn_v>();
        if (f.name) return call_named(f.name, args, env, frame, where);
        // lambda/local function: parameters - alias, environment - captured
        const node_list& params = db_.is<n_lambda>(f.lambda) ? db_.as<n_lambda>(f.lambda).params : db_.as<n_fn>(f.lambda).params;
        node_id          body   = db_.is<n_lambda>(f.lambda) ? db_.as<n_lambda>(f.lambda).body : db_.as<n_fn>(f.lambda).body;
        if (params.size() != args.size()) fail(std::format("ожидалось {} аргументов, передано {}", params.size(), args.size()), where);
        auto inner    = std::make_shared<scope>();
        inner->parent = f.env;
        TZ_TRY(bound, bind(params, args, env, frame, inner));
        u32 me = ++frames_;
        frames_stack_.push_back({me, db_.is<n_fn>(f.lambda) ? db_.as<n_fn>(f.lambda).name : symbol{}, false});
        flow r = eval(body, inner, me);
        frames_stack_.pop_back();
        if (r.kind == flow_kind::ret && r.target == me) return ok(std::move(r.v));
        return r;
    }

    flow call_named(symbol name, const node_list& args, const scope_ptr& env, u32 frame, span where) {
        if (auto it = functions_.find(name); it != functions_.end())
            return call_overloaded(it->second, args, env, frame, {where, std::string(str(name))});
        // Built-in functions get calculated arguments
        std::vector<value> vals;
        for (node_id a : args) {
            TZ_TRY(v, eval(a, env, frame));
            vals.push_back(std::move(v));
        }
        return ok(builtin(name, vals, where));
    }

    // overload: candidates by arity and parameter types; require at the beginning of the
    // body - candidate restriction (§5.4); choose the most specific.
    flow call_overloaded(const std::vector<node_id>& fns, const node_list& args, const scope_ptr& env, u32 frame,
                         call_site site) {
        // Arguments are calculated once; thunk is transmitted for expr positions.
        std::vector<value> vals(args.size());
        std::vector<bool>  is_expr(args.size(), false);
        for (u32 i = 0; i < args.size(); ++i) {
            bool expr_everywhere = true;
            for (node_id f : fns) {
                const auto& ps = db_.as<n_fn>(f).params;
                if (i >= ps.size() || db_.as<n_param>(ps[i]).mode != param_mode::expr) expr_everywhere = false;
            }
            if (expr_everywhere) {
                vals[i]    = value(thunk_v{args[i], env, frame});
                is_expr[i] = true;
            } else {
                TZ_TRY(v, eval(args[i], env, frame));
                vals[i] = std::move(v);
            }
        }

        struct candidate {
            node_id fn;
            int     score;
        };
        std::vector<candidate> ok_candidates;
        std::vector<std::string> rejected;
        for (node_id f : fns) {
            const auto& fn = db_.as<n_fn>(f);
            if (fn.params.size() != args.size()) {
                rejected.push_back(std::format("{}: ожидает {} аргументов", describe_fn(f), fn.params.size()));
                continue;
            }
            int         score = 0;
            std::string why;
            for (u32 i = 0; i < fn.params.size() && why.empty(); ++i) {
                const auto& p = db_.as<n_param>(fn.params[i]);
                if (p.type && db_.is<n_ident>(p.type)) {
                    auto want = str(db_.as<n_ident>(p.type).name);
                    if (types_.describe(vals[i]) != want && types_.name(types_.of(vals[i])) != want)
                        why = std::format("параметр {} ожидает {}, передано {}", str(p.name), want, types_.describe(vals[i]));
                    else ++score;
                }
            }
            if (why.empty()) {
                auto [passed, n_req, reason] = leading_requires(f, vals, args, env, site.where);
                if (!passed) why = reason;
                score += n_req;
            }
            if (why.empty()) ok_candidates.push_back({f, score});
            else rejected.push_back(std::format("{}: {}", describe_fn(f), why));
        }

        if (ok_candidates.empty()) {
            diagnostic d(std::format("нет подходящей перегрузки {}", site.what), site.where);
            for (auto& r : rejected) d.note(std::format("кандидат {}", r), site.where);
            for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) d.note(std::format("в {}", it->what), it->where);
            throw d;
        }
        std::ranges::sort(ok_candidates, std::greater{}, &candidate::score);
        if (ok_candidates.size() > 1 && ok_candidates[0].score == ok_candidates[1].score) {
            diagnostic d(std::format("неоднозначный вызов {}", site.what), site.where);
            for (auto& c : ok_candidates) if (c.score == ok_candidates[0].score) d.note(std::format("кандидат {}", describe_fn(c.fn)), db_[c.fn].where);
            throw d;
        }
        return invoke(ok_candidates[0].fn, vals, args, env, frame, site);
    }

    // Check require at the beginning of the candidate's body: (suitable, number requires, cause).
    std::tuple<bool, int, std::string> leading_requires(node_id f, std::vector<value>& vals, const node_list& args,
                                                        const scope_ptr& env, span where) {
        const auto& fn = db_.as<n_fn>(f);
        if (fn.expr_body || !db_.is<n_block>(fn.body)) return {true, 0, ""};
        auto probe    = std::make_shared<scope>();
        probe->parent = globals_;
        bind_values(fn.params, vals, args, env, probe);
        int n = 0;
        for (node_id s : db_.as<n_block>(fn.body).items) {
            if (!db_.is<n_require>(s)) break;
            ++n;
            flow c = eval(db_.as<n_require>(s).cond, probe, 0);
            if (!c.v.is<bool>() || !c.v.as<bool>())
                return {false, n, std::format("require не выполнено: {}", describe_node(db_.as<n_require>(s).cond))};
        }
        return {true, n, ""};
    }

    flow invoke(node_id f, std::vector<value>& vals, const node_list& args, const scope_ptr& env, u32 frame, call_site site) {
        const auto& fn = db_.as<n_fn>(f);
        if (fn.outline && inline_required(f))
            fail(std::format("{} помечена outline, но использует семантику инлайна", site.what), db_[f].where);

        // Table instance: Function + Argument Types
        std::string key = std::format("{}(", str(fn.name));
        for (u32 i = 0; i < vals.size(); ++i) {
            if (i) key += ", ";
            key += vals[i].is<thunk_v>() ? std::string("expr") : types_.describe(vals[i]);
        }
        key += ")";
        ++instances_[key];

        auto inner    = std::make_shared<scope>();
        inner->parent = globals_;
        bind_values(fn.params, vals, args, env, inner);

        stack_.push_back(site);
        u32 me = ++frames_;
        frames_stack_.push_back({me, fn.name, fn.outline});
        flow r = eval(fn.body, inner, me);
        frames_stack_.pop_back();
        stack_.pop_back();
        if (r.kind == flow_kind::ret && r.target == me) return ok(std::move(r.v));
        return r; // return, addressed to the caller (semantic inlining), goes further
    }

    // Attach parameters: alias to the cell of the caller, if the argument is the place;
    // otherwise, its cell with a time value.
    void bind_values(const node_list& params, std::vector<value>& vals, const node_list& args, const scope_ptr& env,
                     const scope_ptr& inner) {
        for (u32 i = 0; i < params.size(); ++i) {
            const auto& p = db_.as<n_param>(params[i]);
            if (p.mode != param_mode::expr && p.mode != param_mode::own) {
                if (value* cell = place_or_null(args[i], env)) {
                    inner->alias(p.name, cell);
                    continue;
                }
            }
            inner->declare(p.name, vals[i]);
        }
    }

    flow bind(const node_list& params, const node_list& args, const scope_ptr& env, u32 frame, const scope_ptr& inner) {
        for (u32 i = 0; i < params.size(); ++i) {
            const auto& p = db_.as<n_param>(params[i]);
            if (p.mode == param_mode::expr) {
                inner->declare(p.name, value(thunk_v{args[i], env, frame}));
                continue;
            }
            if (value* cell = place_or_null(args[i], env)) {
                inner->alias(p.name, cell);
                continue;
            }
            TZ_TRY(v, eval(args[i], env, frame));
            inner->declare(p.name, std::move(v));
        }
        return ok();
    }

    value* place_or_null(node_id n, const scope_ptr& env) {
        if (db_.is<n_ident>(n)) {
            value* c = env->find(db_.as<n_ident>(n).name);
            return c && !c->is<thunk_v>() ? c : nullptr;
        }
        return nullptr;
    }

    // The function uses the semantics of inline (§6.1): the expr parameter or
    // address return(…) in the body (output from the calling/external function).
    bool inline_required(node_id f) const {
        for (node_id p : db_.as<n_fn>(f).params)
            if (db_.as<n_param>(p).mode == param_mode::expr) return true;
        return has_addressed_return(db_.as<n_fn>(f).body);
    }

    bool has_addressed_return(node_id n) const {
        if (!n) return false;
        if (db_.is<n_return>(n) && db_.as<n_return>(n).target) return true;
        if (db_.is<n_lambda>(n) || db_.is<n_fn>(n)) return false; // assigned functions
        bool found = false;
        db_.for_each_child(n, [&](node_id c) { found = found || has_addressed_return(c); });
        return found;
    }

    std::string describe_fn(node_id f) const {
        const auto& fn = db_.as<n_fn>(f);
        std::string s  = std::format("{}(", str(fn.name));
        for (u32 i = 0; i < fn.params.size(); ++i) {
            if (i) s += ", ";
            const auto& p = db_.as<n_param>(fn.params[i]);
            if (p.type && db_.is<n_ident>(p.type)) s += std::format("{} ", str(db_.as<n_ident>(p.type).name));
            s += str(p.name);
        }
        return s + ")";
    }

    std::string describe_node(node_id n) const {
        const span& w = db_[n].where;
        return std::format("[{}..{}]", w.begin, w.end);
    }

    // --- embedded functions (M0; later VM and stdlib services in language) ---

    value builtin(symbol name, std::vector<value>& a, span where) {
        auto n = str(name);
        if (n == "print") {
            std::string line;
            for (u32 i = 0; i < a.size(); ++i) {
                if (i) line += ' ';
                line += show_v(a[i]);
            }
            if (captured_) *captured_ += line + "\n";
            else std::println("{}", line);
            return {};
        }
        if (n == "typeof" && a.size() == 1) return value(type_v{types_.of(a[0])});
        if (n == "zero" && a.size() == 1 && a[0].is<type_v>()) {
            auto t = types_.name(a[0].as<type_v>().type);
            if (t == "i64") return value(i64(0));
            if (t == "f64") return value(f64(0));
            fail(std::format("zero не определён для {}", t), where);
        }
        if (n == "range" && a.size() == 1 && a[0].is<i64>()) {
            array_v r;
            for (i64 i = 0; i < a[0].as<i64>(); ++i) r.items.push_back(value(i));
            return value(std::move(r));
        }
        if (n == "f64" && a.size() == 1 && a[0].is<i64>()) return value(f64(a[0].as<i64>()));
        if (n == "i64" && a.size() == 1 && a[0].is<f64>()) return value(i64(a[0].as<f64>()));
        if (n == "read_floats") {
            array_v     r;
            std::string line;
            if (std::getline(std::cin, line)) {
                std::stringstream ss(line);
                f64               x;
                while (ss >> x) r.items.push_back(value(x));
            }
            return value(std::move(r));
        }
        if (n == "parse_int" && a.size() == 1 && a[0].is<std::string>()) {
            const auto& s = a[0].as<std::string>();
            i64         v = 0;
            bool        good = !s.empty();
            for (char ch : s) {
                if (ch < '0' || ch > '9') { good = false; break; }
                v = v * 10 + (ch - '0');
            }
            agg_v r{types_.named("result"), {}};
            r.fields.push_back({sym("ok"), value(good)});
            r.fields.push_back({sym("value"), value(good ? v : i64(0))});
            r.fields.push_back({sym("error"), value(good ? std::string() : std::format("не число: {:?}", s))});
            return value(std::move(r));
        }
        if (n == "is_err" && a.size() == 1 && a[0].is<agg_v>()) return value(!member(a[0], sym("ok"), where).as<bool>());
        fail(std::format("неизвестная функция {}", n), where);
    }

#undef TZ_TRY

    ast_db&                                  db_;
    node_id                                  module_;
    scope_ptr                                globals_;
    std::map<symbol, std::vector<node_id>>   functions_;
    type_registry                            types_;
    std::vector<call_site>                   stack_;

    struct frame_rec {
        u32    id;
        symbol name;
        bool   outline;
    };
    std::vector<frame_rec>                   frames_stack_;
    u32                                      frames_ = 0;
    std::map<std::string, u32>               instances_;
    std::string*                             captured_ = nullptr;
};

} // namespace tz
