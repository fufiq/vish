// JIT: instance functions → machine code x86-64 (§10.3 stack, solutions 2026-09-28).
//
// 
// 
//;.
//
// node types are not taken from statistics, but from the static parsing (check) of this
// instance: The node whose value at parsing is known (compile-time) becomes a constant -
// this is "top-down optimization."
//
//
// M1a: primitive code generator operations, local variables in the stack frame,
// without register allocation. What is not supported remains in the interpreter (with
// cause). Internal ABI: u64 f(const u64* args), resulting in bits in rax.
//
#pragma once

#include "../interp.hpp"
#include "x64.hpp"

#include <memory>
#include <set>

namespace tz {

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

enum class jit_fault_kind : uint64_t { none = 0, bounds = 1, div_zero = 2, require = 3, layout = 4 };
uint64_t jit_fault      = 0;
uint64_t jit_fault_node = 0;

// Pointer to a dense array buffer of the desired layout; nullptr - layout is not the
// same (deoptimization: the interpreter will perform).
// want: 1 — i64, 2 — f64. Машинный код получает массив вызывающего (алиас).
void* jit_array_data(array_v* a, uint64_t want) {
    if (a->empty()) return reinterpret_cast<void*>(8); // Empty array: no access
    if (want == 1 && a->kind() == array_v::layout::ints) return a->ints_mut();
    if (want == 2 && a->kind() == array_v::layout::floats) return a->floats_mut();
    return nullptr;
}
uint64_t jit_array_size(array_v* a) { return a->size(); }

enum class jt : uint8_t { unit, i64_, f64_, bool_, arr_i64, arr_f64, other };

std::string_view jt_name(jt t) {
    switch (t) {
    case jt::unit: return "unit";
    case jt::i64_: return "i64";
    case jt::f64_: return "f64";
    case jt::bool_: return "bool";
    case jt::arr_i64: return "array<i64>";
    case jt::arr_f64: return "array<f64>";
    default: return "?";
    }
}

jt jt_of(const value& v) {
    const value* x = shape_of(v);
    if (!x || v.is<split_v>()) return jt::other;
    if (x->is<unit_v>()) return jt::unit;
    if (x->is<int64_t>()) return jt::i64_;
    if (x->is<double>()) return jt::f64_;
    if (x->is<bool>()) return jt::bool_;
    if (x->is<array_v>()) { // Array type - its layout: mixed array not array<i64 >>
        const auto& a = x->as<array_v>();
        if (a.kind() == array_v::layout::ints) return jt::arr_i64;
        if (a.kind() == array_v::layout::floats) return jt::arr_f64;
        return jt::other;
    }
    return jt::other;
}

// Sample erased value of type t (for static parsing instance).
value jt_shape(jt t) {
    switch (t) {
    case jt::i64_: return erase(value(int64_t(1)));
    case jt::f64_: return erase(value(double(1)));
    case jt::bool_: return erase(value(true));
    case jt::arr_i64: return erase(value(array_v::of({value(int64_t(1))})));
    case jt::arr_f64: return erase(value(array_v::of({value(double(1))})));
    default: return value();
    }
}

// Compiled instance. entry - Cell: Calls from another code go through it, so recursion
// and recompilation do not require editing callers.
struct jit_fn {
    x64::code_block    code;
    void*              entry = nullptr;
    std::vector<jt>    params;
    jt                 result = jt::unit;
    bool               ok     = false;
    std::string        reason; // why not compiled
    std::map<uint32_t, uint64_t> deps;   // revision of functions included in the code
    std::size_t        bytes = 0; // size
    uint32_t                unchecked = 0; // indexation without border checks (proven by parsing)
};

struct jit_unsupported {
    node_id     node;
    std::string why;
};

struct jit_engine {
    jit_engine(ast_db& db, node_id module) : db(db), mod(module) {}

    // A compiled instance or nullptr (then an interpreter).
    jit_fn* get(node_id fn, const std::vector<jt>& types) {
        std::string key = std::format("{}(", fn.value);
        for (jt t : types) key += std::string(jt_name(t)) + ",";
        auto it = cache.find(key);
        if (it != cache.end() && still_valid(*it->second)) return it->second->ok ? it->second.get() : nullptr;
        auto  owned = std::make_unique<jit_fn>();
        auto* f     = owned.get();
        cache[key] = std::move(owned);
        f->params   = types;
        compile(fn, *f);
        return f->ok ? f : nullptr;
    }

    std::string stats() const {
        std::string out;
        for (const auto& [key, f] : cache) {
            node_id fn(uint32_t(std::stoul(key.substr(0, key.find('(')))));
            auto    name = db.strings.text(db.as<n_fn>(fn).name);
            if (f->ok) out += std::format("  jit  {}({}) → {} байт машинного кода{}\n", name, types_str(f->params), f->bytes,
                                   f->unchecked ? std::format(", без проверки границ: {}", f->unchecked) : "");
            else out += std::format("  интерпретатор  {}({}): {}\n", name, types_str(f->params), f->reason);
        }
        return out;
    }
    static std::string types_str(const std::vector<jt>& ts) {
        std::string s;
        for (jt t : ts) s += (s.empty() ? "" : ", ") + std::string(jt_name(t));
        return s;
    }

    bool still_valid(const jit_fn& f) const {
        for (const auto& [node, rev] : f.deps)
            if (db.node_revision(node_id(node)) != rev) return false;
        return true;
    }

    void compile(node_id fn, jit_fn& out);
    ast_db&                                        db;
    node_id                                        mod;
    std::map<std::string, std::unique_ptr<jit_fn>> cache;
};

// --- code generation one instance ---

struct jit_codegen {
    jit_codegen(jit_engine& eng, node_id fn, jit_fn& out, interpreter& analysis)
        : eng(eng), db(eng.db), fn(fn), out(out), an(analysis) {}

    void run() {
        using namespace x64;
        const auto& f = db.as<n_fn>(fn);
        code.push(rbp);
        code.mov(rbp, rsp);
        code.sub_rsp(0);
        frame_patch = code.here() - 4;

        // parameters: from the array of arguments (rdi) to the frame
        scopes.emplace_back();
        std::vector<std::pair<slot, uint32_t>> arrays;
        for (uint32_t i = 0; i < f.params.size(); ++i) {
            const auto& p = db.as<n_param>(f.params[i]);
            if (p.mode == param_mode::expr || p.mode == param_mode::out || p.mode == param_mode::own)
                throw jit_unsupported{f.params[i], "режим параметра"};
            if (!is_array(out.params[i]) && an.param_written(fn, i)) // The scalar is transmitted by the value: there is no return record
                throw jit_unsupported{f.params[i], "меняет скалярный параметр (только встраивание)"};
            slot s = declare(p.name, out.params[i]);
            code.load(rax, rdi, int32_t(8 * i));
            code.store(rbp, s.off, rax);
            if (is_array(s.type)) arrays.push_back({s, i});
        }
        // Arrays: dense buffer and size - once at the input (the size in the code does not change)
        for (auto [s, i] : arrays) {
            code.load(rdi, rbp, s.off);
            code.mov(rsi, uint64_t(s.type == jt::arr_i64 ? 1 : 2));
            call_native(reinterpret_cast<void*>(&jit_array_data));
            code.test(rax, rax);
            code.jcc(e, fault(jit_fault_kind::layout, f.params[i])); // Type parsing guaranteed: we don't get here
            code.store(rbp, s.data_off, rax);
            code.load(rdi, rbp, s.off);
            call_native(reinterpret_cast<void*>(&jit_array_size));
            code.store(rbp, s.size_off, rax);
        }

        frames.push_back({fn, &epilogue, out.result, 0, true});
        jt body = gen_value(f.body);
        if (body != out.result && body != jt::unit)
            throw jit_unsupported{f.body, std::format("тип результата {} ≠ {}", jt_name(body), jt_name(out.result))};
        to_result(body);

        code.bind(epilogue);
        code.mov(rsp, rbp);
        code.pop(rbp);
        code.ret();
        for (auto& st : stubs) emit_stub(st);

        code.patch32(frame_patch, int32_t((uint32_t(slots) * 8 + 15) & ~15u));
        out.bytes     = code.size();
        out.unchecked = bce;
        out.code  = code.finalize();
        out.entry = out.code.entry;
        out.ok    = true;
    }
    struct slot {
        int32_t     off = 0;
        jt      type = jt::unit;
        int32_t     data_off = 0, size_off = 0; // massif
        node_id thunk;                      // expr-parameter: the expression of the calling ...
        uint32_t     ctx = 0;                    // ... and the context in which it is calculated.
    };

    // The context of the embedding point: what was seen by the caller.
    struct context {
        uint32_t scopes, fn_base, frames;
        uint64_t ctx; // Context of caller embedding (parsing observations)
    };

    // The frame of the function in the embedding chain: where does its return lead?
    struct frame {
        node_id     fn;
        x64::label* exit;
        jt          result;
        uint32_t         depth; // time-stack depth at the entrance
        bool        top;   // instance: Return to the epilogue
    };

    struct stub {
        x64::label      at;
        jit_fault_kind  kind;
        node_id         node;
    };

    static bool is_array(jt t) { return t == jt::arr_i64 || t == jt::arr_f64; }


    int32_t alloc() { return -8 * int32_t(++slots); }

    slot declare(symbol name, jt t) { // The vector of the regions can move
        slot s;
        s.type = t;
        s.off  = alloc();
        if (is_array(t)) {
            s.data_off = alloc();
            s.size_off = alloc();
        }
        scopes.back().push_back({name, s});
        return s;
    }

    // The name in the visible areas of the current function (the built-in function
    // does not see the local names of the caller - only its parameters - alias).
    const slot* find(symbol name) const {
        for (uint32_t k = uint32_t(scopes.size()); k-- > fn_base;)
            for (auto it = scopes[k].rbegin(); it != scopes[k].rend(); ++it)
                if (it->first == name) return &it->second;
        return nullptr;
    }

    // Perform gen in the context of the caller (for the expr parameter): areas and
    // frames that appear after the embedding point are removed at this time.
    auto in_context(uint32_t index, auto&& f) {
        const context& c = contexts[index];
        std::vector<std::vector<std::pair<symbol, slot>>> later_scopes(std::make_move_iterator(scopes.begin() + c.scopes),
                                                                       std::make_move_iterator(scopes.end()));
        std::vector<frame> later_frames(frames.begin() + c.frames, frames.end());
        scopes.resize(c.scopes);
        frames.resize(c.frames);
        uint32_t base = std::exchange(fn_base, c.fn_base);
        uint64_t cx   = std::exchange(ctx, c.ctx);
        auto r   = f();
        fn_base = base;
        ctx     = cx;
        for (auto& sc : later_scopes) scopes.push_back(std::move(sc));
        frames.insert(frames.end(), later_frames.begin(), later_frames.end());
        return r;
    }

    // The variable slot; the expr parameter associated with the name of the caller is its slot.
    const slot* resolve(symbol name) {
        const slot* s = find(name);
        if (s && s->thunk && db.is<n_ident>(s->thunk)) {
            symbol inner = db.as<n_ident>(s->thunk).name;
            return in_context(s->ctx, [&] { return resolve(inner); });
        }
        return s;
    }

    x64::label& fault(jit_fault_kind k, node_id n) {
        stubs.push_back(std::make_unique<stub>(stub{{}, k, n}));
        return stubs.back()->at;
    }

    void emit_stub(const std::unique_ptr<stub>& st) {
        using namespace x64;
        code.bind(st->at);
        code.mov(rax, reinterpret_cast<uint64_t>(&jit_fault));
        code.store_imm32(rax, 0, int32_t(st->kind));
        code.mov(rax, reinterpret_cast<uint64_t>(&jit_fault_node));
        code.store_imm32(rax, 0, int32_t(st->node.value));
        code.mov(rax, uint64_t(0));
        code.jmp(epilogue);
    }

    // Call runtime; align the stack by 16 before calling.
    void call_native(void* fn) {
        using namespace x64;
        bool pad = depth % 2 != 0;
        if (pad) code.sub_rsp(8);
        code.mov(rax, reinterpret_cast<uint64_t>(fn));
        code.call(rax);
        if (pad) code.add_rsp(8);
    }

    // Result: i64/bool/array - rax, f64 - xmm0. Rax for return.
    void to_result(jt t) {
        if (t == jt::f64_) code.movq(x64::rax, x64::xmm0);
    }

    void push_result(jt t) {
        if (t == jt::f64_) code.movq(x64::rax, x64::xmm0);
        code.push(x64::rax);
        ++depth;
    }

    void pop_to(jt t, bool into_second) {
        using namespace x64;
        // second operand already in rax/xmm0 → transfer to rcx/xmm1, the first from the stack
        if (into_second) {
            if (t == jt::f64_) code.movsd(xmm1, xmm0);
            else code.mov(rcx, rax);
        }
        code.pop(rax);
        --depth;
        if (t == jt::f64_) code.movq(xmm0, rax);
    }

    // Observation of node in the current embedding context: Each call site has its own
    // copy of the body with its own types and compile-time.
    const interpreter::observation* obs(node_id n) const { return an.observed(n, ctx); }

    jt type_of(node_id n) const {
        const auto* o = obs(n);
        return o && o->seen ? jt_of(o->v) : jt::other;
    }

    // A constant if the value of node at parsing is known and pure ("top").
    bool try_constant(node_id n, jt& t) {
        using namespace x64;
        if (db.is<n_var>(n) || db.is<n_block>(n) || db.is<n_return>(n) || db.is<n_for>(n) || db.is<n_while>(n)) return false;
        if (db.is<n_op>(n)) {
            auto op = db.strings.text(db.as<n_op>(n).op);
            if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") return false;
        }
        const auto* o = obs(n);
        if (!o || !o->seen || o->effect || o->diverted || !o->v.known()) return false;
        if (o->v.is<int64_t>()) { code.mov(rax, uint64_t(o->v.as<int64_t>())); t = jt::i64_; return true; }
        if (o->v.is<bool>()) { code.mov(rax, uint64_t(o->v.as<bool>())); t = jt::bool_; return true; }
        if (o->v.is<double>()) {
            uint64_t bits;
            double x = o->v.as<double>();
            std::memcpy(&bits, &x, 8);
            code.mov(rax, bits);
            code.movq(xmm0, rax);
            t = jt::f64_;
            return true;
        }
        return false;
    }

    // --- expression ---

    jt gen_value(node_id n) {
        jt t;
        if (try_constant(n, t)) return t;
        return std::visit([&](const auto& k) { return gen(n, k); }, db[n].kind);
    }

    jt gen(node_id, const n_int& x) { code.mov(x64::rax, uint64_t(x.value)); return jt::i64_; }

    jt gen(node_id, const n_float& x) {
        uint64_t bits;
        std::memcpy(&bits, &x.value, 8);
        code.mov(x64::rax, bits);
        code.movq(x64::xmm0, x64::rax);
        return jt::f64_;
    }

    jt gen(node_id n, const n_ident& x) {
        const slot* s = find(x.name);
        if (!s) throw jit_unsupported{n, std::format("имя {} вне кадра", db.strings.text(x.name))};
        if (s->thunk) { // expr parameter: the caller expression is calculated at each mention (§6.1)
            slot th = *s;
            return in_context(th.ctx, [&] { return gen_value(th.thunk); });
        }
        if (s->type == jt::f64_) code.movsd_load(x64::xmm0, x64::rbp, s->off);
        else code.load(x64::rax, x64::rbp, s->off);
        return s->type;
    }

    const slot& array_slot(node_id obj) {
        if (!db.is<n_ident>(obj)) throw jit_unsupported{obj, "массив не из переменной"};
        const slot* s = resolve(db.as<n_ident>(obj).name);
        if (!s || !is_array(s->type)) throw jit_unsupported{obj, "не массив"};
        return *s;
    }

    jt gen(node_id n, const n_member& m) {
        if (db.strings.text(m.name) != "size") throw jit_unsupported{n, "поле"};
        code.load(x64::rax, x64::rbp, array_slot(m.object).size_off);
        return jt::i64_;
    }

    // Index with check of borders (unsigned comparison catches and negative). There is
    // no verification if parsing has proven an index within bounds (safe index, §12.1):
    // JIT does not prove itself, but takes language knowledge from parsing.
    void gen_checked_index(const slot& s, node_id index, node_id n) {
        using namespace x64;
        if (an.bounds_proven(n)) {
            if (gen_value(index) != jt::i64_) throw jit_unsupported{index, "индекс не i64"};
            ++bce;
            return;
        }
        if (gen_value(index) != jt::i64_) throw jit_unsupported{index, "индекс не i64"};
        code.load(rcx, rbp, s.size_off);
        code.cmp(rax, rcx);
        code.jcc(ae, fault(jit_fault_kind::bounds, n));
    }

    jt gen(node_id n, const n_index& i) {
        using namespace x64;
        slot s = array_slot(i.object);
        gen_checked_index(s, i.index, n);
        code.load(rcx, rbp, s.data_off);
        if (s.type == jt::arr_f64) { code.movsd_load_idx(xmm0, rcx, rax); return jt::f64_; }
        code.load_idx(rax, rcx, rax);
        return jt::i64_;
    }

    jt gen(node_id n, const n_op& o) {
        using namespace x64;
        auto op = db.strings.text(o.op);
        if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") { gen_assign(n, o, op); return jt::unit; }
        if (o.args.size() == 1) {
            jt t = gen_value(o.args[0]);
            if (op == "-" && t == jt::i64_) { code.neg(rax); return t; }
            if (op == "-" && t == jt::f64_) {
                code.mov(rax, uint64_t(0x8000000000000000ull));
                code.movq(xmm1, rax);
                code.xorpd(xmm0, xmm1);
                return t;
            }
            if (op == "!" && t == jt::bool_) { code.xor_imm8(rax, 1); return t; }
            throw jit_unsupported{n, std::format("оператор {}", op)};
        }
        if (op == "&&" || op == "||") {
            label done;
            if (gen_value(o.args[0]) != jt::bool_) throw jit_unsupported{n, "логика не над bool"};
            code.test(rax, rax);
            code.jcc(op == "&&" ? e : ne, done);
            if (gen_value(o.args[1]) != jt::bool_) throw jit_unsupported{n, "логика не над bool"};
            code.bind(done);
            return jt::bool_;
        }
        jt a = gen_value(o.args[0]);
        push_result(a);
        jt b_ = gen_value(o.args[1]);
        if (a != b_) throw jit_unsupported{n, std::format("{} {} {}", jt_name(a), op, jt_name(b_))};
        pop_to(a, true);
        return binary(n, op, a);
    }

    // The first operand is in rax/xmm0, the second in rcx/xmm1.
    jt binary(node_id n, std::string_view op, jt t) {
        using namespace x64;
        if (t == jt::i64_) {
            if (op == "+") { code.add(rax, rcx); return t; }
            if (op == "-") { code.sub(rax, rcx); return t; }
            if (op == "*") { code.imul(rax, rcx); return t; }
            if (op == "/" || op == "%") {
                label normal, done;
                code.test(rcx, rcx);
                code.jcc(e, fault(jit_fault_kind::div_zero, n));
                code.cmp_imm8(rcx, -1); // INT_MIN / -1 should not fall: modular arithmetic
                code.jcc(ne, normal);
                if (op == "/") code.neg(rax);
                else code.mov(rax, uint64_t(0));
                code.jmp(done);
                code.bind(normal);
                code.cqo();
                code.idiv(rcx);
                if (op == "%") code.mov(rax, rdx);
                code.bind(done);
                return t;
            }
            return int_compare(n, op);
        }
        if (t == jt::f64_) {
            if (op == "+") { code.addsd(xmm0, xmm1); return t; }
            if (op == "-") { code.subsd(xmm0, xmm1); return t; }
            if (op == "*") { code.mulsd(xmm0, xmm1); return t; }
            if (op == "/") { code.divsd(xmm0, xmm1); return t; }
            return float_compare(n, op);
        }
        if (t == jt::bool_ && (op == "==" || op == "!=")) return int_compare(n, op);
        throw jit_unsupported{n, std::format("{} над {}", op, jt_name(t))};
    }

    jt int_compare(node_id n, std::string_view op) {
        using namespace x64;
        cond c;
        if (op == "==") c = e;
        else if (op == "!=") c = ne;
        else if (op == "<") c = l;
        else if (op == "<=") c = le;
        else if (op == ">") c = g;
        else if (op == ">=") c = ge;
        else throw jit_unsupported{n, std::format("оператор {}", op)};
        code.cmp(rax, rcx);
        code.setcc(c, rax);
        code.movzx_rax_al();
        return jt::bool_;
    }

    // IEEE F64 Comparison: With NaN, everything except != is false.
    jt float_compare(node_id n, std::string_view op) {
        using namespace x64;
        if (op == "==" || op == "!=") {
            code.ucomisd(xmm0, xmm1);
            code.setcc(op == "==" ? e : ne, rax);
            code.setcc(op == "==" ? np : p, rcx);
            if (op == "==") code.and_al_cl();
            else code.or_al_cl();
        } else if (op == "<" || op == "<=") {
            code.ucomisd(xmm1, xmm0);
            code.setcc(op == "<" ? a : ae, rax);
        } else if (op == ">" || op == ">=") {
            code.ucomisd(xmm0, xmm1);
            code.setcc(op == ">" ? a : ae, rax);
        } else {
            throw jit_unsupported{n, std::format("оператор {}", op)};
        }
        code.movzx_rax_al();
        return jt::bool_;
    }

    void gen_assign(node_id n, const n_op& o, std::string_view op) {
        using namespace x64;
        node_id target = o.args[0];
        if (db.is<n_ident>(target)) {
            const slot* sp = resolve(db.as<n_ident>(target).name);
            if (!sp || sp->thunk || is_array(sp->type)) throw jit_unsupported{target, "присваивание"};
            slot  cell = *sp; // meaning: embedding on the right side can shift areas
            const slot* s = &cell;
            jt t = gen_value(o.args[1]);
            if (t != s->type) throw jit_unsupported{n, "тип присваивания"};
            if (op != "=") { // The current is the first operand, the right is the second
                push_result(t);
                if (t == jt::f64_) code.movsd_load(xmm0, rbp, s->off);
                else code.load(rax, rbp, s->off);
                pop_second(t);
                binary(n, op.substr(0, 1), t);
            }
            if (t == jt::f64_) code.movsd_store(rbp, s->off, xmm0);
            else code.store(rbp, s->off, rax);
            return;
        }
        if (db.is<n_index>(target)) {
            const auto& ix = db.as<n_index>(target);
            slot        s  = array_slot(ix.object);
            jt          et = s.type == jt::arr_f64 ? jt::f64_ : jt::i64_;
            if (gen_value(o.args[1]) != et) throw jit_unsupported{n, "тип элемента"};
            push_result(et);                        // [rsp+8] - right side (after index)
            gen_checked_index(s, ix.index, target); // rax - index
            code.push(rax);                          // [rsp] - index
            ++depth;
            if (op != "=") {
                code.load(rcx, rbp, s.data_off);
                if (et == jt::f64_) {
                    code.movsd_load_idx(xmm0, rcx, rax);
                    code.load(r8, rsp, 8);
                    code.movq(xmm1, r8);
                } else {
                    code.load_idx(rax, rcx, rax);
                    code.load(rcx, rsp, 8);
                }
                binary(n, op.substr(0, 1), et);
            } else if (et == jt::f64_) {
                code.load(rax, rsp, 8);
                code.movq(xmm0, rax);
            } else {
                code.load(rax, rsp, 8);
            }
            code.pop(rdx);
            code.add_rsp(8);
            depth -= 2;
            code.load(rcx, rbp, s.data_off);
            if (et == jt::f64_) code.movsd_store_idx(rcx, rdx, xmm0);
            else code.store_idx(rcx, rdx, rax);
            return;
        }
        throw jit_unsupported{target, "цель присваивания"};
    }

    // The second operand from the stack is rcx/xmm1, without touching the first (rax/xmm0).
    void pop_second(jt t) {
        using namespace x64;
        if (t == jt::f64_) {
            code.pop(rax);
            code.movq(xmm1, rax);
        } else {
            code.pop(rcx);
        }
        --depth;
    }

    jt gen(node_id n, const n_call& c) {
        using namespace x64;
        if (!db.is<n_ident>(c.callee)) throw jit_unsupported{n, "вызов не по имени"};
        auto name = db.strings.text(db.as<n_ident>(c.callee).name);
        if (name == "f64" && c.args.size() == 1) {
            if (gen_value(c.args[0]) != jt::i64_) throw jit_unsupported{n, "f64(не i64)"};
            code.cvtsi2sd(xmm0, rax);
            return jt::f64_;
        }
        if (name == "i64" && c.args.size() == 1) {
            if (gen_value(c.args[0]) != jt::f64_) throw jit_unsupported{n, "i64(не f64)"};
            code.cvttsd2si(rax, xmm0);
            return jt::i64_;
        }
        // User function: single candidate by name and arnost, without requiring
        node_id target;
        for (node_id item : db.as<n_module>(eng.mod).items)
            if (db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == name
                && db.as<n_fn>(item).params.size() == c.args.size()) {
                if (target) throw jit_unsupported{n, std::format("перегрузка {} (M1a)", name)};
                target = item;
            }
        if (!target) throw jit_unsupported{n, std::format("вызов {}", name)};
        // Default semantic inlining (D10): the body is embedded in the context of the
        // call, the parameters are alias of the caller's seats, without ABI.
//,
        // 
        // 
        // . Recursion and outline are a real challenge.
        bool recursive = std::ranges::any_of(frames, [&](const frame& f) { return f.fn == target; });
        if (!recursive && !db.as<n_fn>(target).outline) return inline_call(n, c, target);
        std::vector<jt> types;
        for (node_id a : c.args) {
            jt t = type_of(a);
            if (t == jt::other || t == jt::unit) throw jit_unsupported{a, "тип аргумента"};
            types.push_back(t);
        }
        jit_fn* callee = target == fn ? &out : eng.get(target, types);
        if (target == fn && types != out.params) callee = eng.get(target, types);
        if (!callee || (callee != &out && !callee->ok))
            throw jit_unsupported{n, std::format("{} не компилируется", name)};
        out.deps.insert(callee->deps.begin(), callee->deps.end());

        // arguments - in adjacent frame slots, the address of the first - in rdi
        uint32_t first = slots + 1;
        for (uint32_t i = 0; i < c.args.size(); ++i) alloc();
        int32_t base = -8 * int32_t(first + c.args.size() - 1);
        for (uint32_t i = 0; i < c.args.size(); ++i) {
            jt t = gen_value(c.args[i]);
            if (is_array(t)) { // the array is transmitted by a pointer to array_v
                code.load(rax, rbp, array_slot(c.args[i]).off);
            }
            to_result(t);
            code.store(rbp, base + int32_t(8 * i), rax);
        }
        code.lea(rdi, rbp, base);
        bool pad = depth % 2 != 0;
        if (pad) code.sub_rsp(8);
        code.mov(rax, reinterpret_cast<uint64_t>(&callee->entry));
        code.call_mem(rax, 0);
        if (pad) code.add_rsp(8);
        // Error in the code called - come out, the mark is already
        code.mov(rcx, reinterpret_cast<uint64_t>(&jit_fault));
        code.cmp_mem_imm8(rcx, 0, 0);
        code.jcc(ne, epilogue);
        if (callee->result == jt::f64_) code.movq(xmm0, rax);
        return callee->result;
    }

    jt inline_call(node_id n, const n_call& c, node_id target) {
        using namespace x64;
        const auto& g = db.as<n_fn>(target);
        context     at{uint32_t(scopes.size()), fn_base, uint32_t(frames.size()), ctx};
        uint32_t    ctx_index = uint32_t(contexts.size());
        contexts.push_back(at);
        // arguments: place - alias slot, expr - expression with context, other - calculated in your slot
        std::vector<std::pair<symbol, slot>> params;
        for (uint32_t i = 0; i < g.params.size(); ++i) {
            const auto& p   = db.as<n_param>(g.params[i]);
            node_id     arg = c.args[i];
            if (p.mode == param_mode::expr) {
                slot s;
                s.thunk = arg;
                s.ctx   = ctx_index;
                params.push_back({p.name, s});
                continue;
            }
            if (p.mode != param_mode::own && db.is<n_ident>(arg)) {
                if (const slot* s = find(db.as<n_ident>(arg).name)) {
                    params.push_back({p.name, *s}); // alias: both reading and writing are caller cells
                    continue;
                }
            }
            if (p.mode == param_mode::out || p.mode == param_mode::mut) throw jit_unsupported{arg, "mut/out не на место"};
            jt t = gen_value(arg);
            if (t == jt::other || t == jt::unit || is_array(t)) throw jit_unsupported{arg, std::format("аргумент типа {}", jt_name(t))};
            slot s;
            s.type = t;
            s.off  = alloc();
            if (t == jt::f64_) code.movsd_store(rbp, s.off, xmm0);
            else code.store(rbp, s.off, rax);
            params.push_back({p.name, s});
        }
        const auto* o      = obs(n);
        jt          result = o && o->seen ? jt_of(o->v) : jt::unit;
        if (result == jt::other || is_array(result)) throw jit_unsupported{n, "тип результата встроенной функции"};
        out.deps[target.value] = db.node_revision(target);

        auto exit = std::make_unique<label>();
        scopes.emplace_back(std::move(params));
        uint32_t base = std::exchange(fn_base, uint32_t(scopes.size() - 1));
        uint64_t cx   = std::exchange(ctx, interpreter::inline_context(ctx, n.value));
        frames.push_back({target, exit.get(), result, depth, false});
        jt body = gen_value(g.body);
        if (body != result && body != jt::unit) throw jit_unsupported{n, std::format("тип результата {} ≠ {}", jt_name(body), jt_name(result))};
        code.bind(*exit);
        frames.pop_back();
        fn_base = base;
        ctx     = cx;
        scopes.resize(at.scopes);
        inline_exits.push_back(std::move(exit));
        return result;
    }

    jt gen(node_id n, const n_if& i) {
        using namespace x64;
        label else_, done;
        if (gen_value(i.cond) != jt::bool_) throw jit_unsupported{i.cond, "условие не bool"};
        code.test(rax, rax);
        code.jcc(e, else_);
        jt t = gen_value(i.then_);
        code.jmp(done);
        code.bind(else_);
        jt u = i.else_ ? gen_value(i.else_) : jt::unit;
        code.bind(done);
        if (t != u && t != jt::unit && u != jt::unit) throw jit_unsupported{n, "ветки разных типов (расщепление)"};
        return t == u ? t : jt::unit;
    }

    jt gen(node_id n, const n_block& b) {
        scopes.emplace_back();
        jt last = jt::unit;
        for (uint32_t k = 0; k < b.items.size(); ++k) {
            last = gen_value(b.items[k]);
            if (!(b.has_value && k + 1 == b.items.size())) last = jt::unit;
        }
        scopes.pop_back();
        return last;
    }

    jt gen(node_id n, const n_var& v) {
        using namespace x64;
        jt t = gen_value(v.init);
        if (t == jt::other || t == jt::unit || is_array(t)) throw jit_unsupported{n, std::format("переменная типа {}", jt_name(t))};
        slot s = declare(v.name, t);
        if (t == jt::f64_) code.movsd_store(rbp, s.off, xmm0);
        else code.store(rbp, s.off, rax);
        return jt::unit;
    }

    // return(f) / return(caller) - from the external in the embedding chain (§6.1): just
    // a transition to its output.
    jt gen(node_id n, const n_return& r) {
        uint32_t k = uint32_t(frames.size() - 1);
        if (r.target) {
            symbol name      = db.as<n_ident>(r.target).name;
            bool   to_caller = db.strings.text(name) == "caller";
            bool   found     = false;
            while (k > 0) {
                if (db.as<n_fn>(frames[k].fn).outline) throw jit_unsupported{n, "return через outline"};
                --k;
                if (to_caller || db.as<n_fn>(frames[k].fn).name == name) { found = true; break; }
            }
            if (!found) throw jit_unsupported{n, "return: цель вне машинного кода"};
        }
        frame fr = frames[k];
        jt    t  = r.value ? gen_value(r.value) : jt::unit;
        if (t != fr.result) throw jit_unsupported{n, "тип return"};
        if (fr.top) {
            to_result(t);
            code.jmp(epilogue); // epilogue restores rsp
        } else {
            if (depth > fr.depth) code.add_rsp(int32_t(8 * (depth - fr.depth))); // transient
            code.jmp(*fr.exit);
        }
        return jt::unit;
    }

    jt gen(node_id n, const n_require& r) {
        using namespace x64;
        if (gen_value(r.cond) != jt::bool_) throw jit_unsupported{r.cond, "require не bool"};
        code.test(rax, rax);
        code.jcc(e, fault(jit_fault_kind::require, n));
        return jt::unit;
    }

    jt gen(node_id n, const n_while& w) {
        using namespace x64;
        label top, done;
        code.bind(top);
        if (gen_value(w.cond) != jt::bool_) throw jit_unsupported{w.cond, "условие не bool"};
        code.test(rax, rax);
        code.jcc(e, done);
        gen_value(w.body);
        code.jmp(top);
        code.bind(done);
        return jt::unit;
    }

    jt gen(node_id n, const n_for& f) {
        using namespace x64;
        scopes.emplace_back();
        label top, done;
        // For(i : range(n) ) - Counting cycle without array: language knows what range is
        if (db.is<n_call>(f.range) && db.is<n_ident>(db.as<n_call>(f.range).callee)
            && db.strings.text(db.as<n_ident>(db.as<n_call>(f.range).callee).name) == "range"
            && db.as<n_call>(f.range).args.size() == 1) {
            node_id bound = db.as<n_call>(f.range).args[0];
            if (gen_value(bound) != jt::i64_) throw jit_unsupported{f.range, "range(не i64)"};
            int32_t   limit = alloc();
            slot  var   = declare(f.var, jt::i64_);
            code.store(rbp, limit, rax);
            code.mov(rax, uint64_t(0));
            code.store(rbp, var.off, rax);
            code.bind(top);
            code.load(rax, rbp, var.off);
            code.load(rcx, rbp, limit);
            code.cmp(rax, rcx);
            code.jcc(ge, done);
            gen_value(f.body);
            code.load(rax, rbp, var.off);
            code.mov(rcx, uint64_t(1));
            code.add(rax, rcx);
            code.store(rbp, var.off, rax);
            code.jmp(top);
        } else { // variable array
            slot        arr  = array_slot(f.range);
            jt          et   = arr.type == jt::arr_f64 ? jt::f64_ : jt::i64_;
            int32_t         idx  = alloc();
            slot        var  = declare(f.var, et);
            code.mov(rax, uint64_t(0));
            code.store(rbp, idx, rax);
            code.bind(top);
            code.load(rax, rbp, idx);
            code.load(rcx, rbp, arr.size_off);
            code.cmp(rax, rcx);
            code.jcc(ge, done);
            code.load(rcx, rbp, arr.data_off);
            if (et == jt::f64_) { code.movsd_load_idx(xmm0, rcx, rax); code.movsd_store(rbp, var.off, xmm0); }
            else { code.load_idx(rax, rcx, rax); code.store(rbp, var.off, rax); }
            gen_value(f.body);
            code.load(rax, rbp, idx);
            code.mov(rcx, uint64_t(1));
            code.add(rax, rcx);
            code.store(rbp, idx, rax);
            code.jmp(top);
        }
        code.bind(done);
        scopes.pop_back();
        return jt::unit;
    }

    jt gen(node_id n, const n_ct& c) { return gen_value(c.body); }

    jt gen(node_id n, const auto&) {
        throw jit_unsupported{n, std::format("узел {}", kind_name(db[n]))};
    }

    jit_engine&                                   eng;
    ast_db&                                       db;
    node_id                                       fn;
    jit_fn&                                       out;
    interpreter&                                  an;
    x64::assembler                                code;
    x64::label                                    epilogue;
    std::vector<std::unique_ptr<stub>>            stubs;
    std::vector<std::vector<std::pair<symbol, slot>>> scopes;
    uint32_t                                           slots = 0;
    uint32_t                                           bce = 0; // stripped
    uint32_t                                           depth = 0;
    uint32_t                                           frame_patch = 0;
    uint32_t                                           fn_base = 0; // first area of the current (built-in) function
    uint64_t                                           ctx = 0;     // Embedded context (as in parsing)
    std::vector<frame>                            frames;
    std::vector<context>                          contexts;
    std::vector<std::unique_ptr<x64::label>>      inline_exits;
};

// Cheap syntactic filter to static parsing: what code generator obviously can’t do is not worth
// disassembling (parsing can calculate a lot with compile-time).
std::optional<std::pair<node_id, std::string>> jit_prefilter(const ast_db& db, node_id module, node_id n) {
    if (!n) return std::nullopt;
    if (db.is<n_str>(n) || db.is<n_lambda>(n) || db.is<n_try_compile>(n) || db.is<n_construct>(n) || db.is<n_hole>(n))
        return std::pair{n, std::format("узел {}", kind_name(db[n]))};
    if (db.is<n_fn>(n) && db[n].parent && !db.is<n_module>(db[n].parent)) return std::pair{n, std::string("локальная функция")};
    if (db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) {
        auto name  = db.strings.text(db.as<n_ident>(db.as<n_call>(n).callee).name);
        bool known = name == "f64" || name == "i64" || name == "range";
        for (node_id item : db.as<n_module>(module).items)
            known = known || (db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == name);
        if (!known) return std::pair{n, std::format("вызов {}", name)};
    }
    std::optional<std::pair<node_id, std::string>> found;
    db.for_each_child(n, [&](node_id c) { if (!found) found = jit_prefilter(db, module, c); });
    return found;
}

void jit_engine::compile(node_id fn, jit_fn& out) {
    out.deps[fn.value]      = db.node_revision(fn);
    out.deps[mod.value] = db.node_revision(mod);
    if (auto bad = jit_prefilter(db, mod, db.as<n_fn>(fn).body)) {
        auto lc    = locate(db.source(db[bad->first].where.source), db[bad->first].where.begin);
        out.reason = std::format("{} (строка {})", bad->second, lc.line);
        return;
    }
    // Static parsing instance: Types and compile-time node (value-approach)
    interpreter        an(db, mod, {"jit"}, mode::check);
    an.set_fuel(200000);
    std::vector<value> shapes;
    for (jt t : out.params) {
        if (t == jt::other || t == jt::unit) { out.reason = "тип параметра"; return; }
        shapes.push_back(jt_shape(t));
    }
    try {
        value r    = an.analyze_instance(fn, shapes);
        out.result = jt_of(r);
        if (out.result == jt::arr_i64 || out.result == jt::arr_f64) { out.reason = "результат — массив (пока интерпретатор)"; return; }
        if (out.result == jt::other) { out.reason = std::format("тип результата {}", r.is<split_v>() ? "расщеплён" : "не скалярный"); return; }
        jit_codegen(*this, fn, out, an).run();
    } catch (const jit_unsupported& u) {
        auto lc    = locate(db.source(db[u.node].where.source), db[u.node].where.begin);
        out.reason = std::format("{} (строка {})", u.why, lc.line);
    } catch (const diagnostic& d) {
        out.reason = std::format("разбор: {}", d.what());
    }
}

// Call instance by machine code from the interpreter (run mode).
std::optional<interpreter::flow> interpreter::try_jit(node_id f, std::vector<value>& vals, const node_list& args,
                                                             const scope_ptr& env, const std::string& key) {
    // expr/own/out and variable scalars - semantics of inline/feedback while interpreter;
    // variable array possible: machine code receives an array of caller
    for (const auto& v : vals) if (v.is<thunk_v>()) return std::nullopt;
    const auto& params = db.as<n_fn>(f).params;
    for (uint32_t i = 0; i < params.size(); ++i) {
        auto m = db.as<n_param>(params[i]).mode;
        if (m == param_mode::expr || m == param_mode::own || m == param_mode::out) return std::nullopt;
        // scalar that the function changes: there is no return entry from the interpreter
        if (!vals[i].is<array_v>() && writes_param(f, i)) return std::nullopt;
    }
    std::vector<jt> param_types;
    for (const auto& v : vals) {
        jt t = jt_of(v);
        if (t == jt::other || t == jt::unit) return std::nullopt;
        param_types.push_back(t);
    }
    jit_fn* code = jit->get(f, param_types);
    if (!code) return std::nullopt;

    std::vector<uint64_t> raw(vals.size());
    for (uint32_t i = 0; i < vals.size(); ++i) {
        const value& v = vals[i];
        if (v.is<int64_t>()) raw[i] = uint64_t(v.as<int64_t>());
        else if (v.is<bool>()) raw[i] = v.as<bool>();
        else if (v.is<double>()) std::memcpy(&raw[i], &v.as<double>(), 8);
        else { // array: pointer to array_v caller (alias) or to a copy
            value* cell = i < args.size() ? place_or_null(args[i], env) : nullptr;
            raw[i]      = reinterpret_cast<uint64_t>(cell && cell->is<array_v>() ? &cell->as<array_v>() : &vals[i].as<array_v>());
        }
    }
    jit_fault      = 0;
    auto entry     = reinterpret_cast<uint64_t (*)(const uint64_t*)>(code->entry);
    uint64_t  bits      = entry(raw.data());
    auto fault     = jit_fault_kind(jit_fault);
    // No Deoptimizations: Type - Contract for the entire program
// The mismatch is a VM error.
    if (fault != jit_fault_kind::none) {
        node_id at{uint32_t(jit_fault_node)};
        jit_fault = 0;
        if (fault == jit_fault_kind::layout)
            throw diagnostic("внутренняя ошибка JIT: раскладка массива не совпала с типом экземпляра", db[at].where);
        const char* what = fault == jit_fault_kind::bounds ? "индекс вне границ массива (машинный код)"
                         : fault == jit_fault_kind::div_zero ? "деление на ноль (машинный код)"
                                                            : "нарушено требование require (машинный код)";
        fail(what, db[at].where, diag_kind::runtime);
    }
    emit({report_event::instance, key, {}, {}, {}, {}});
    switch (code->result) {
    case jt::i64_: return ok(value(int64_t(bits)));
    case jt::bool_: return ok(value(bits != 0));
    case jt::f64_: {
        double x;
        std::memcpy(&x, &bits, 8);
        return ok(value(x));
    }
    default: return ok(value());
    }
}

} // namespace tz
