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

#include <deque>
#include <set>

namespace tz {

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// clang-format off
enum class jit_fault_kind : uint64_t { none = 0, bounds = 1, div_zero = 2, require = 3, layout = 4, thrown = 5 };
// clang-format on
uint64_t jit_fault = 0;
uint64_t jit_fault_node = 0;
// The exception that comes out of machine code is the "second way"
// value and its type.
uint64_t jit_thrown = 0;
uint64_t jit_thrown_type = 0; // jt

// Pointer to a dense array buffer of the desired layout; nullptr - layout is not the
// same (deoptimization: the interpreter will perform).
// want: 1 — i64, 2 — f64. Машинный код получает массив вызывающего (алиас).
void * jit_array_data(array_v * a, uint64_t want) {
  if(a->empty()) return reinterpret_cast<void *>(8); // Empty array: no access
  if(want == 1 && a->kind() == array_v::layout::ints) return a->ints_mut();
  if(want == 2 && a->kind() == array_v::layout::floats) return a->floats_mut();
  return nullptr;
}
uint64_t jit_array_size(array_v * a) { return a->size(); }

// clang-format off
enum class jt : uint8_t { unit, i64_, f64_, bool_, arr_i64, arr_f64, range_, other }; // range - range(n) : in the slot only n
// clang-format on

std::string_view jt_name(jt t) {
  switch(t) {
    case jt::unit: return "unit";
    case jt::i64_: return "i64";
    case jt::f64_: return "f64";
    case jt::bool_: return "bool";
    case jt::arr_i64: return "array<i64>";
    case jt::arr_f64: return "array<f64>";
    case jt::range_: return "range";
    default: return "?";
  }
}

jt jt_of(value const & v) {
  value const * x = shape_of(v);
  if(!x || v.is<split_v>()) return jt::other;
  if(x->is<unit_v>()) return jt::unit;
  if(x->is<int64_t>()) return jt::i64_;
  if(x->is<double>()) return jt::f64_;
  if(x->is<bool>()) return jt::bool_;
  if(x->is<array_v>()) { // Array type - its layout: mixed array not array<i64 >>
    auto const & a = x->as<array_v>();
    if(a.kind() == array_v::layout::ints) return jt::arr_i64;
    if(a.kind() == array_v::layout::floats) return jt::arr_f64;
    return jt::other;
  }
  return jt::other;
}

// Sample erased value of type t (for static parsing instance).
value jt_shape(jt t) {
  switch(t) {
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
  x64::code_block code;
  void * entry = nullptr;
  std::vector<jt> params;
  jt result = jt::unit;
  bool ok = false;
  std::string reason; // why not compiled
  std::map<uint32_t, uint64_t> deps; // revision of functions included in the code
  std::size_t bytes = 0; // size
  uint32_t unchecked = 0; // indexation without border checks (proven by parsing)
  uint32_t jumps = 0; // throw that became a transition to catch (static exceptions)
};

struct jit_unsupported {
  node_id node;
  std::string why;
};

struct jit_engine {
  jit_engine(ast_db & db, node_id module) : db(db), mod(module) {}

  // A compiled instance or nullptr (then an interpreter).
  jit_fn * get(node_id fn, std::vector<jt> const & types) {
    std::string key = cat(fn.value, "(");
    for(jt t: types) key += std::string(jt_name(t)) + ",";
    auto it = cache.find(key);
    if(it != cache.end() && still_valid(fns[it->second])) return fns[it->second].ok ? &fns[it->second] : nullptr;
    // new instance - at the end of the arena; outdated remains in place: its input
    // cell can be referenced by already compiled code
    jit_fn * f = &fns.emplace_back();
    cache[key] = uint32_t(fns.size() - 1);
    f->params = types;
    compile(fn, *f);
    return f->ok ? f : nullptr;
  }

  std::string stats() const {
    std::string out;
    for(auto const & [key, index]: cache) {
      jit_fn const * f = &fns[index];
      node_id fn(uint32_t(std::stoul(key.substr(0, key.find('(')))));
      auto name = db.strings.text(db.as<n_fn>(fn).name);
      if(f->ok) out += cat("  jit  ", name, "(", types_str(f->params), ") → ", f->bytes, " байт машинного кода", f->unchecked ? cat(", без проверки границ: ", f->unchecked) : "", f->jumps ? cat(", исключений переходом: ", f->jumps) : "", "\n");
      else out += cat("  интерпретатор  ", name, "(", types_str(f->params), "): ", f->reason, "\n");
    }
    return out;
  }
  static std::string types_str(std::vector<jt> const & ts) {
    std::string s;
    for(jt t: ts) s += (s.empty() ? "" : ", ") + std::string(jt_name(t));
    return s;
  }

  bool still_valid(jit_fn const & f) const {
    for(auto const & [node, rev]: f.deps)
      if(db.node_revision(node_id(node)) != rev) return false;
    return true;
  }

  void compile(node_id fn, jit_fn & out);
  ast_db & db;
  node_id mod;
  std::deque<jit_fn> fns; // instance arena (addresses stable)
  std::map<std::string, uint32_t> cache; // key instance → number in fns
};

// --- code generation one instance ---

struct jit_codegen {
  jit_codegen(jit_engine & eng, node_id fn, jit_fn & out, interpreter & analysis)
      : eng(eng), db(eng.db), fn(fn), out(out), an(analysis) {}

  void run() {
    using namespace x64;
    auto const & f = db.as<n_fn>(fn);
    code.push(rbp);
    code.mov(rbp, rsp);
    code.sub_rsp(0);
    frame_patch = code.here() - 4;
    // registers of variables that the caller is obliged to save - in the frame slots
    for(auto r: gpr_pool) {
      int32_t off = alloc();
      saved.push_back({r, off});
      code.store(rbp, off, r);
    }
    weigh(f.body, 1);

    // parameters: from the array of arguments (rdi) to the frame
    scopes.emplace_back();
    std::vector<std::pair<slot, uint32_t>> arrays;
    for(uint32_t i = 0; i < f.params.size(); ++i) {
      auto const & p = db.as<n_param>(f.params[i]);
      if(p.mode == param_mode::expr || p.mode == param_mode::out || p.mode == param_mode::own)
        throw jit_unsupported{f.params[i], "режим параметра"};
      if(!is_array(out.params[i]) && an.param_written(fn, i)) // The scalar is transmitted by the value: there is no return record
        throw jit_unsupported{f.params[i], "меняет скалярный параметр (только встраивание)"};
      slot s = declare(p.name, out.params[i]);
      code.load(rax, rdi, int32_t(8 * i));
      if(is_array(s.type)) {
        code.store(rbp, s.off, rax); // array_v * - always in the frame (transmitted further)
        arrays.push_back({s, i});
      } else {
        if(s.type == jt::f64_) code.movq(xmm0, rax);
        store_var(s);
      }
    }
    // Arrays: dense buffer and size - once at the input (the size in the code does not change)
    for(auto [s, i]: arrays) {
      code.load(rdi, rbp, s.off);
      code.mov(rsi, uint64_t(s.type == jt::arr_i64 ? 1 : 2));
      call_native(reinterpret_cast<void *>(&jit_array_data));
      code.test(rax, rax);
      code.jcc(e, fault(jit_fault_kind::layout, f.params[i])); // Type parsing guaranteed: we don't get here
      if(s.data_reg >= 0) code.mov(gpr_pool[s.data_reg], rax);
      else code.store(rbp, s.data_off, rax);
      code.load(rdi, rbp, s.off);
      call_native(reinterpret_cast<void *>(&jit_array_size));
      code.store(rbp, s.size_off, rax);
    }

    frames.push_back({fn, &epilogue, out.result, 0, true});
    jt body = gen_value(f.body);
    if(body != out.result && body != jt::unit)
      throw jit_unsupported{f.body, cat("тип результата ", jt_name(body), " ≠ ", jt_name(out.result))};
    to_result(body);

    code.bind(epilogue);
    for(auto [r, off]: saved) code.load(r, rbp, off);
    code.mov(rsp, rbp);
    code.pop(rbp);
    code.ret();
    for(auto & st: stubs) emit_stub(st);

    code.patch32(frame_patch, int32_t((uint32_t(slots) * 8 + 15) & ~15u));
    out.bytes = code.size();
    out.unchecked = bce;
    out.jumps = throw_jumps;
    out.code = code.finalize();
    out.entry = out.code.entry;
    out.ok = true;
  }
  struct slot {
    int32_t off = 0;
    jt type = jt::unit;
    int32_t data_off = 0, size_off = 0; // massif
    node_id thunk; // expr-parameter: the expression of the calling ...
    uint32_t ctx = 0; // ... and the context in which it is calculated.
    int8_t reg = -1; // scalar i64/bool in register gpr_pool [reg]
    int8_t xreg = -1; // f64 in register xmm_pool [xreg]
    int8_t data_reg = -1; // Array buffer in register gpr_pool [data_reg]
    bool owner = false; // The slot itself occupied the registers (alias - no)
  };

  // The context of the embedding point: what was seen by the caller.
  struct context {
    uint32_t scopes, fn_base, frames;
    uint64_t ctx; // Context of caller embedding (parsing observations)
  };

  // The frame of the function in the embedding chain: where does its return lead?
  struct frame {
    node_id fn;
    x64::label * exit;
    jt result;
    uint32_t depth; // time-stack depth at the entrance
    bool top; // instance: Return to the epilogue
  };

  struct stub {
    x64::label at;
    jit_fault_kind kind;
    node_id node;
    x64::label * to = nullptr; // VM error inside try: transition to catch
    uint32_t pop = 0; // ... taking the temporary off the stack
  };

  // The handler catch in machine code.
  struct catch_info {
    std::string type; // empty - any (base)
    x64::label * at;
    slot var; // where the throw puts the value
    bool binds = false;
  };
  struct try_frame {
    std::vector<catch_info> catches;
    uint32_t depth;
  };

  static std::string_view vm_type(jit_fault_kind k) {
    return k == jit_fault_kind::bounds ? "out_of_bounds" : k == jit_fault_kind::div_zero ? "div_by_zero"
        : k == jit_fault_kind::require                                                   ? "require_failed"
                                                                                         : "";
  }

  static bool is_array(jt t) { return t == jt::arr_i64 || t == jt::arr_f64; }

  int32_t alloc() { return -8 * int32_t(++slots); }

  // --- registers ---
  //
  // 
// So simple: variable with weight (references × 8)
//
  // for each level of the cycle) occupies a free register for the duration of its
  // area; there is no coloring graph. Home slot remains: f64-registers called does
  // not save, their values at the time of the call go to the slots.
  static constexpr x64::reg gpr_pool[] = {x64::rbx, x64::r12, x64::r14, x64::r15, x64::r13}; // r13 - not the SIB base
  static constexpr x64::xmm xmm_pool[] = {x64::xmm8, x64::xmm9, x64::xmm10, x64::xmm11, x64::xmm12, x64::xmm13, x64::xmm14, x64::xmm15};

  void weigh(node_id n, uint32_t w) {
    if(!n) return;
    if(db.is<n_ident>(n)) weights[db.as<n_ident>(n).name.value] += w;
    if(db.is<n_loop>(n) || (db.is<n_lambda>(n) && db.as<n_lambda>(n).block)) w *= 8; // block with parameters - the body of the structure-cycle
    db.for_each_child(n, [&](node_id c) { weigh(c, w); });
  }

  int8_t take_gpr(bool as_base) {
    for(int8_t k = 0; k < int8_t(std::size(gpr_pool)); ++k)
      if(!(gpr_used & (1u << k)) && !(as_base && gpr_pool[k] == x64::r13)) {
        gpr_used |= 1u << k;
        return k;
      }
    return -1;
  }
  int8_t take_xmm() {
    for(int8_t k = 0; k < int8_t(std::size(xmm_pool)); ++k)
      if(!(xmm_used & (1u << k))) {
        xmm_used |= 1u << k;
        return k;
      }
    return -1;
  }

  slot declare(symbol name, jt t, uint32_t weight = 0) { // The vector of the regions can move
    slot s;
    s.type = t;
    s.off = alloc();
    if(is_array(t)) {
      s.data_off = alloc();
      s.size_off = alloc();
    }
    if(!weight && name) weight = weights[name.value];
    if(weight >= 2) {
      if(t == jt::i64_ || t == jt::bool_ || t == jt::range_) s.reg = take_gpr(false);
      else if(t == jt::f64_) {
        s.xreg = take_xmm();
        if(s.xreg >= 0) live_xmm.push_back({s.xreg, s.off});
      } else if(is_array(t)) s.data_reg = take_gpr(true);
    }
    s.owner = true;
    scopes.back().push_back({name, s});
    return s;
  }

  void release(slot const & s) {
    if(!s.owner) return;
    if(s.reg >= 0) gpr_used &= ~(1u << s.reg);
    if(s.data_reg >= 0) gpr_used &= ~(1u << s.data_reg);
    if(s.xreg >= 0) {
      xmm_used &= ~(1u << s.xreg);
      std::erase_if(live_xmm, [&](auto const & e) { return e.first == s.xreg; });
    }
  }

  // End of areas (not temporary hiding as in in_context): registers are free.
  void pop_scopes(std::size_t keep) {
    while(scopes.size() > keep) {
      for(auto & [name, s]: scopes.back()) release(s);
      scopes.pop_back();
    }
  }

  // f64-registers called does not save: at the time of the call - in home slots.
  void spill_xmm() {
    for(auto [x, off]: live_xmm) code.movsd_store(x64::rbp, off, xmm_pool[x]);
  }
  void reload_xmm() {
    for(auto [x, off]: live_xmm) code.movsd_load(xmm_pool[x], x64::rbp, off);
  }

  // Variable = rax/xmm0 and back; in an arbitrary register - for the second operand.
  void load_gpr(slot const & s, x64::reg r) {
    if(s.reg >= 0) {
      if(gpr_pool[s.reg] != r) code.mov(r, gpr_pool[s.reg]);
    } else code.load(r, x64::rbp, s.off);
  }
  void load_xmm(slot const & s, x64::xmm x) {
    if(s.xreg >= 0) code.movsd(x, xmm_pool[s.xreg]);
    else code.movsd_load(x, x64::rbp, s.off);
  }
  void load_var(slot const & s) {
    if(s.type == jt::f64_) load_xmm(s, x64::xmm0);
    else load_gpr(s, x64::rax);
  }
  void store_var(slot const & s) {
    if(s.type == jt::f64_) {
      if(s.xreg >= 0) code.movsd(xmm_pool[s.xreg], x64::xmm0);
      else code.movsd_store(x64::rbp, s.off, x64::xmm0);
    } else if(s.reg >= 0) code.mov(gpr_pool[s.reg], x64::rax);
    else code.store(x64::rbp, s.off, x64::rax);
  }
  // Array Buffer Base: Variable Register or Scratch Load.
  x64::reg data_base(slot const & s, x64::reg scratch) {
    if(s.data_reg >= 0) return gpr_pool[s.data_reg];
    code.load(scratch, x64::rbp, s.data_off);
    return scratch;
  }

  // The name in the visible areas of the current function (the built-in function
  // does not see the local names of the caller - only its parameters - alias).
  slot const * find(symbol name) const {
    for(uint32_t k = uint32_t(scopes.size()); k-- > fn_base;)
      for(auto it = scopes[k].rbegin(); it != scopes[k].rend(); ++it)
        if(it->first == name) return &it->second;
    return nullptr;
  }

  // Perform gen in the context of the caller (for the expr parameter): areas and
  // frames that appear after the embedding point are removed at this time.
  auto in_context(uint32_t index, auto && f) {
    context const & c = contexts[index];
    std::vector<std::vector<std::pair<symbol, slot>>> later_scopes(std::make_move_iterator(scopes.begin() + c.scopes),
        std::make_move_iterator(scopes.end()));
    std::vector<frame> later_frames(frames.begin() + c.frames, frames.end());
    scopes.resize(c.scopes);
    frames.resize(c.frames);
    uint32_t base = std::exchange(fn_base, c.fn_base);
    uint64_t cx = std::exchange(ctx, c.ctx);
    auto r = f();
    fn_base = base;
    ctx = cx;
    for(auto & sc: later_scopes) scopes.push_back(std::move(sc));
    frames.insert(frames.end(), later_frames.begin(), later_frames.end());
    return r;
  }

  // The variable slot; the expr parameter associated with the name of the caller is its slot.
  slot const * resolve(symbol name) {
    slot const * s = find(name);
    if(s && s->thunk && db.is<n_ident>(s->thunk)) {
      symbol inner = db.as<n_ident>(s->thunk).name;
      return in_context(s->ctx, [&] { return resolve(inner); });
    }
    return s;
  }

  x64::label & fault(jit_fault_kind k, node_id n) {
    // VM error inside try with a suitable catch - go there (the object of error is the
    // aggregate, in the machine code it is not: catch should not use it)
    for(auto t = tries.rbegin(); t != tries.rend(); ++t)
      for(auto & c: t->catches)
        if(c.type.empty() || c.type == vm_type(k)) {
          if(c.binds) throw jit_unsupported{n, "catch использует объект ошибки VM"};
          stubs.push_back(stub{{}, k, n, c.at, depth - t->depth});
          return stubs.back().at;
        }
    stubs.push_back(stub{{}, k, n});
    return stubs.back().at;
  }

  void emit_stub(stub & st) {
    using namespace x64;
    code.bind(st.at);
    if(st.to) {
      if(st.pop) code.add_rsp(int32_t(8 * st.pop));
      code.jmp(*st.to);
      return;
    }
    code.mov(rax, reinterpret_cast<uint64_t>(&jit_fault));
    code.store_imm32(rax, 0, int32_t(st.kind));
    code.mov(rax, reinterpret_cast<uint64_t>(&jit_fault_node));
    code.store_imm32(rax, 0, int32_t(st.node.value));
    code.mov(rax, uint64_t(0));
    code.jmp(epilogue);
  }

  // Call runtime; align the stack by 16 before calling.
  void call_native(void * fn) {
    using namespace x64;
    spill_xmm();
    bool pad = depth % 2 != 0;
    if(pad) code.sub_rsp(8);
    code.mov(rax, reinterpret_cast<uint64_t>(fn));
    code.call(rax);
    if(pad) code.add_rsp(8);
    reload_xmm();
  }

  // Result: i64/bool/array - rax, f64 - xmm0. Rax for return.
  void to_result(jt t) {
    if(t == jt::f64_) code.movq(x64::rax, x64::xmm0);
  }

  void push_result(jt t) {
    if(t == jt::f64_) code.movq(x64::rax, x64::xmm0);
    code.push(x64::rax);
    ++depth;
  }

  void pop_to(jt t, bool into_second) {
    using namespace x64;
    // second operand already in rax/xmm0 → transfer to rcx/xmm1, the first from the stack
    if(into_second) {
      if(t == jt::f64_) code.movsd(xmm1, xmm0);
      else code.mov(rcx, rax);
    }
    code.pop(rax);
    --depth;
    if(t == jt::f64_) code.movq(xmm0, rax);
  }

  // Observation of node in the current embedding context: Each call site has its own
  // copy of the body with its own types and compile-time.
  interpreter::observation const * obs(node_id n) const { return an.observed(n, ctx); }

  jt type_of(node_id n) const {
    auto const * o = obs(n);
    return o && o->seen ? jt_of(o->v) : jt::other;
  }

  // A constant if the value of node at parsing is known and pure ("top").
  bool try_constant(node_id n, jt & t) {
    using namespace x64;
    if(db.is<n_var>(n) || db.is<n_block>(n) || db.is<n_return>(n) || db.is<n_loop>(n) || db.is<n_break>(n) || db.is<n_continue>(n)) return false;
    if(db.is<n_op>(n)) {
      auto op = db.strings.text(db.as<n_op>(n).op);
      if(op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") return false;
    }
    auto const * o = obs(n);
    if(!o || !o->seen || o->effect || o->diverted || !o->v.known()) return false;
    if(o->v.is<int64_t>()) {
      code.mov(rax, uint64_t(o->v.as<int64_t>()));
      t = jt::i64_;
      return true;
    }
    if(o->v.is<bool>()) {
      code.mov(rax, uint64_t(o->v.as<bool>()));
      t = jt::bool_;
      return true;
    }
    if(o->v.is<double>()) {
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
    if(try_constant(n, t)) return t;
    return std::visit([&](auto const & k) { return gen(n, k); }, db[n].kind);
  }

  jt gen(node_id, n_int const & x) {
    code.mov(x64::rax, uint64_t(x.value));
    return jt::i64_;
  }

  jt gen(node_id, n_float const & x) {
    uint64_t bits;
    std::memcpy(&bits, &x.value, 8);
    code.mov(x64::rax, bits);
    code.movq(x64::xmm0, x64::rax);
    return jt::f64_;
  }

  jt gen(node_id n, n_ident const & x) {
    slot const * s = find(x.name);
    if(!s) throw jit_unsupported{n, cat("имя ", db.strings.text(x.name), " вне кадра")};
    if(s->thunk) { // expr parameter: the caller expression is calculated at each mention (§6.1)
      slot th = *s;
      return in_context(th.ctx, [&] { return gen_value(th.thunk); });
    }
    if(is_array(s->type)) code.load(x64::rax, x64::rbp, s->off); // array_v*
    else load_var(*s);
    return s->type;
  }

  slot const & array_slot(node_id obj) {
    if(!db.is<n_ident>(obj)) throw jit_unsupported{obj, "массив не из переменной"};
    slot const * s = resolve(db.as<n_ident>(obj).name);
    if(!s || !is_array(s->type)) throw jit_unsupported{obj, "не массив"};
    return *s;
  }

  jt gen(node_id n, n_member const & m) {
    if(db.strings.text(m.name) != "size") throw jit_unsupported{n, "поле"};
    if(slot const * r = range_slot(m.object)) {
      load_gpr(*r, x64::rax);
      return jt::i64_;
    }
    code.load(x64::rax, x64::rbp, array_slot(m.object).size_off);
    return jt::i64_;
  }

  // range(n) in variable: the slot stores only n (the array is not built).
  slot const * range_slot(node_id obj) {
    if(!db.is<n_ident>(obj)) return nullptr;
    slot const * s = resolve(db.as<n_ident>(obj).name);
    return s && s->type == jt::range_ ? s : nullptr;
  }

  // Index with check of borders (unsigned comparison catches and negative). There is
  // no verification if parsing has proven an index within bounds (safe index, §12.1):
  // JIT does not prove itself, but takes language knowledge from parsing.
  void gen_checked_index(slot const & s, node_id index, node_id n) {
    using namespace x64;
    if(an.bounds_proven(n)) {
      if(gen_value(index) != jt::i64_) throw jit_unsupported{index, "индекс не i64"};
      ++bce;
      return;
    }
    if(gen_value(index) != jt::i64_) throw jit_unsupported{index, "индекс не i64"};
    code.load(rcx, rbp, s.size_off);
    code.cmp(rax, rcx);
    code.jcc(ae, fault(jit_fault_kind::bounds, n));
  }

  jt gen(node_id n, n_index const & i) {
    using namespace x64;
    if(slot const * r = range_slot(i.object)) { // range: element k is k
      slot rs = *r;
      if(gen_value(i.index) != jt::i64_) throw jit_unsupported{i.index, "индекс не i64"};
      if(an.bounds_proven(n)) ++bce;
      else {
        load_gpr(rs, rcx);
        code.cmp(rax, rcx);
        code.jcc(ae, fault(jit_fault_kind::bounds, n));
      }
      return jt::i64_;
    }
    slot s = array_slot(i.object);
    gen_checked_index(s, i.index, n);
    reg b = data_base(s, rcx);
    if(s.type == jt::arr_f64) {
      code.movsd_load_idx(xmm0, b, rax);
      return jt::f64_;
    }
    code.load_idx(rax, b, rax);
    return jt::i64_;
  }

  jt gen(node_id n, n_op const & o) {
    using namespace x64;
    auto op = db.strings.text(o.op);
    if(an.user_operator(n)) { // code operator: built-in operation if its overload is not accepted by these types
      std::vector<std::string> ts;
      for(node_id a: o.args) ts.push_back(std::string(jt_name(type_of(a))));
      if(o.postfix || an.user_operator_applies(an.entity_of(o), ts)) throw jit_unsupported{n, cat("оператор ", op, ", объявленный кодом")};
    }
    if(op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") {
      gen_assign(n, o, op);
      return jt::unit;
    }
    if(o.args.size() == 1) {
      jt t = gen_value(o.args[0]);
      if(op == "-" && t == jt::i64_) {
        code.neg(rax);
        return t;
      }
      if(op == "-" && t == jt::f64_) {
        code.mov(rax, uint64_t(0x8000000000000000ull));
        code.movq(xmm1, rax);
        code.xorpd(xmm0, xmm1);
        return t;
      }
      if(op == "!" && t == jt::bool_) {
        code.xor_imm8(rax, 1);
        return t;
      }
      throw jit_unsupported{n, cat("оператор ", op)};
    }
    if(op == "&&" || op == "||") {
      label done;
      if(gen_value(o.args[0]) != jt::bool_) throw jit_unsupported{n, "логика не над bool"};
      code.test(rax, rax);
      code.jcc(op == "&&" ? e : ne, done);
      if(gen_value(o.args[1]) != jt::bool_) throw jit_unsupported{n, "логика не над bool"};
      code.bind(done);
      return jt::bool_;
    }
    jt a = gen_value(o.args[0]);
    jt b_;
    if(simple(o.args[1])) b_ = gen_second(o.args[1]); // without stack: immediately in rcx/xmm1
    else {
      push_result(a);
      b_ = gen_value(o.args[1]);
      if(a == b_) pop_to(a, true);
    }
    if(a != b_) throw jit_unsupported{n, cat(jt_name(a), " ", op, " ", jt_name(b_))};
    return binary(n, op, a);
  }

  // node constant from observations parsing (pure, known).
  interpreter::observation const * constant_obs(node_id n) const {
    if(db.is<n_var>(n) || db.is<n_block>(n) || db.is<n_return>(n) || db.is<n_loop>(n) || db.is<n_break>(n) || db.is<n_continue>(n) || db.is<n_op>(n) && is_assign(db.strings.text(db.as<n_op>(n).op))) return nullptr;
    auto const * o = obs(n);
    if(!o || !o->seen || o->effect || o->diverted || !o->v.known()) return nullptr;
    if(!o->v.is<int64_t>() && !o->v.is<double>() && !o->v.is<bool>()) return nullptr;
    return o;
  }
  static bool is_assign(std::string_view op) { return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/="; }

  // The second operand, which can be obtained immediately in rcx/xmm1 without touching rax/xmm0.
  bool simple(node_id n) {
    if(constant_obs(n) || db.is<n_int>(n) || db.is<n_float>(n)) return true;
    if(db.is<n_ident>(n)) {
      slot const * s = find(db.as<n_ident>(n).name);
      return s && !s->thunk && !is_array(s->type);
    }
    if(db.is<n_index>(n)) {
      auto const & i = db.as<n_index>(n);
      if(!db.is<n_ident>(i.object)) return false;
      slot const * a = find(db.as<n_ident>(i.object).name);
      if(!a || a->thunk || !is_array(a->type)) return false;
      if(db.is<n_int>(i.index)) return true;
      if(!db.is<n_ident>(i.index)) return false;
      slot const * k = find(db.as<n_ident>(i.index).name);
      return k && !k->thunk && k->type == jt::i64_;
    }
    return false;
  }

  jt gen_second(node_id n) {
    using namespace x64;
    auto imm = [&](value const & v) {
      if(v.is<double>()) {
        uint64_t bits;
        double x = v.as<double>();
        std::memcpy(&bits, &x, 8);
        code.mov(rdx, bits);
        code.movq(xmm1, rdx);
        return jt::f64_;
      }
      code.mov(rcx, v.is<bool>() ? uint64_t(v.as<bool>()) : uint64_t(v.as<int64_t>()));
      return v.is<bool>() ? jt::bool_ : jt::i64_;
    };
    if(auto const * c = constant_obs(n)) return imm(c->v);
    if(db.is<n_int>(n)) return imm(value(db.as<n_int>(n).value));
    if(db.is<n_float>(n)) return imm(value(db.as<n_float>(n).value));
    if(db.is<n_ident>(n)) {
      slot s = *find(db.as<n_ident>(n).name);
      if(s.type == jt::f64_) load_xmm(s, xmm1);
      else load_gpr(s, rcx);
      return s.type;
    }
    auto const & i = db.as<n_index>(n);
    slot a = *find(db.as<n_ident>(i.object).name);
    if(db.is<n_int>(i.index)) code.mov(rdx, uint64_t(db.as<n_int>(i.index).value));
    else load_gpr(*find(db.as<n_ident>(i.index).name), rdx);
    if(an.bounds_proven(n)) ++bce;
    else {
      code.load(rcx, rbp, a.size_off);
      code.cmp(rdx, rcx);
      code.jcc(ae, fault(jit_fault_kind::bounds, n));
    }
    reg b = data_base(a, rcx);
    if(a.type == jt::arr_f64) {
      code.movsd_load_idx(xmm1, b, rdx);
      return jt::f64_;
    }
    code.load_idx(rcx, b, rdx);
    return jt::i64_;
  }

  // The first operand is in rax/xmm0, the second in rcx/xmm1.
  jt binary(node_id n, std::string_view op, jt t) {
    using namespace x64;
    if(t == jt::i64_) {
      if(op == "+") {
        code.add(rax, rcx);
        return t;
      }
      if(op == "-") {
        code.sub(rax, rcx);
        return t;
      }
      if(op == "*") {
        code.imul(rax, rcx);
        return t;
      }
      if(op == "/" || op == "%") {
        label normal, done;
        code.test(rcx, rcx);
        code.jcc(e, fault(jit_fault_kind::div_zero, n));
        code.cmp_imm8(rcx, -1); // INT_MIN / -1 should not fall: modular arithmetic
        code.jcc(ne, normal);
        if(op == "/") code.neg(rax);
        else code.mov(rax, uint64_t(0));
        code.jmp(done);
        code.bind(normal);
        code.cqo();
        code.idiv(rcx);
        if(op == "%") code.mov(rax, rdx);
        code.bind(done);
        return t;
      }
      return int_compare(n, op);
    }
    if(t == jt::f64_) {
      if(op == "+") {
        code.addsd(xmm0, xmm1);
        return t;
      }
      if(op == "-") {
        code.subsd(xmm0, xmm1);
        return t;
      }
      if(op == "*") {
        code.mulsd(xmm0, xmm1);
        return t;
      }
      if(op == "/") {
        code.divsd(xmm0, xmm1);
        return t;
      }
      return float_compare(n, op);
    }
    if(t == jt::bool_ && (op == "==" || op == "!=")) return int_compare(n, op);
    throw jit_unsupported{n, cat(op, " над ", jt_name(t))};
  }

  jt int_compare(node_id n, std::string_view op) {
    using namespace x64;
    cond c;
    if(op == "==") c = e;
    else if(op == "!=") c = ne;
    else if(op == "<") c = l;
    else if(op == "<=") c = le;
    else if(op == ">") c = g;
    else if(op == ">=") c = ge;
    else throw jit_unsupported{n, cat("оператор ", op)};
    code.cmp(rax, rcx);
    code.setcc(c, rax);
    code.movzx_rax_al();
    return jt::bool_;
  }

  // IEEE F64 Comparison: With NaN, everything except != is false.
  jt float_compare(node_id n, std::string_view op) {
    using namespace x64;
    if(op == "==" || op == "!=") {
      code.ucomisd(xmm0, xmm1);
      code.setcc(op == "==" ? e : ne, rax);
      code.setcc(op == "==" ? np : p, rcx);
      if(op == "==") code.and_al_cl();
      else code.or_al_cl();
    } else if(op == "<" || op == "<=") {
      code.ucomisd(xmm1, xmm0);
      code.setcc(op == "<" ? a : ae, rax);
    } else if(op == ">" || op == ">=") {
      code.ucomisd(xmm0, xmm1);
      code.setcc(op == ">" ? a : ae, rax);
    } else {
      throw jit_unsupported{n, cat("оператор ", op)};
    }
    code.movzx_rax_al();
    return jt::bool_;
  }

  void gen_assign(node_id n, n_op const & o, std::string_view op) {
    using namespace x64;
    node_id target = o.args[0];
    if(db.is<n_ident>(target) && op == "=" && !resolve(db.as<n_ident>(target).name)) { // x = e without name - announcement
      jt t = gen_value(o.args[1]);
      if(t == jt::other || t == jt::unit || is_array(t)) throw jit_unsupported{n, cat("переменная типа ", jt_name(t))};
      slot s = declare(db.as<n_ident>(target).name, t);
      store_var(s);
      return;
    }
    if(db.is<n_ident>(target)) {
      slot const * sp = resolve(db.as<n_ident>(target).name);
      if(!sp || sp->thunk || is_array(sp->type)) throw jit_unsupported{target, "присваивание"};
      slot cell = *sp; // meaning: embedding on the right side can shift areas
      slot const * s = &cell;
      jt t = gen_value(o.args[1]);
      if(t != s->type) throw jit_unsupported{n, "тип присваивания"};
      if(op != "=") { // the right part is the second operand (rcx/xmm1), the current one is the first; no stack
        if(t == jt::f64_) code.movsd(xmm1, xmm0);
        else code.mov(rcx, rax);
        load_var(*s);
        binary(n, op.substr(0, 1), t);
      }
      store_var(*s);
      return;
    }
    if(db.is<n_index>(target)) {
      auto const & ix = db.as<n_index>(target);
      slot s = array_slot(ix.object);
      jt et = s.type == jt::arr_f64 ? jt::f64_ : jt::i64_;
      if(gen_value(o.args[1]) != et) throw jit_unsupported{n, "тип элемента"};
      push_result(et); // [rsp+8] - right side (after index)
      gen_checked_index(s, ix.index, target); // rax - index
      code.push(rax); // [rsp] - index
      ++depth;
      if(op != "=") {
        reg b = data_base(s, rcx);
        if(et == jt::f64_) {
          code.movsd_load_idx(xmm0, b, rax);
          code.load(r8, rsp, 8);
          code.movq(xmm1, r8);
        } else {
          code.load_idx(rax, b, rax);
          code.load(rcx, rsp, 8);
        }
        binary(n, op.substr(0, 1), et);
      } else if(et == jt::f64_) {
        code.load(rax, rsp, 8);
        code.movq(xmm0, rax);
      } else {
        code.load(rax, rsp, 8);
      }
      code.pop(rdx);
      code.add_rsp(8);
      depth -= 2;
      reg b = data_base(s, rcx);
      if(et == jt::f64_) code.movsd_store_idx(b, rdx, xmm0);
      else code.store_idx(b, rdx, rax);
      return;
    }
    throw jit_unsupported{target, "цель присваивания"};
  }

  // The second operand from the stack is rcx/xmm1, without touching the first (rax/xmm0).
  void pop_second(jt t) {
    using namespace x64;
    if(t == jt::f64_) {
      code.pop(rax);
      code.movq(xmm1, rax);
    } else {
      code.pop(rcx);
    }
    --depth;
  }

  jt gen(node_id n, n_call const & c) {
    using namespace x64;
    if(!db.is<n_ident>(c.callee)) throw jit_unsupported{n, "вызов не по имени"};
    auto name = db.strings.text(db.as<n_ident>(c.callee).name);
    if(name == "f64" && c.args.size() == 1) {
      if(gen_value(c.args[0]) != jt::i64_) throw jit_unsupported{n, "f64(не i64)"};
      code.cvtsi2sd(xmm0, rax);
      return jt::f64_;
    }
    if(name == "i64" && c.args.size() == 1) {
      if(gen_value(c.args[0]) != jt::f64_) throw jit_unsupported{n, "i64(не f64)"};
      code.cvttsd2si(rax, xmm0);
      return jt::i64_;
    }
    if(slot const * s = find(db.as<n_ident>(c.callee).name); s && s->thunk) return call_block(n, c, *s);
    if(name == "range" && c.args.size() == 1) {
      if(gen_value(c.args[0]) != jt::i64_) throw jit_unsupported{n, "range(не i64)"};
      return jt::range_;
    }
    if(name == "is_index" && c.args.size() == 2) { // 0 <= k < size: one unsigned comparison
      if(gen_value(c.args[0]) != jt::i64_) throw jit_unsupported{n, "is_index: индекс не i64"};
      if(slot const * r = range_slot(c.args[1])) load_gpr(*r, rcx);
      else code.load(rcx, rbp, array_slot(c.args[1]).size_off);
      code.cmp(rax, rcx);
      code.setcc(b, rax);
      code.movzx_rax_al();
      return jt::bool_;
    }
    if(name == "intercept") return gen_intercept(n, c);
    // Language function (prelude or module): single candidate by name and arnost, without requiring
    // candidates by name and arnost; typed parameters are cut off by argument types from parsing, with
    // equality more precisely - with a large number of types
    node_id target;
    int best = -1;
    bool tie = false;
    for(node_id m: {db.prelude, eng.mod})
      for(node_id item: m ? db.as<n_module>(m).items : node_list{}) {
        if(!db.is<n_fn>(item) || db.strings.text(db.as<n_fn>(item).name) != name || db.as<n_fn>(item).params.size() != c.args.size()) continue;
        int score = 0;
        bool fits = true;
        auto const & ps = db.as<n_fn>(item).params;
        for(std::size_t k = 0; k < ps.size() && fits; ++k) {
          auto const & p = db.as<n_param>(ps[k]);
          if(!p.type || !db.is<n_ident>(p.type)) continue;
          auto const * o = obs(c.args[k]);
          if(p.mode == param_mode::expr || !o || !o->seen) {
            fits = false;
            continue;
          }
          auto want = db.strings.text(db.as<n_ident>(p.type).name);
          if(an.types.describe(o->v) != want && an.types.name(an.types.of(o->v)) != want) fits = false;
          else ++score;
        }
        if(!fits) continue;
        if(score > best) {
          best = score;
          target = item;
          tie = false;
        } else if(score == best) tie = true;
      }
    if(tie) throw jit_unsupported{n, cat("перегрузка ", name, " (неоднозначно для машинного кода)")};
    if(!target) throw jit_unsupported{n, cat("вызов ", name)};
    // Default semantic inlining (D10): the body is embedded in the context of the
    // call, the parameters are alias of the caller's seats, without ABI.
//,
    // 
    // 
    // . Recursion and outline are a real challenge.
    bool recursive = std::ranges::any_of(frames, [&](frame const & f) { return f.fn == target; });
    if(!recursive && !db.as<n_fn>(target).outline) return inline_call(n, c, target);
    std::vector<jt> types;
    for(node_id a: c.args) {
      jt t = type_of(a);
      if(t == jt::other || t == jt::unit) throw jit_unsupported{a, "тип аргумента"};
      types.push_back(t);
    }
    if(!tries.empty()) throw jit_unsupported{n, "настоящий вызов внутри try (M2a: только встраивание)"};
    jit_fn * callee = target == fn ? &out : eng.get(target, types);
    if(target == fn && types != out.params) callee = eng.get(target, types);
    if(!callee || (callee != &out && !callee->ok))
      throw jit_unsupported{n, cat(name, " не компилируется")};
    out.deps.insert(callee->deps.begin(), callee->deps.end());

    // arguments - in adjacent frame slots, the address of the first - in rdi
    uint32_t first = slots + 1;
    for(uint32_t i = 0; i < c.args.size(); ++i) alloc();
    int32_t base = -8 * int32_t(first + c.args.size() - 1);
    for(uint32_t i = 0; i < c.args.size(); ++i) {
      jt t = gen_value(c.args[i]);
      if(is_array(t)) { // the array is transmitted by a pointer to array_v
        code.load(rax, rbp, array_slot(c.args[i]).off);
      }
      to_result(t);
      code.store(rbp, base + int32_t(8 * i), rax);
    }
    code.lea(rdi, rbp, base);
    spill_xmm();
    bool pad = depth % 2 != 0;
    if(pad) code.sub_rsp(8);
    code.mov(rax, reinterpret_cast<uint64_t>(&callee->entry));
    code.call_mem(rax, 0);
    if(pad) code.add_rsp(8);
    reload_xmm();
    // Error in the code called - come out, the mark is already
    code.mov(rcx, reinterpret_cast<uint64_t>(&jit_fault));
    code.cmp_mem_imm8(rcx, 0, 0);
    code.jcc(ne, epilogue);
    if(callee->result == jt::f64_) code.movq(xmm0, rax);
    return callee->result;
  }

  jt inline_call(node_id n, n_call const & c, node_id target) {
    using namespace x64;
    auto const & g = db.as<n_fn>(target);
    context at{uint32_t(scopes.size()), fn_base, uint32_t(frames.size()), ctx};
    uint32_t ctx_index = uint32_t(contexts.size());
    contexts.push_back(at);
    // arguments: place - alias slot, expr - expression with context, other - calculated in your slot
    std::vector<std::pair<symbol, slot>> params;
    for(uint32_t i = 0; i < g.params.size(); ++i) {
      auto const & p = db.as<n_param>(g.params[i]);
      node_id arg = c.args[i];
      if(p.mode == param_mode::expr) {
        slot s;
        s.thunk = arg;
        s.ctx = ctx_index;
        params.push_back({p.name, s});
        continue;
      }
      if(p.mode != param_mode::own && db.is<n_ident>(arg)) {
        if(slot const * s = find(db.as<n_ident>(arg).name)) {
          slot alias = *s;
          alias.owner = false; // register belongs to the caller
          params.push_back({p.name, alias}); // alias: both reading and writing are caller cells
          continue;
        }
      }
      if(p.mode == param_mode::out || p.mode == param_mode::mut || an.param_written(target, i)) throw jit_unsupported{arg, "изменяемый параметр не на переменную (ссылка на элемент — интерпретатор)"};
      jt t = gen_value(arg);
      if(t == jt::other || t == jt::unit || is_array(t)) throw jit_unsupported{arg, cat("аргумент типа ", jt_name(t))};
      slot s;
      s.type = t;
      s.off = alloc();
      if(t == jt::f64_) code.movsd_store(rbp, s.off, xmm0);
      else code.store(rbp, s.off, rax);
      params.push_back({p.name, s});
    }
    auto const * o = obs(n);
    jt result = o && o->seen ? jt_of(o->v) : jt::unit;
    if(result == jt::other || is_array(result)) throw jit_unsupported{n, "тип результата встроенной функции"};
    out.deps[target.value] = db.node_revision(target);

    label * exit = &inline_exits.emplace_back();
    weigh(g.body, 1);
    scopes.emplace_back(std::move(params));
    uint32_t base = std::exchange(fn_base, uint32_t(scopes.size() - 1));
    uint64_t cx = std::exchange(ctx, interpreter::inline_context(ctx, n.value));
    frames.push_back({target, exit, result, depth, false});
    jt body = gen_value(g.body);
    if(body != result && body != jt::unit) throw jit_unsupported{n, cat("тип результата ", jt_name(body), " ≠ ", jt_name(result))};
    code.bind(*exit);
    frames.pop_back();
    fn_base = base;
    ctx = cx;
    pop_scopes(at.scopes);
    return result;
  }

  jt gen(node_id n, n_if const & i) {
    using namespace x64;
    label else_, done;
    if(gen_value(i.cond) != jt::bool_) throw jit_unsupported{i.cond, "условие не bool"};
    code.test(rax, rax);
    code.jcc(e, else_);
    jt t = gen_value(i.then_);
    code.jmp(done);
    code.bind(else_);
    jt u = i.else_ ? gen_value(i.else_) : jt::unit;
    code.bind(done);
    if(t != u && t != jt::unit && u != jt::unit) throw jit_unsupported{n, "ветки разных типов (расщепление)"};
    return t == u ? t : jt::unit;
  }

  jt gen(node_id n, n_block const & b) {
    scopes.emplace_back();
    jt last = jt::unit;
    for(uint32_t k = 0; k < b.items.size(); ++k) {
      last = gen_value(b.items[k]);
      if(!(b.has_value && k + 1 == b.items.size())) last = jt::unit;
    }
    pop_scopes(scopes.size() - 1);
    return last;
  }

  jt gen(node_id n, n_var const & v) {
    using namespace x64;
    if(v.ref) throw jit_unsupported{n, "auto& (ссылка на элемент — интерпретатор)"};
    jt t = gen_value(v.init);
    if(t == jt::other || t == jt::unit || is_array(t)) throw jit_unsupported{n, cat("переменная типа ", jt_name(t))};
    slot s = declare(v.name, t);
    store_var(s);
    return jt::unit;
  }

  // return(f) / return(caller) - from the external in the embedding chain (§6.1): just
  // a transition to its output.
  jt gen(node_id n, n_return const & r) {
    uint32_t k = uint32_t(frames.size() - 1);
    if(r.target) {
      symbol name = db.as<n_ident>(r.target).name;
      bool to_caller = db.strings.text(name) == "caller";
      bool found = false;
      while(k > 0) {
        if(db.as<n_fn>(frames[k].fn).outline) throw jit_unsupported{n, "return через outline"};
        --k;
        if(to_caller || db.as<n_fn>(frames[k].fn).name == name) {
          found = true;
          break;
        }
      }
      if(!found) throw jit_unsupported{n, "return: цель вне машинного кода"};
    }
    frame fr = frames[k];
    jt t = r.value ? gen_value(r.value) : jt::unit;
    if(t != fr.result) throw jit_unsupported{n, "тип return"};
    if(fr.top) {
      to_result(t);
      code.jmp(epilogue); // epilogue restores rsp
    } else {
      if(depth > fr.depth) code.add_rsp(int32_t(8 * (depth - fr.depth))); // transient
      code.jmp(*fr.exit);
    }
    return jt::unit;
  }

  jt gen(node_id n, n_require const & r) {
    using namespace x64;
    if(gen_value(r.cond) != jt::bool_) throw jit_unsupported{r.cond, "require не bool"};
    code.test(rax, rax);
    code.jcc(e, fault(jit_fault_kind::require, n));
    return jt::unit;
  }

  // kernel cycle: break/continue - transition to the output/beginning of the nearest
  // loop in the embedding chain (construction block - part of the caller: break from
  // the body for leaves the loop inside for).
  jt gen(node_id n, n_loop const & l) {
    using namespace x64;
    label top, done;
    code.bind(top);
    loops.push_back({&top, &done, depth});
    gen_value(l.body);
    loops.pop_back();
    code.jmp(top);
    code.bind(done);
    return jt::unit;
  }

  jt gen(node_id n, n_break const &) { return jump_loop(n, true); }
  jt gen(node_id, n_hide const &) { return jt::unit; } // Hiding the name - only when compiling
  jt gen(node_id n, n_continue const &) { return jump_loop(n, false); }

  jt jump_loop(node_id n, bool brk) {
    if(loops.empty()) throw jit_unsupported{n, "break/continue вне цикла машинного кода"};
    loop_frame const & lf = loops.back();
    if(depth > lf.depth) code.add_rsp(int32_t(8 * (depth - lf.depth))); // transient
    code.jmp(brk ? *lf.done : *lf.top);
    return jt::unit;
  }

  // expr-parameter transmitted further than another design - up to the original expression.
  slot resolve_thunk(slot th) {
    while(th.thunk && db.is<n_ident>(th.thunk)) {
      symbol name = db.as<n_ident>(th.thunk).name;
      std::optional<slot> inner = in_context(th.ctx, [&]() -> std::optional<slot> {
        slot const * s = find(name);
        return s ? std::optional<slot>(*s) : std::nullopt;
      });
      if(!inner || !inner->thunk) break;
      th = *inner;
    }
    return th;
  }

  // Design block call: body(xs[k]) - the block is embedded in the context of who caused
  // the design, with parameters in its slots.
  jt call_block(node_id n, n_call const & c, slot th) {
    th = resolve_thunk(th);
    if(!db.is<n_lambda>(th.thunk) || !db.as<n_lambda>(th.thunk).block) throw jit_unsupported{n, "вызов expr-параметра, который не блок"};
    auto const & l = db.as<n_lambda>(th.thunk);
    if(l.params.size() != c.args.size()) throw jit_unsupported{n, "арность блока"};
    std::vector<std::pair<symbol, slot>> ps;
    for(uint32_t i = 0; i < c.args.size(); ++i) {
      symbol pname = db.as<n_param>(l.params[i]).name;
      jt t = gen_value(c.args[i]);
      if(t == jt::other || t == jt::unit || is_array(t)) throw jit_unsupported{c.args[i], cat("аргумент блока типа ", jt_name(t))};
      slot s = declare(symbol{}, t, std::max<uint32_t>(weights[pname.value], 1u << 19));
      store_var(s);
      s.owner = false; // The register is occupied by the call area, the block only sees it.
      ps.push_back({pname, s});
    }
    return in_context(th.ctx, [&] {
      scopes.emplace_back(std::move(ps));
      jt r = gen_value(l.body);
      pop_scopes(scopes.size() - 1);
      return r;
    });
  }

  // --- exceptions (§8.1 ) ---
  //
  // 
// The type of abandoned is known statically.
  // So catch is selected when compiling: throw = write value + jmp. Not caught in machine
  // code - output "second way" (jit_thrown), then catches the interpreter.
  //
  static std::string_view scalar_name(jt t) { return t == jt::i64_ ? "i64" : t == jt::f64_ ? "f64"
        : t == jt::bool_                                                                   ? "bool"
                                                                                           : ""; }

  jt gen(node_id n, n_throw const & t) {
    using namespace x64;
    jt ty = gen_value(t.value);
    if(scalar_name(ty).empty()) throw jit_unsupported{n, "throw не скаляра"};
    for(auto tf = tries.rbegin(); tf != tries.rend(); ++tf)
      for(auto & c: tf->catches)
        if(c.type.empty() || c.type == scalar_name(ty)) {
          if(c.binds) {
            if(c.var.type != ty) throw jit_unsupported{n, "тип catch"};
            store_var(c.var);
          }
          if(depth > tf->depth) code.add_rsp(int32_t(8 * (depth - tf->depth)));
          code.jmp(*c.at);
          ++throw_jumps;
          return jt::unit;
        }
    to_result(ty); // Not caught here: second way out
    code.mov(rcx, reinterpret_cast<uint64_t>(&jit_thrown));
    code.store(rcx, 0, rax);
    code.mov(rcx, reinterpret_cast<uint64_t>(&jit_thrown_type));
    code.store_imm32(rcx, 0, int32_t(ty));
    code.mov(rcx, reinterpret_cast<uint64_t>(&jit_fault_node));
    code.store_imm32(rcx, 0, int32_t(n.value));
    code.mov(rcx, reinterpret_cast<uint64_t>(&jit_fault));
    code.store_imm32(rcx, 0, int32_t(jit_fault_kind::thrown));
    code.jmp(epilogue);
    return jt::unit;
  }

  // intercept(тело, блоки) is the core of the library try/catch: blocks are n_pack
  // packs from catch(E e) { … } clauses.
  jt gen_intercept(node_id n, n_call const & c) {
    using namespace x64;
    if(c.args.size() != 2 || !db.is<n_ident>(c.args[1])) throw jit_unsupported{n, "intercept(тело, блоки)"};
    slot const * hs0 = find(db.as<n_ident>(c.args[1]).name);
    if(!hs0 || !hs0->thunk) throw jit_unsupported{n, "intercept: блоки не expr-параметр"};
    slot hs = resolve_thunk(*hs0);
    node_list handlers = db.is<n_pack>(hs.thunk) ? db.as<n_pack>(hs.thunk).items : node_list{hs.thunk};
    auto const * o = obs(n);
    jt result = o && o->seen ? jt_of(o->v) : jt::unit;
    if(result == jt::other || is_array(result)) throw jit_unsupported{n, "тип try"};
    try_frame tf{{}, depth};
    for(node_id h: handlers) {
      if(!db.is<n_lambda>(h) || db.as<n_lambda>(h).params.size() != 1) throw jit_unsupported{h, "catch не блок с параметром"};
      auto const & p = db.as<n_param>(db.as<n_lambda>(h).params[0]);
      catch_info ci;
      ci.type = p.type && db.is<n_ident>(p.type) ? std::string(db.strings.text(db.as<n_ident>(p.type).name)) : std::string();
      ci.at = &inline_exits.emplace_back();
      ci.binds = db.strings.text(p.name) != "_";
      if(ci.binds) {
        jt vt = ci.type == "i64" ? jt::i64_ : ci.type == "f64" ? jt::f64_
            : ci.type == "bool"                                ? jt::bool_
                                                               : jt::other;
        if(vt == jt::other) {
          // catch(auto e) or catch(агрегат e): The value of a type unknown in
          // machine code is only valid if the name is not used.
          uint32_t uses = 0;
          weigh_uses(db.as<n_lambda>(h).body, p.name, uses);
          if(uses) throw jit_unsupported{h, "catch использует значение, неизвестное машинному коду"};
          ci.binds = false;
        } else {
          ci.var.type = vt;
          ci.var.off = alloc();
        }
      }
      tf.catches.push_back(ci);
    }
    label * end = &inline_exits.emplace_back();
    tries.push_back(tf);
    jt b = gen_value(c.args[0]);
    tries.pop_back();
    if(b != result && b != jt::unit) throw jit_unsupported{n, "тип тела try"};
    code.jmp(*end);
    for(std::size_t k = 0; k < handlers.size(); ++k) {
      auto const & l = db.as<n_lambda>(handlers[k]);
      catch_info const & ci = tf.catches[k];
      code.bind(*ci.at);
      jt cb = in_context(hs.ctx, [&] {
        scopes.emplace_back();
        if(ci.binds) scopes.back().push_back({db.as<n_param>(l.params[0]).name, ci.var});
        jt r = gen_value(l.body);
        pop_scopes(scopes.size() - 1);
        return r;
      });
      if(cb != result && cb != jt::unit) throw jit_unsupported{handlers[k], "тип catch"};
      code.jmp(*end);
    }
    code.bind(*end);
    return result;
  }

  void weigh_uses(node_id n, symbol name, uint32_t & uses) const {
    if(!n) return;
    if(db.is<n_ident>(n) && db.as<n_ident>(n).name == name) ++uses;
    db.for_each_child(n, [&](node_id c) { weigh_uses(c, name, uses); });
  }

  jt gen(node_id n, n_ct const &) { throw jit_unsupported{n, "ct не свёрнут в константу"}; } // ct for instantiation, not in runtime

  // Operator chain: A tree selected by parsing in this context (§4.4).
  jt gen(node_id n, n_chain const &) {
    node_id t = an.chain_tree(n, ctx);
    if(!t) throw jit_unsupported{n, "цепочка операторов: дерево зависит от пути"};
    return gen_value(t);
  }

  jt gen(node_id n, auto const &) {
    throw jit_unsupported{n, cat("узел ", kind_name(db[n]))};
  }

  jit_engine & eng;
  ast_db & db;
  node_id fn;
  jit_fn & out;
  interpreter & an;
  x64::assembler code;
  x64::label epilogue;
  std::deque<stub> stubs; // tag addresses stable
  std::vector<std::vector<std::pair<symbol, slot>>> scopes;
  uint32_t slots = 0;
  uint32_t bce = 0; // stripped
  uint32_t depth = 0;
  uint32_t frame_patch = 0;
  uint32_t fn_base = 0; // first area of the current (built-in) function
  std::vector<try_frame> tries; // machine-coded
  struct loop_frame {
    x64::label * top;
    x64::label * done;
    uint32_t depth;
  };
  std::vector<loop_frame> loops; // loop-covering
  uint32_t throw_jumps = 0;
  uint32_t gpr_used = 0, xmm_used = 0; // occupied variable registers (pool bits)
  std::vector<std::pair<int8_t, int32_t>> live_xmm; // f64 in registers: (register, home slot)
  std::vector<std::pair<x64::reg, int32_t>> saved; // saved registers of caller
  std::unordered_map<uint32_t, uint32_t> weights; // Name weight: mentions × 8 per cycle level
  uint64_t ctx = 0; // Embedded context (as in parsing)
  std::vector<frame> frames;
  std::vector<context> contexts;
  std::deque<x64::label> inline_exits; // insertion
};

// Cheap syntactic filter to static parsing: what code generator obviously can’t do is not worth
// disassembling (parsing can calculate a lot with compile-time).
std::optional<std::pair<node_id, std::string>> jit_prefilter(ast_db const & db, node_id module, node_id n) {
  if(!n) return std::nullopt;
  if(db.is<n_str>(n) || (db.is<n_lambda>(n) && !db.as<n_lambda>(n).block) || db.is<n_try_compile>(n) || db.is<n_construct>(n) || db.is<n_hole>(n))
    return std::pair{n, cat("узел ", kind_name(db[n]))};
  if(db.is<n_fn>(n) && db[n].parent && !db.is<n_module>(db[n].parent)) return std::pair{n, std::string("локальная функция")};
  if(db.is<n_ct>(n)) return std::pair{n, std::string("ct — по месту вызова (канонический порядок)")};
  if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) {
    auto name = db.strings.text(db.as<n_ident>(db.as<n_call>(n).callee).name);
    bool known = name == "f64" || name == "i64" || name == "range" || name == "zero" || name == "typeof" || name == "is_index" || name == "intercept"; // pure: convolution or code generator
    for(node_id m: {db.prelude, module})
      for(node_id item: m ? db.as<n_module>(m).items : node_list{})
        known = known || (db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == name);
    for(node_id p = db[n].parent; p && !known; p = db[p].parent) // expr-parameter is called: block of construction
      if(db.is<n_fn>(p))
        for(node_id pn: db.as<n_fn>(p).params)
          known = known || db.as<n_param>(pn).name == db.as<n_ident>(db.as<n_call>(n).callee).name;
    if(!known) return std::pair{n, cat("вызов ", name)};
  }
  std::optional<std::pair<node_id, std::string>> found;
  db.for_each_child(n, [&](node_id c) { if (!found) found = jit_prefilter(db, module, c); });
  return found;
}

void jit_engine::compile(node_id fn, jit_fn & out) {
  out.deps[fn.value] = db.node_revision(fn);
  out.deps[mod.value] = db.node_revision(mod);
  if(auto bad = jit_prefilter(db, mod, db.as<n_fn>(fn).body)) {
    auto lc = locate(db.source(db[bad->first].where.source), db[bad->first].where.begin);
    out.reason = cat(bad->second, " (строка ", lc.line, ")");
    return;
  }
  // Static parsing instance: Types and compile-time node (value-approach)
  interpreter an(db, mod, {"jit"}, mode::check);
  an.set_fuel(200000);
  std::vector<value> shapes;
  for(jt t: out.params) {
    if(t == jt::other || t == jt::unit) {
      out.reason = "тип параметра";
      return;
    }
    shapes.push_back(jt_shape(t));
  }
  try {
    value r = an.analyze_instance(fn, shapes);
    out.result = jt_of(r);
    if(out.result == jt::arr_i64 || out.result == jt::arr_f64) {
      out.reason = "результат — массив (пока интерпретатор)";
      return;
    }
    if(out.result == jt::other) {
      out.reason = cat("тип результата ", r.is<split_v>() ? "расщеплён" : "не скалярный");
      return;
    }
    jit_codegen(*this, fn, out, an).run();
  } catch(jit_unsupported const & u) {
    auto lc = locate(db.source(db[u.node].where.source), db[u.node].where.begin);
    out.reason = cat(u.why, " (строка ", lc.line, ")");
  } catch(diagnostic const & d) {
    out.reason = cat("разбор: ", d.what());
  }
}

// Call instance by machine code from the interpreter (run mode).
std::optional<interpreter::flow> interpreter::try_jit(node_id f, std::vector<value> & vals, node_list const & args,
    scope * env, std::string const & key) {
  // expr/own/out and variable scalars - semantics of inline/feedback while interpreter;
  // variable array possible: machine code receives an array of caller
  for(auto const & v: vals)
    if(v.is<thunk_v>()) return std::nullopt;
  auto const & params = db.as<n_fn>(f).params;
  for(uint32_t i = 0; i < params.size(); ++i) {
    auto m = db.as<n_param>(params[i]).mode;
    if(m == param_mode::expr || m == param_mode::own || m == param_mode::out) return std::nullopt;
    // scalar that the function changes: there is no return entry from the interpreter
    if(!vals[i].is<array_v>() && writes_param(f, i)) return std::nullopt;
  }
  std::vector<jt> param_types;
  for(auto const & v: vals) {
    jt t = jt_of(v);
    if(t == jt::other || t == jt::unit) return std::nullopt;
    param_types.push_back(t);
  }
  jit_fn * code = jit->get(f, param_types);
  if(!code) return std::nullopt;

  std::vector<uint64_t> raw(vals.size());
  for(uint32_t i = 0; i < vals.size(); ++i) {
    value const & v = vals[i];
    if(v.is<int64_t>()) raw[i] = uint64_t(v.as<int64_t>());
    else if(v.is<bool>()) raw[i] = v.as<bool>();
    else if(v.is<double>()) std::memcpy(&raw[i], &v.as<double>(), 8);
    else { // array: pointer to array_v caller (alias) or to a copy
      value * cell = i < args.size() ? place_or_null(args[i], env) : nullptr;
      raw[i] = reinterpret_cast<uint64_t>(cell && cell->is<array_v>() ? &cell->as<array_v>() : &vals[i].as<array_v>());
    }
  }
  jit_fault = 0;
  auto entry = reinterpret_cast<uint64_t (*)(uint64_t const *)>(code->entry);
  uint64_t bits = entry(raw.data());
  auto fault = jit_fault_kind(jit_fault);
  // No Deoptimizations: Type - Contract for the entire program
// The mismatch is a VM error.
  if(fault == jit_fault_kind::thrown) { // Exception by the second way - catches the interpreter
    node_id at{uint32_t(jit_fault_node)};
    jit_fault = 0;
    jt ty = jt(jit_thrown_type);
    value v = ty == jt::f64_ ? value(std::bit_cast<double>(jit_thrown)) : ty == jt::bool_ ? value(jit_thrown != 0)
                                                                                          : value(int64_t(jit_thrown));
    throw_stack = stack;
    return flow{flow_kind::thr, std::move(v), at.value};
  }
  if(fault != jit_fault_kind::none) {
    node_id at{uint32_t(jit_fault_node)};
    jit_fault = 0;
    if(fault == jit_fault_kind::layout)
      throw diagnostic("внутренняя ошибка JIT: раскладка массива не совпала с типом экземпляра", db[at].where);
    char const * what = fault == jit_fault_kind::bounds ? "индекс вне границ массива (машинный код)"
        : fault == jit_fault_kind::div_zero             ? "деление на ноль (машинный код)"
                                                        : "нарушено требование require (машинный код)";
    char const * type = fault == jit_fault_kind::bounds ? "out_of_bounds" : fault == jit_fault_kind::div_zero ? "div_by_zero"
                                                                                                              : "require_failed";
    fail_rt(type, what, db[at].where);
  }
  emit({report_event::instance, key, {}, {}, {}, {}});
  switch(code->result) {
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
