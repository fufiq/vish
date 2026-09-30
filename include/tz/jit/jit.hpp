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

#include <ranges>
#include <deque>
#include <set>

namespace tz {

// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// clang-format off
enum class jit_fault_kind : uint64_t { none = 0, bounds = 1, div_zero = 2, require = 3, layout = 4, thrown = 5, vm_error = 6 };
// clang-format on
uint64_t jit_fault = 0;
uint64_t jit_fault_node = 0;
// The exception that comes out of machine code is the "second way"
// value and its type.
uint64_t jit_thrown = 0;
uint64_t jit_thrown_type = 0; // jt
// Error or cancellation of fiber within the VM operation: The C++ exception does not go
// through the machine code - it waits here, the code comes out with the vm_error label.
std::exception_ptr jit_pending;

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

// An interpreter executing machine code (VM operations from code through it).
interpreter * jit_vm = nullptr;
uint64_t jit_stream_call(uint64_t op, uint64_t id, uint64_t bits, uint64_t t_node);

// clang-format off
enum class jt : uint8_t { unit, i64_, f64_, bool_, arr_i64, arr_f64, range_, stream_i64, stream_f64, stream_bool, other }; // range - range(n) : in the slot only n; stream is its number
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
    case jt::stream_i64: return "stream<i64>";
    case jt::stream_f64: return "stream<f64>";
    case jt::stream_bool: return "stream<bool>";
    default: return "?";
  }
}

bool is_stream(jt t) { return t == jt::stream_i64 || t == jt::stream_f64 || t == jt::stream_bool; }
jt elem_of(jt t) { return t == jt::stream_i64 ? jt::i64_ : t == jt::stream_f64 ? jt::f64_ : jt::bool_; }

jt jt_of(value const & v, interpreter const * in = nullptr) {
  value const * x = shape_of(v);
  if(!x || v.is<split_v>()) return jt::other;
  // stream: number in the slot, element type - from parsing (the execution stream - parsing of the entire program)
  if(in && x->is<agg_v>() && in->types.name(x->as<agg_v>().type) == "stream" && !x->as<agg_v>().fields.empty()) {
    auto const & fs = x->as<agg_v>().fields;
    value const & id = fs[0].second;
    value shape;
    if(id.is<int64_t>() && id.as<int64_t>() >= 0 && uint64_t(id.as<int64_t>()) < in->root->streams.size() && in->root->streams[std::size_t(id.as<int64_t>())].typed)
      shape = in->root->streams[std::size_t(id.as<int64_t>())].shape;
    else if(fs.size() > 1) shape = fs[1].second; // number erased by fusion - type of meaning
    else return jt::other;
    jt e = jt_of(shape);
    return e == jt::i64_ ? jt::stream_i64 : e == jt::f64_ ? jt::stream_f64 : e == jt::bool_ ? jt::stream_bool : jt::other;
  }
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
  x64::code_block code; // Objective - host: Executable pages
  void * entry = nullptr;
  // Aarch64 - qemu check: bytes and places of absolute addresses host
  std::vector<uint8_t> image;
  std::vector<std::pair<uint32_t, void const *>> relocs;
  std::vector<jt> params;
  jt result = jt::unit;
  bool ok = false;
  std::string reason; // why not compiled
  std::map<uint32_t, uint64_t> deps; // revision of functions included in the code
  std::size_t bytes = 0; // size
  uint32_t unchecked = 0; // indexation without border checks (proven by parsing)
  uint32_t jumps = 0; // throw that became a transition to catch (static exceptions)
  uint32_t lang_ops = 0; // operations whose code gives code generator in the language (target context)
  std::string lang_from; // from where code generator (modules)
  uint32_t lang_bytes = 0; // byte of code from code generator language (the rest - C++: addresses host, spare)
  bool by_lang = false; // all code - compiler in language (jit_compile)
  std::string lang_error; // The compiler on the language fell - the reason (code was given by gluing C++)
};

struct jit_unsupported {
  node_id node;
  std::string why;
};

// The instance engine for purpose A is an assembler with a common set of operations:
// code generation is one, the goal is the context that
// redefines operations.
template<typename A>
struct jit_machine {
  jit_machine(ast_db & db, node_id module) : db(db), mod(module) {}

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
      if(f->ok && f->by_lang) out += cat("  jit  ", name, "(", types_str(f->params), ") → ", f->bytes, " байт машинного кода, компилятор языка\n");
      else if(f->ok) out += cat("  jit  ", name, "(", types_str(f->params), ") → ", f->bytes, " байт машинного кода", f->unchecked ? cat(", без проверки границ: ", f->unchecked) : "", f->jumps ? cat(", исключений переходом: ", f->jumps) : "", f->lang_ops ? cat(", кодогенераторов языка: ", f->lang_ops, " (", f->lang_from, "), от C++: ", f->bytes - f->lang_bytes, " байт") : "", "\n");
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

  // Compiler in language (jit_compile prelude or programs): function code, purpose, types
  // → bytes. Empty - can not: gluing JIT in C++ (bootstrap).
  interpreter * an_for_lang = nullptr; // parsing instance: chain trees for compiler language
  bool lang_compile(node_id fn, jit_fn & out) {
    node_id lc;
    for(uint32_t i = db.size(); i-- > 0;)
      if(db.is<n_fn>(node_id(i)) && db.strings.text(db.as<n_fn>(node_id(i)).name) == "jit_compile") {
        lc = node_id(i);
        break;
      }
    if(!lc) return false;
    std::vector<value> types;
    for(jt t: out.params) types.push_back(value(std::string(jt_name(t))));
    value r;
    try {
      interpreter comp(db, mod, {"jit"}, mode::run);
      // Operator chain: the tree selected parsing instance (no priority - §4.4)
      std::vector<value> chains;
      auto walk = [&](this auto const & self, node_id n) -> void {
        if(db.is<n_chain>(n))
          if(node_id t = an_for_lang->chain_tree(n, 0)) chains.push_back(value(array_v::of({comp.code_value(n), comp.code_value(t)})));
        db.for_each_child(n, [&](node_id c) { self(c); });
      };
      walk(db.as<n_fn>(fn).body);
      r = comp.analyze_instance(lc, {value(std::string(A::target)), comp.code_value(fn), value(array_v::of(std::move(types))), value(std::string(jt_name(out.result))), value(array_v::of(std::move(chains)))});
    } catch(diagnostic const & d) {
      out.lang_error = d.what();
      return false;
    }
    if(!r.is<array_v>() || r.as<array_v>().size() == 0) return false;
    A code;
    for(uint32_t k = 0; k < r.as<array_v>().size(); ++k) code.byte(uint8_t(r.as<array_v>().get(k).as<int64_t>()));
    out.deps[lc.value] = db.node_revision(lc);
    out.bytes = code.size();
    out.lang_bytes = out.bytes;
    out.by_lang = true;
    finish(out, code);
    out.ok = true;
    return true;
  }
  ast_db & db;
  node_id mod;
  std::deque<jit_fn> fns; // instance arena (addresses stable)
  std::map<std::string, uint32_t> cache; // key instance → number in fns
};

// JIT host (x86-64): its code is executed by an interpreter.
struct jit_engine : jit_machine<x64::assembler> {
  using jit_machine::jit_machine;
};

// Finished code in instance: host are executable pages.
void finish(jit_fn & out, x64::assembler & a) {
  out.code = a.finalize();
  out.entry = out.code.entry;
}

// --- code generation one instance ---

template<typename A>
struct jit_codegen {
  jit_codegen(jit_machine<A> & eng, node_id fn, jit_fn & out, interpreter & analysis)
      : eng(eng), db(eng.db), fn(fn), out(out), an(analysis) {}

  void run() {
    using namespace x64;
    auto const & f = db.as<n_fn>(fn);
    psh(rbp);
    mv(rbp, rsp);
    frame_patch = frame_alloc();
    // registers of variables that the caller is obliged to save - in the frame slots
    for(auto r: gpr_pool) {
      int32_t off = alloc();
      saved.push_back({r, off});
      st(rbp, off, r);
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
      ld(rax, rdi, int32_t(8 * i));
      if(is_array(s.type)) {
        st(rbp, s.off, rax); // array_v * - always in the frame (transmitted further)
        arrays.push_back({s, i});
      } else {
        if(s.type == jt::f64_) mq(xmm0, rax);
        store_var(s);
      }
    }
    // Arrays: dense buffer and size - once at the input (the size in the code does not change)
    for(auto [s, i]: arrays) {
      ld(rdi, rbp, s.off);
      mv(rsi, uint64_t(s.type == jt::arr_i64 ? 1 : 2));
      call_native(reinterpret_cast<void *>(&jit_array_data));
      jmp_unless(fault(jit_fault_kind::layout, f.params[i])); // Type parsing guaranteed: we don't get here
      if(s.data_reg >= 0) mv(gpr_pool[s.data_reg], rax);
      else st(rbp, s.data_off, rax);
      ld(rdi, rbp, s.off);
      call_native(reinterpret_cast<void *>(&jit_array_size));
      st(rbp, s.size_off, rax);
    }

    frames.push_back({fn, &epilogue, out.result, 0, true});
    jt body = gen_value(f.body);
    if(!generators.empty()) throw jit_unsupported{generators.front().at, "spawn (файбер) — интерпретатор"};
    if(body != out.result && body != jt::unit)
      throw jit_unsupported{f.body, cat("тип результата ", jt_name(body), " ≠ ", jt_name(out.result))};
    to_result(body);

    bnd(epilogue);
    for(auto [r, off]: saved) ld(r, rbp, off);
    mv(rsp, rbp);
    pp(rbp);
    ret();
    for(auto & st: stubs) {
      emit_stub(st);
    }
    if(!lang_patches.empty()) throw jit_unsupported{lang_patches.front().n, "переход кодогенератора языка на непривязанную метку"};

    set_frame(frame_patch, (uint32_t(slots) * 8 + 15) & ~15u);
    out.bytes = code.size();
    out.unchecked = bce;
    out.jumps = throw_jumps;
    out.lang_ops = lang_ops;
    out.lang_bytes = lang_bytes;
    for(auto const & m: lang_from) out.lang_from += (out.lang_from.empty() ? "" : ", ") + m;
    finish(out, code);
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
      if(t == jt::i64_ || t == jt::bool_ || t == jt::range_ || is_stream(t)) s.reg = take_gpr(false);
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
    for(auto [x, off]: live_xmm) stf(x64::rbp, off, xmm_pool[x]);
  }
  void reload_xmm() {
    for(auto [x, off]: live_xmm) ldf(xmm_pool[x], x64::rbp, off);
  }

  // Variable = rax/xmm0 and back; in an arbitrary register - for the second operand.
  void load_gpr(slot const & s, x64::reg r) {
    if(s.reg >= 0) {
      if(gpr_pool[s.reg] != r) mv(r, gpr_pool[s.reg]);
    } else ld(r, x64::rbp, s.off);
  }
  void load_xmm(slot const & s, x64::xmm x) {
    if(s.xreg >= 0) mvf(x, xmm_pool[s.xreg]);
    else ldf(x, x64::rbp, s.off);
  }
  void load_var(slot const & s) {
    if(s.type == jt::f64_) load_xmm(s, x64::xmm0);
    else load_gpr(s, x64::rax);
  }
  void store_var(slot const & s) {
    if(s.type == jt::f64_) {
      if(s.xreg >= 0) mvf(xmm_pool[s.xreg], x64::xmm0);
      else stf(x64::rbp, s.off, x64::xmm0);
    } else if(s.reg >= 0) mv(gpr_pool[s.reg], x64::rax);
    else st(x64::rbp, s.off, x64::rax);
  }
  // Array Buffer Base: Variable Register or Scratch Load.
  x64::reg data_base(slot const & s, x64::reg scratch) {
    if(s.data_reg >= 0) return gpr_pool[s.data_reg];
    ld(scratch, x64::rbp, s.data_off);
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
    if(fuse && !fuse->in_b) throw jit_unsupported{n, "генератор может упасть: свёртка изменила бы, где ошибка"};
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
    bnd(st.at);
    if(st.to) {
      if(st.pop) stack_free(int32_t(8 * st.pop));
      jmp_to(*st.to);
      return;
    }
    code.mov_addr(rax, &jit_fault);
    store_const(rax, 0, int32_t(st.kind));
    code.mov_addr(rax, &jit_fault_node);
    store_const(rax, 0, int32_t(st.node.value));
    mv(rax, uint64_t(0));
    jmp_to(epilogue);
  }

  // Call runtime; align the stack by 16 before calling.
  void call_native(void * fn) {
    using namespace x64;
    spill_xmm();
    bool pad = depth % 2 != 0;
    if(pad) stack_alloc(8);
    code.mov_addr(rax, fn); // host is the boundary with host (C++): AArch64 has a relocation
    call_reg(rax);
    if(pad) stack_free(8);
    reload_xmm();
  }

  // Result: i64/bool/array - rax, f64 - xmm0. Rax for return.
  void to_result(jt t) {
    if(t == jt::f64_) mq(x64::rax, x64::xmm0);
  }

  void push_result(jt t) {
    if(t == jt::f64_) mq(x64::rax, x64::xmm0);
    psh(x64::rax);
    ++depth;
  }

  void pop_to(jt t, bool into_second) {
    using namespace x64;
    // second operand already in rax/xmm0 → transfer to rcx/xmm1, the first from the stack
    if(into_second) {
      if(t == jt::f64_) mvf(xmm1, xmm0);
      else mv(rcx, rax);
    }
    pp(rax);
    --depth;
    if(t == jt::f64_) mq(xmm0, rax);
  }

  // Observation of node in the current embedding context: Each call site has its own
  // copy of the body with its own types and compile-time.
  interpreter::observation const * obs(node_id n) const { return an.observed(n, ctx); }

  jt type_of(node_id n) const {
    interpreter::observation const * o = obs(n);
    return o && o->seen ? jt_of(o->v, &an) : jt::other;
  }

  // A constant if the value of node at parsing is known and pure ("top").
  bool try_constant(node_id n, jt & t) {
    using namespace x64;
    if(db.is<n_var>(n) || db.is<n_block>(n) || db.is<n_return>(n) || db.is<n_loop>(n) || db.is<n_break>(n) || db.is<n_continue>(n)) return false;
    if(db.is<n_op>(n)) {
      auto op = db.strings.text(db.as<n_op>(n).op);
      if(op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") return false;
    }
    interpreter::observation const * o = obs(n);
    if(!o || !o->seen || o->effect || o->diverted || !o->v.known()) return false;
    if(o->v.is<int64_t>()) {
      mv(rax, uint64_t(o->v.as<int64_t>()));
      t = jt::i64_;
      return true;
    }
    if(o->v.is<bool>()) {
      mv(rax, uint64_t(o->v.as<bool>()));
      t = jt::bool_;
      return true;
    }
    if(o->v.is<double>()) {
      uint64_t bits;
      double x = o->v.as<double>();
      std::memcpy(&bits, &x, 8);
      mv(rax, bits);
      mq(xmm0, rax);
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
    mv(x64::rax, uint64_t(x.value));
    return jt::i64_;
  }

  jt gen(node_id, n_float const & x) {
    uint64_t bits;
    std::memcpy(&bits, &x.value, 8);
    mv(x64::rax, bits);
    mq(x64::xmm0, x64::rax);
    return jt::f64_;
  }

  jt gen(node_id n, n_ident const & x) {
    slot const * s = find(x.name);
    if(!s) throw jit_unsupported{n, cat("имя ", db.strings.text(x.name), " вне кадра")};
    if(s->thunk) { // expr parameter: the caller expression is calculated at each mention (§6.1)
      slot th = *s;
      return in_context(th.ctx, [&] { return gen_value(th.thunk); });
    }
    if(is_array(s->type)) ld(x64::rax, x64::rbp, s->off); // array_v*
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
    ld(x64::rax, x64::rbp, array_slot(m.object).size_off);
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
    ld(rcx, rbp, s.size_off);
    bounds(rax, rcx, n);
  }

  jt gen(node_id n, n_index const & i) {
    using namespace x64;
    if(slot const * r = range_slot(i.object)) { // range: element k is k
      slot rs = *r;
      if(gen_value(i.index) != jt::i64_) throw jit_unsupported{i.index, "индекс не i64"};
      if(an.bounds_proven(n)) ++bce;
      else {
        load_gpr(rs, rcx);
        bounds(rax, rcx, n);
      }
      return jt::i64_;
    }
    slot s = array_slot(i.object);
    gen_checked_index(s, i.index, n);
    reg b = data_base(s, rcx);
    if(s.type == jt::arr_f64) {
      ldxf(xmm0, b, rax);
      return jt::f64_;
    }
    ldx(rax, b, rax);
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
        ng(rax);
        return t;
      }
      if(op == "-" && t == jt::f64_ && emit_lang(n, "prefix-", {t})) return t;
      if(op == "!" && t == jt::bool_ && emit_lang(n, "prefix!", {t})) return t;
      if(op == "-" && t == jt::f64_) {
        mv(rax, uint64_t(0x8000000000000000ull));
        mq(xmm1, rax);
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
      if(op == "&&") jmp_unless(done);
      else jmp_if(done);
      if(gen_value(o.args[1]) != jt::bool_) throw jit_unsupported{n, "логика не над bool"};
      bnd(done);
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
    interpreter::observation const * o = obs(n);
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
        mv(rdx, bits);
        mq(xmm1, rdx);
        return jt::f64_;
      }
      mv(rcx, v.is<bool>() ? uint64_t(v.as<bool>()) : uint64_t(v.as<int64_t>()));
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
    if(db.is<n_int>(i.index)) mv(rdx, uint64_t(db.as<n_int>(i.index).value));
    else load_gpr(*find(db.as<n_ident>(i.index).name), rdx);
    if(an.bounds_proven(n)) ++bce;
    else {
      ld(rcx, rbp, a.size_off);
      bounds(rdx, rcx, n);
    }
    reg b = data_base(a, rcx);
    if(a.type == jt::arr_f64) {
      ldxf(xmm1, b, rdx);
      return jt::f64_;
    }
    ldx(rcx, b, rdx);
    return jt::i64_;
  }

  // code generator language operations: in the target context function (amd64, arm64),
  // the redefinition of `using operator` with operand types is its value, byte array,
  // and there is machine code above the JIT register model (§10.4):
// 
// 
// 
//.
  // code generator is taken from the database: the new one is added by the program,
  // without building VM. code generator entity in the context of a goal: `using operator`
  // (operations) or context function (jump, jump_unless) with these types of parameters.
  std::map<std::string, node_id> codegen_cache; // entity and types → code generator (or empty)
  std::map<std::pair<uint32_t, std::vector<int64_t>>, std::vector<uint8_t>> bytes_cache;

  node_id find_codegen(std::string_view entity, std::vector<jt> const & types) {
    std::string key(entity);
    for(jt t: types) key += cat("|", jt_name(t));
    if(auto it = codegen_cache.find(key); it != codegen_cache.end()) return it->second;
    node_id found = find_codegen_in_db(entity, types);
    codegen_cache[key] = found;
    return found;
  }

  node_id find_codegen_in_db(std::string_view entity, std::vector<jt> const & types) {
    std::vector<std::string> names;
    for(jt t: types) names.push_back(std::string(jt_name(t)));
    node_id s = tz::find_codegen(db, A::target, entity, names).first;
    if(!s) return {};
    out.deps[s.value] = db.node_revision(s); // editing code generator - recompilation
    node_id m = s;
    while(db[m].parent) m = db[m].parent;
    std::string from = db.is<n_module>(m) && db.as<n_module>(m).name ? std::string(db.strings.text(db.as<n_module>(m).name)) : std::string("программа");
    if(!std::ranges::contains(lang_from, from)) lang_from.push_back(from);
    return s;
  }


  // The value of code generator is bytes. Operands are the numbers of the target registers (the first
  // is the battery, the second is the register of the second operand); label is the displacement from
  // the beginning of its bytes (label value: the transition is encoded by code generator itself). The
  // name of the tag is from the environment code generator: `fault` (trap) or `target` (transition).
  std::vector<uint8_t> run_codegen(node_id n, node_id s, std::vector<int64_t> const & args, std::string_view label_name, int64_t off) {
    std::vector<int64_t> key = args;
    key.push_back(off);
    if(auto it = bytes_cache.find({s.value, key}); it != bytes_cache.end()) return it->second;
    scope * regs = an.new_scope(an.globals);
    interpreter::scope_hold hold{an, regs};
    // JIT register model - environment names code generator: battery and second operand register
    regs->declare(an.sym("acc"), value(int64_t(A::code_reg(x64::rax))));
    regs->declare(an.sym("second"), value(int64_t(A::code_reg(x64::rcx))));
    regs->declare(an.sym("acc_f"), value(int64_t(A::code_xreg(x64::xmm0))));
    regs->declare(an.sym("second_f"), value(int64_t(A::code_xreg(x64::xmm1))));
    auto const & ps = db.as<n_fn>(s).params;
    for(std::size_t k = 0; k < ps.size(); ++k) regs->declare(db.as<n_param>(ps[k]).name, value(args[k]));
    if(!label_name.empty()) regs->declare(an.sym(label_name), value(off));
    auto r = an.eval(db.as<n_fn>(s).body, regs, 0);
    value const & v = r.v;
    if(!r.normal() || !v.is<array_v>()) throw jit_unsupported{n, cat("кодогенератор ", A::target, ": ожидался массив байтов")};
    std::vector<uint8_t> bytes;
    for(uint32_t k = 0; k < v.as<array_v>().size(); ++k) {
      value b = v.as<array_v>().get(k);
      if(!b.is<int64_t>() || b.as<int64_t>() < 0 || b.as<int64_t>() > 255) throw jit_unsupported{n, cat("кодогенератор ", A::target, ": байт вне 0..255")};
      bytes.push_back(uint8_t(b.as<int64_t>()));
    }
    bytes_cache[{s.value, key}] = bytes;
    return bytes;
  }

  // Operands: the first is the battery, the second is the register of the second operand.
  std::vector<int64_t> operand_regs(std::vector<jt> const & types) const {
    std::vector<int64_t> r;
    for(std::size_t k = 0; k < types.size(); ++k)
      r.push_back(types[k] == jt::f64_ ? A::code_xreg(k ? x64::xmm1 : x64::xmm0) : A::code_reg(k ? x64::rcx : x64::rax));
    return r;
  }

  // code generator in language: bytes to code; tag not yet tied - second real-shift call
  // when it is tied (bind_lang), with the same length.
  struct lang_patch {
    uint32_t at, size;
    node_id n, codegen;
    std::vector<int64_t> args;
    std::string label_name;
    x64::label * target;
  };
  std::vector<lang_patch> lang_patches;

  bool emit_lang(node_id n, std::string_view entity, std::vector<jt> const & types, std::string_view label_name = {}, x64::label * target = nullptr) {
    return emit_lang_args(n, entity, types, operand_regs(types), label_name, target);
  }

  bool emit_lang_args(node_id n, std::string_view entity, std::vector<jt> const & types, std::vector<int64_t> const & args, std::string_view label_name = {}, x64::label * target = nullptr) {
    node_id s = find_codegen(entity, types);
    if(!s) return false;
    uint32_t at = code.size();
    bool bound = target && target->pos >= 0;
    auto bytes = run_codegen(n, s, args, label_name, bound ? int64_t(target->pos) - int64_t(at) : 0);
    for(uint8_t b: bytes) code.byte(b);
    ++lang_ops;
    lang_bytes += uint32_t(bytes.size());
    if(target && !bound) lang_patches.push_back({at, uint32_t(bytes.size()), n, s, args, std::string(label_name), target});
    return true;
  }

  // Transfers and memory - code generator of the target context (move, load, store and
  // their f64 variants): registers are target encoding numbers, offset is a number.
  void mv(x64::reg d, x64::reg r) {
    if(!emit_lang_args(fn, "move", {jt::i64_, jt::i64_}, {A::code_reg(d), A::code_reg(r)})) code.mov(d, r);
  }
  void mv(x64::reg d, uint64_t imm) {
    if(!emit_lang_args(fn, "load_const", {jt::i64_, jt::i64_}, {A::code_reg(d), int64_t(imm)})) code.mov(d, imm);
  }
  // array element: [base + index * 8]
  void ldx(x64::reg d, x64::reg base, x64::reg index) {
    if(!emit_lang_args(fn, "load_index", {jt::i64_, jt::i64_, jt::i64_}, {A::code_reg(d), A::code_reg(base), A::code_reg(index)})) code.load_idx(d, base, index);
  }
  void stx(x64::reg base, x64::reg index, x64::reg r) {
    if(!emit_lang_args(fn, "store_index", {jt::i64_, jt::i64_, jt::i64_}, {A::code_reg(base), A::code_reg(index), A::code_reg(r)})) code.store_idx(base, index, r);
  }
  void ldxf(x64::xmm d, x64::reg base, x64::reg index) {
    if(!emit_lang_args(fn, "load_index_f64", {jt::i64_, jt::i64_, jt::i64_}, {A::code_xreg(d), A::code_reg(base), A::code_reg(index)})) code.movsd_load_idx(d, base, index);
  }
  void stxf(x64::reg base, x64::reg index, x64::xmm r) {
    if(!emit_lang_args(fn, "store_index_f64", {jt::i64_, jt::i64_, jt::i64_}, {A::code_reg(base), A::code_reg(index), A::code_xreg(r)})) code.movsd_store_idx(base, index, r);
  }
  // time stack (except for a couple of frames in the prologue), transformations, forwarding gprеxmm
  void psh(x64::reg r) {
    if(r == x64::rbp ? !emit_lang_args(fn, "frame_enter", {}, {}) : !emit_lang_args(fn, "push_tmp", {jt::i64_}, {A::code_reg(r)})) code.push(r);
  }
  void pp(x64::reg r) {
    if(r == x64::rbp ? !emit_lang_args(fn, "frame_leave", {}, {}) : !emit_lang_args(fn, "pop_tmp", {jt::i64_}, {A::code_reg(r)})) code.pop(r);
  }
  void stack_alloc(int32_t n) {
    if(!emit_lang_args(fn, "stack_alloc", {jt::i64_}, {n})) code.sub_rsp(n);
  }
  void stack_free(int32_t n) {
    if(!emit_lang_args(fn, "stack_free", {jt::i64_}, {n})) code.add_rsp(n);
  }
  void cvt_f(x64::xmm d, x64::reg r) {
    if(!emit_lang_args(fn, "to_f64", {jt::i64_, jt::i64_}, {A::code_xreg(d), A::code_reg(r)})) code.cvtsi2sd(d, r);
  }
  void cvt_i(x64::reg d, x64::xmm r) {
    if(!emit_lang_args(fn, "to_i64", {jt::i64_, jt::i64_}, {A::code_reg(d), A::code_xreg(r)})) code.cvttsd2si(d, r);
  }
  void mq(x64::xmm d, x64::reg r) {
    if(!emit_lang_args(fn, "move_to_f64", {jt::i64_, jt::i64_}, {A::code_xreg(d), A::code_reg(r)})) code.movq(d, r);
  }
  void mq(x64::reg d, x64::xmm r) {
    if(!emit_lang_args(fn, "move_from_f64", {jt::i64_, jt::i64_}, {A::code_reg(d), A::code_xreg(r)})) code.movq(d, r);
  }
  void ng(x64::reg r) {
    if(!emit_lang_args(fn, "negate", {jt::i64_}, {A::code_reg(r)})) code.neg(r);
  }

  // Tags, transitions, frames, calls - code generator of the context of the goal.
  void bnd(x64::label & l) { // tie the label and add language transitions to it
    code.bind(l);
    bind_lang(l);
  }
  void jmp_to(x64::label & l) {
    if(!emit_lang(fn, "jump", {}, "target", &l)) code.jmp(l);
  }
  void jmp_unless(x64::label & l) { // Bool in the battery lies - transition
    using namespace x64;
    if(!emit_lang(fn, "jump_unless", {jt::bool_}, "target", &l)) {
      code.test(rax, rax);
      code.jcc(e, l);
    }
  }
  void jmp_if(x64::label & l) { // Bool in the battery truth - transition
    using namespace x64;
    if(!emit_lang(fn, "jump_if", {jt::bool_}, "target", &l)) {
      code.test(rax, rax);
      code.jcc(ne, l);
    }
  }
  void jmp_if_set(x64::reg base, x64::label & l) { // qword [base] ка 0 - transition (VM error mark)
    using namespace x64;
    if(!emit_lang_args(fn, "jump_if_set", {jt::i64_}, {A::code_reg(base)}, "target", &l)) {
      code.cmp_mem_imm8(base, 0, 0);
      code.jcc(ne, l);
    }
  }
  void store_const(x64::reg base, int32_t disp, int32_t v) {
    if(!emit_lang_args(fn, "store_const", {jt::i64_, jt::i64_, jt::i64_}, {A::code_reg(base), disp, v})) code.store_imm32(base, disp, v);
  }
  void address(x64::reg d, x64::reg base, int32_t disp) {
    if(!emit_lang_args(fn, "address", {jt::i64_, jt::i64_, jt::i64_}, {A::code_reg(d), A::code_reg(base), disp})) code.lea(d, base, disp);
  }
  void call_reg(x64::reg r) {
    if(!emit_lang_args(fn, "call_reg", {jt::i64_}, {A::code_reg(r)})) code.call(r);
  }
  void call_mem(x64::reg base, int32_t disp) {
    if(!emit_lang_args(fn, "call_mem", {jt::i64_, jt::i64_}, {A::code_reg(base), disp})) code.call_mem(base, disp);
  }
  void ret() {
    if(!emit_lang_args(fn, "ret", {}, {})) code.ret();
  }
  // frame: size known at the end - second code generator call with the same byte size
  struct frame_site {
    uint32_t at = 0, size = 0;
    node_id codegen;
  } frame_lang;
  uint32_t frame_alloc() {
    node_id s = find_codegen("frame_alloc", {jt::i64_});
    if(!s) return code.frame_alloc();
    uint32_t at = code.size();
    auto bytes = run_codegen(fn, s, {0}, {}, 0);
    for(uint8_t b: bytes) code.byte(b);
    ++lang_ops;
    lang_bytes += uint32_t(bytes.size());
    frame_lang = {at, uint32_t(bytes.size()), s};
    return at;
  }
  void set_frame(uint32_t at, uint32_t bytes) {
    if(!frame_lang.codegen) return code.set_frame(at, bytes);
    auto b = run_codegen(fn, frame_lang.codegen, {int64_t(bytes)}, {}, 0);
    if(b.size() != frame_lang.size) throw jit_unsupported{fn, cat("кодогенератор ", A::target, ": длина frame_alloc зависит от размера")};
    for(uint32_t k = 0; k < frame_lang.size; ++k) code.patch(frame_lang.at + k, b[k]);
  }

  // boundary check: k < n without a sign, otherwise - trap out_of_bounds (label `fault`)
  void bounds(x64::reg k, x64::reg size, node_id n) {
    using namespace x64;
    x64::label & f = fault(jit_fault_kind::bounds, n);
    if(!emit_lang_args(n, "check_index", {jt::i64_, jt::i64_}, {A::code_reg(k), A::code_reg(size)}, "fault", &f)) {
      code.cmp(k, size);
      code.jcc(ae, f);
    }
  }
  void ld(x64::reg d, x64::reg base, int32_t disp) {
    if(!emit_lang_args(fn, "load", {jt::i64_, jt::i64_, jt::i64_}, {A::code_reg(d), A::code_reg(base), disp})) code.load(d, base, disp);
  }
  void st(x64::reg base, int32_t disp, x64::reg r) {
    if(!emit_lang_args(fn, "store", {jt::i64_, jt::i64_, jt::i64_}, {A::code_reg(base), disp, A::code_reg(r)})) code.store(base, disp, r);
  }
  void mvf(x64::xmm d, x64::xmm r) {
    if(!emit_lang_args(fn, "move_f64", {jt::i64_, jt::i64_}, {A::code_xreg(d), A::code_xreg(r)})) code.movsd(d, r);
  }
  void ldf(x64::xmm d, x64::reg base, int32_t disp) {
    if(!emit_lang_args(fn, "load_f64", {jt::i64_, jt::i64_, jt::i64_}, {A::code_xreg(d), A::code_reg(base), disp})) code.movsd_load(d, base, disp);
  }
  void stf(x64::reg base, int32_t disp, x64::xmm r) {
    if(!emit_lang_args(fn, "store_f64", {jt::i64_, jt::i64_, jt::i64_}, {A::code_reg(base), disp, A::code_xreg(r)})) code.movsd_store(base, disp, r);
  }

  void bind_lang(x64::label & l) { // The tag is tied: add transitions to it
    for(std::size_t k = 0; k < lang_patches.size();) {
      auto const & p = lang_patches[k];
      if(p.target != &l) {
        ++k;
        continue;
      }
      auto bytes = run_codegen(p.n, p.codegen, p.args, p.label_name, int64_t(l.pos) - int64_t(p.at));
      if(bytes.size() != p.size) throw jit_unsupported{p.n, cat("кодогенератор ", A::target, ": длина зависит от смещения перехода")};
      for(uint32_t i = 0; i < p.size; ++i) code.patch(p.at + i, bytes[i]);
      lang_patches.erase(lang_patches.begin() + std::ptrdiff_t(k));
    }
  }

  // The first operand is in rax/xmm0, the second in rcx/xmm1.
  jt binary(node_id n, std::string_view op, jt t) {
    using namespace x64;
    if(t == jt::i64_ && (op == "/" || op == "%") && find_codegen(cat("infix", op), {t, t})) { // trap
      emit_lang(n, cat("infix", op), {t, t}, "fault", &fault(jit_fault_kind::div_zero, n));
      return t;
    }
    if((t == jt::i64_ && op != "/" && op != "%") || t == jt::f64_)
      if(emit_lang(n, cat("infix", op), {t, t})) {
        bool cmp = op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
        return cmp ? jt::bool_ : t;
      }
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
        if(op == "/") ng(rax);
        else mv(rax, uint64_t(0));
        code.jmp(done);
        bnd(normal);
        code.cqo();
        code.idiv(rcx);
        if(op == "%") mv(rax, rdx);
        bnd(done);
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
      shared_write(cell, n);
      jt t = gen_value(o.args[1]);
      if(t != s->type) throw jit_unsupported{n, "тип присваивания"};
      if(op != "=") { // the right part is the second operand (rcx/xmm1), the current one is the first; no stack
        if(t == jt::f64_) mvf(xmm1, xmm0);
        else mv(rcx, rax);
        load_var(*s);
        binary(n, op.substr(0, 1), t);
      }
      store_var(*s);
      return;
    }
    if(db.is<n_index>(target)) {
      auto const & ix = db.as<n_index>(target);
      slot s = array_slot(ix.object);
      shared_write(s, n);
      jt et = s.type == jt::arr_f64 ? jt::f64_ : jt::i64_;
      if(gen_value(o.args[1]) != et) throw jit_unsupported{n, "тип элемента"};
      push_result(et); // [rsp+8] - right side (after index)
      gen_checked_index(s, ix.index, target); // rax - index
      psh(rax); // [rsp] - index
      ++depth;
      if(op != "=") {
        reg b = data_base(s, rcx);
        if(et == jt::f64_) {
          ldxf(xmm0, b, rax);
          ld(r8, rsp, 8);
          mq(xmm1, r8);
        } else {
          ldx(rax, b, rax);
          ld(rcx, rsp, 8);
        }
        binary(n, op.substr(0, 1), et);
      } else if(et == jt::f64_) {
        ld(rax, rsp, 8);
        mq(xmm0, rax);
      } else {
        ld(rax, rsp, 8);
      }
      pp(rdx);
      stack_free(8);
      depth -= 2;
      reg b = data_base(s, rcx);
      if(et == jt::f64_) stxf(b, rdx, xmm0);
      else stx(b, rdx, rax);
      return;
    }
    throw jit_unsupported{target, "цель присваивания"};
  }

  // The second operand from the stack is rcx/xmm1, without touching the first (rax/xmm0).
  void pop_second(jt t) {
    using namespace x64;
    if(t == jt::f64_) {
      pp(rax);
      mq(xmm1, rax);
    } else {
      pp(rcx);
    }
    --depth;
  }

  jt gen(node_id n, n_call const & c) {
    using namespace x64;
    if(!db.is<n_ident>(c.callee)) throw jit_unsupported{n, "вызов не по имени"};
    auto name = db.strings.text(db.as<n_ident>(c.callee).name);
    if(name == "f64" && c.args.size() == 1) {
      if(gen_value(c.args[0]) != jt::i64_) throw jit_unsupported{n, "f64(не i64)"};
      cvt_f(xmm0, rax);
      return jt::f64_;
    }
    if(name == "i64" && c.args.size() == 1) {
      if(gen_value(c.args[0]) != jt::f64_) throw jit_unsupported{n, "i64(не f64)"};
      cvt_i(rax, xmm0);
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
      else ld(rcx, rbp, array_slot(c.args[1]).size_off);
      if(!emit_lang(n, "is_index", {jt::i64_, jt::i64_})) {
        code.cmp(rax, rcx);
        code.setcc(b, rax);
        code.movzx_rax_al();
      }
      return jt::bool_;
    }
    if(name == "intercept") return gen_intercept(n, c);
    if(name.starts_with("stream_") || (name == "yield" && c.args.empty())) return gen_stream(n, c, name);
    if(name == "fiber_start") return gen_spawn(n, c);
    // Language function (prelude or module): single candidate by name and arnost, without requiring
    // candidates by name and arnost; typed parameters are cut off by argument types from parsing, with
    // equality more precisely - with a large number of types
    node_id target;
    int best = -1;
    bool tie = false;
    // overload - Table parsing : and from extensions (by name and link)
    node_list candidates;
    if(auto it = an.functions.find(db.as<n_ident>(c.callee).name); it != an.functions.end()) candidates.assign(it->second.begin(), it->second.end());
    for(node_id item: candidates) {
      if(!db.is<n_fn>(item) || db.as<n_fn>(item).params.size() != c.args.size()) continue;
      for(node_id p: db.as<n_fn>(item).params) {
        if(db.as<n_param>(p).type && db.is<n_pat>(db.as<n_param>(p).type)) throw jit_unsupported{n, cat("перегрузка ", name, " по образцу (рантайм-выбор)")};
        if(db.as<n_param>(p).pack && db.as<n_param>(p).mode == param_mode::let) throw jit_unsupported{n, cat(name, ": хвост аргументов ...xs")};
      }
      int score = 0;
      bool fits = true;
      auto const & ps = db.as<n_fn>(item).params;
      for(std::size_t k = 0; k < ps.size() && fits; ++k) {
        auto const & p = db.as<n_param>(ps[k]);
        if(!p.type || !db.is<n_ident>(p.type)) continue;
        interpreter::observation const * o = obs(c.args[k]);
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
    if(auto k = generator_of(target, c)) return gen_fused(n, c, *k);
    if(!recursive && !db.as<n_fn>(target).outline) return inline_call(n, c, target);
    if(fuse && !fuse->in_b) throw jit_unsupported{n, "генератор: настоящий вызов"};
    for(node_id a: c.args) stream_escapes(a);
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
        ld(rax, rbp, array_slot(c.args[i]).off);
      }
      to_result(t);
      st(rbp, base + int32_t(8 * i), rax);
    }
    address(rdi, rbp, base);
    spill_xmm();
    bool pad = depth % 2 != 0;
    if(pad) stack_alloc(8);
    code.mov_addr(rax, &callee->entry);
    call_mem(rax, 0);
    if(pad) stack_free(8);
    reload_xmm();
    // Error in the code called - come out, the mark is already
    code.mov_addr(rcx, &jit_fault);
    jmp_if_set(rcx, epilogue);
    if(callee->result == jt::f64_) mq(xmm0, rax);
    return callee->result;
  }

  // Flow operation / yield - call VM. fiber can be locked inside: switching "bottom", on
  // the fiber stack - the code around remains optimized.
  //
  jt gen_stream(node_id n, n_call const & c, std::string_view name) {
    using namespace x64;
    if(!tries.empty()) throw jit_unsupported{n, "операция потока внутри try"};
    uint64_t op = name == "stream_new" ? 0 : name == "stream_write" ? 1 : name == "stream_read" ? 2 : name == "stream_more" ? 3 : name == "stream_close" ? 4 : name == "yield" ? 5 : 9;
    if(op == 9) throw jit_unsupported{n, cat("вызов ", name)};
    if(fuse && !fuse->in_b) return sink(n, c, op);
    if(!c.args.empty()) stream_escapes(c.args[0]);
    jt result = jt::unit, carried = jt::unit;
    if(op == 0) {
      result = type_of(n);
      if(!is_stream(result)) throw jit_unsupported{n, "поток без типа элементов"};
      if(c.args.size() != 1 || gen_value(c.args[0]) != jt::i64_) throw jit_unsupported{n, "stream_new(ёмкость i64)"};
      mv(rdx, rax);
      mv(rsi, uint64_t(0));
    } else if(op != 5) {
      if(c.args.size() != (op == 1 ? 2u : 1u)) throw jit_unsupported{n, cat(name, ": аргументы")};
      jt st = gen_value(c.args[0]);
      if(!is_stream(st)) throw jit_unsupported{c.args[0], "не поток известного типа"};
      if(op == 1) {
        push_result(st);
        carried = gen_value(c.args[1]);
        if(carried != elem_of(st)) throw jit_unsupported{c.args[1], "тип записи ≠ типу потока"};
        to_result(carried);
        mv(rdx, rax);
        pp(rsi);
        --depth;
      } else mv(rsi, rax);
      if(op == 2) result = carried = elem_of(st);
      if(op == 3) result = jt::bool_;
    }
    mv(rdi, op);
    mv(rcx, (uint64_t(carried) << 32) | n.value);
    call_native(reinterpret_cast<void *>(&jit_stream_call));
    code.mov_addr(rcx, &jit_fault);
    jmp_if_set(rcx, epilogue); // Error/Cancel in VM: Exit, mark is worth
    if(result == jt::f64_) mq(xmm0, rax);
    return result;
  }

  // --- generator → cycle (O11) ---
  //
  // ; Pair s = stream(…) ; spawn { W } ; ... for(x : s) B - without fiber : in the place
  // for W is executed, where write(s, v) is B with x = v. *My:* convolution only when the
  // observed does not change - W is "pure" (from the actions of VM only the record in s
  // and the last close(s) , without errors, return/throw and write out), s is not used
  // anywhere else, B does not write in what W reads and does not leave the cycle; for - in
  // the same control area that spawn. fiber would have started on the first reading. The
  // function is an interpreter, as before.
  //
  //

  jt gen_spawn(node_id n, n_call const & c) {
    if(fuse) throw jit_unsupported{n, "spawn внутри генератора"};
    slot const * s = c.args.size() == 1 && db.is<n_ident>(c.args[0]) ? find(db.as<n_ident>(c.args[0]).name) : nullptr;
    if(!s || !s->thunk) throw jit_unsupported{n, "spawn"};
    generator g{resolve_thunk(*s), regions.back(), {}, n};
    std::vector<symbol> names;
    idents(g.w.thunk, names);
    in_context(g.w.ctx, [&] {
      for(symbol x: names)
        if(slot const * v = find(x)) g.captured.insert(v->off);
      return 0;
    });
    generators.push_back(std::move(g));
    return jt::unit;
  }
  void idents(node_id n, std::vector<symbol> & out) {
    if(db.is<n_ident>(n)) out.push_back(db.as<n_ident>(n).name);
    db.for_each_child(n, [&](node_id c) { idents(c, out); });
  }

  // for(stream s, expr body) on the stream that the waiting spawn sees.
  std::optional<std::size_t> generator_of(node_id target, n_call const & c) {
    if(generators.empty() || fuse || c.args.size() != 2 || !db.is<n_ident>(c.args[0])) return std::nullopt;
    auto const & f = db.as<n_fn>(target);
    if(db.strings.text(f.name) != "for" || f.params.size() != 2) return std::nullopt;
    auto const & p = db.as<n_param>(f.params[0]);
    if(!p.type || !db.is<n_ident>(p.type) || db.strings.text(db.as<n_ident>(p.type).name) != "stream") return std::nullopt;
    slot const * st = resolve(db.as<n_ident>(c.args[0]).name);
    if(!st || !is_stream(st->type)) return std::nullopt;
    for(std::size_t k = 0; k < generators.size(); ++k)
      if(generators[k].captured.contains(st->off)) return k;
    return std::nullopt;
  }

  jt gen_fused(node_id n, n_call const & c, std::size_t k) {
    slot st = *resolve(db.as<n_ident>(c.args[0]).name);
    generator g = generators[k];
    if(g.region != regions.back()) throw jit_unsupported{n, "for по потоку генератора — не в области spawn"};
    for(std::size_t j = 0; j < generators.size(); ++j)
      if(j != k && generators[j].captured.contains(st.off)) throw jit_unsupported{n, "поток видят несколько spawn"};
    node_id body = c.args[1];
    if(!db.is<n_lambda>(body) || !db.as<n_lambda>(body).block || db.as<n_lambda>(body).params.size() != 1) throw jit_unsupported{body, "тело for"};
    if(!consumer_stays(db.as<n_lambda>(body).body, 0)) throw jit_unsupported{body, "тело for выходит из цикла"};
    std::set<uint32_t> seen;
    if(!generator_tail(g.w.thunk, seen)) throw jit_unsupported{g.at, "генератор: close(s) — не последнее действие, или return/throw"};
    generators.erase(generators.begin() + std::ptrdiff_t(k));
    contexts.push_back({uint32_t(scopes.size()), fn_base, uint32_t(frames.size()), ctx}); // place for - context B
    fusion f{st.off, elem_of(st.type), body, uint32_t(contexts.size() - 1), scopes, frames, fn_base, ctx, {}, g.captured};
    in_context(g.w.ctx, [&] {
      for(auto const & sc: scopes)
        for(auto const & [name, s]: sc) f.outer.insert(s.off);
      return 0;
    });
    fuse = std::move(f);
    in_context(g.w.ctx, [&] {
      node_id e = g.w.thunk;
      if(db.is<n_lambda>(e)) e = db.as<n_lambda>(e).body;
      return gen_value(e);
    });
    if(fuse->closes != 1) throw jit_unsupported{g.at, "генератор: close(s) не один раз"};
    fuse.reset();
    // Flow after cycle - closed and empty, as after a real exchange
    using namespace x64;
    load_var(st);
    mv(rsi, rax);
    mv(rdi, uint64_t(4));
    mv(rcx, uint64_t(n.value));
    call_native(reinterpret_cast<void *>(&jit_stream_call));
    code.mov_addr(rcx, &jit_fault);
    jmp_if_set(rcx, epilogue);
    return jt::unit;
  }

  // Operation VM inside W: write(s, v) is the consumer's body with x = v; close(s) is the end.
  jt sink(node_id n, n_call const & c, uint64_t op) {
    slot const * sp = !c.args.empty() && db.is<n_ident>(c.args[0]) ? resolve(db.as<n_ident>(c.args[0]).name) : nullptr;
    if(op == 5 || !sp || sp->off != fuse->stream_off || (op != 1 && op != 4)) throw jit_unsupported{n, "генератор: другая операция VM"};
    if(op == 4) {
      ++fuse->closes;
      return jt::unit;
    }
    jt v = gen_value(c.args[1]);
    if(v != fuse->elem) throw jit_unsupported{c.args[1], "тип записи ≠ типу потока"};
    auto const & l = db.as<n_lambda>(fuse->body);
    symbol pname = db.as<n_param>(l.params[0]).name;
    slot p = declare(symbol{}, v, std::max<uint32_t>(weights[pname.value], 1u << 19));
    store_var(p);
    p.owner = false; // register
    // B - in areas of the place for; areas W for this time
    auto w_scopes = std::move(scopes);
    auto w_frames = std::move(frames);
    uint32_t w_base = fn_base;
    uint64_t w_ctx = ctx;
    scopes = fuse->full_scopes;
    frames = fuse->full_frames;
    fn_base = fuse->full_base;
    ctx = fuse->full_ctx;
    fuse->in_b = true;
    in_context(fuse->body_ctx, [&] {
      scopes.emplace_back(std::vector<std::pair<symbol, slot>>{{pname, p}});
      gen_value(l.body);
      pop_scopes(scopes.size() - 1);
      return 0;
    });
    fuse->in_b = false;
    scopes = std::move(w_scopes);
    frames = std::move(w_frames);
    fn_base = w_base;
    ctx = w_ctx;
    return jt::unit;
  }

  // A record that would make the order W and B observable.
  void shared_write(slot const & s, node_id n) {
    if(!fuse) { // after spawn: fiber could start earlier for (blocking between them) and read the old one
      for(auto const & g: generators)
        if(g.captured.contains(s.off)) throw jit_unsupported{n, "переменную генератора меняют после spawn"};
      return;
    }
    if(!fuse->in_b && fuse->outer.contains(s.off)) throw jit_unsupported{n, "генератор пишет наружу"};
    if(fuse->in_b && fuse->captured.contains(s.off)) throw jit_unsupported{n, "потребитель пишет то, что читает генератор"};
  }

  // The stream or array that the waiting spawn sees does not go through the for.
  void stream_escapes(node_id a) {
    if(!db.is<n_ident>(a) || (generators.empty() && !fuse)) return;
    slot const * s = resolve(db.as<n_ident>(a).name);
    if(!s || !(is_stream(s->type) || is_array(s->type))) return;
    if(fuse && fuse->in_b && s->off == fuse->stream_off) throw jit_unsupported{a, "потребитель трогает поток генератора"};
    for(auto const & g: generators)
      if(g.captured.contains(s->off)) throw jit_unsupported{a, "поток генератора используется не через for"};
  }

  // B does not leave the cycle for: no return, break/continue - only its cycles.
  bool consumer_stays(node_id n, uint32_t loops_in) {
    if(db.is<n_return>(n)) return false;
    if((db.is<n_break>(n) || db.is<n_continue>(n)) && loops_in == 0) return false;
    uint32_t inner = loops_in + (db.is<n_loop>(n) || (db.is<n_lambda>(n) && db.as<n_lambda>(n).block) ? 1 : 0);
    bool ok = true;
    db.for_each_child(n, [&](node_id c) { ok = ok && consumer_stays(c, inner); });
    return ok;
  }

  // Language functions by name and arnost (prelude and module).
  std::vector<node_id> fns_named(symbol name, std::size_t arity) {
    std::vector<node_id> out;
    for(node_id m: {db.prelude, eng.mod})
      for(node_id item: m ? db.as<n_module>(m).items : node_list{})
        if(db.is<n_fn>(item) && db.as<n_fn>(item).name == name && db.as<n_fn>(item).params.size() == arity) out.push_back(item);
    return out;
  }
  // W ends with close(s): the last action is close or the call that ends with it.
  bool generator_tail(node_id n, std::set<uint32_t> & seen) {
    if(db.is<n_lambda>(n)) return generator_tail(db.as<n_lambda>(n).body, seen);
    if(db.is<n_block>(n)) {
      auto const & items = db.as<n_block>(n).items;
      if(items.empty()) return false;
      for(std::size_t i = 0; i + 1 < items.size(); ++i)
        if(generator_escapes(items[i], seen)) return false;
      return generator_tail(items.back(), seen);
    }
    if(!db.is<n_call>(n) || !db.is<n_ident>(db.as<n_call>(n).callee)) return false;
    auto const & c = db.as<n_call>(n);
    symbol name = db.as<n_ident>(c.callee).name;
    if(db.strings.text(name) == "close") return true;
    for(node_id a: c.args)
      if(generator_escapes(a, seen)) return false;
    auto fs = fns_named(name, c.args.size());
    if(fs.size() != 1 || !seen.insert(fs[0].value).second) return false;
    return generator_tail(db.as<n_fn>(fs[0]).body, seen);
  }
  // return/throw/close outside the W tail (with evoked functions).
  bool generator_escapes(node_id n, std::set<uint32_t> & seen) {
    if(db.is<n_return>(n) || db.is<n_throw>(n)) return true;
    if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) {
      symbol name = db.as<n_ident>(db.as<n_call>(n).callee).name;
      if(db.strings.text(name) == "close") return true;
      for(node_id f: fns_named(name, db.as<n_call>(n).args.size()))
        if(seen.insert(f.value).second && generator_escapes(db.as<n_fn>(f).body, seen)) return true;
    }
    bool bad = false;
    db.for_each_child(n, [&](node_id c) { bad = bad || generator_escapes(c, seen); });
    return bad;
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
      if(p.mode == param_mode::out || p.mode == param_mode::mut || (p.mode != param_mode::own && an.param_written(target, i))) throw jit_unsupported{arg, "изменяемый параметр не на переменную (ссылка на элемент — интерпретатор)"};
      jt t = gen_value(arg);
      if(t == jt::other || t == jt::unit || is_array(t)) throw jit_unsupported{arg, cat("аргумент типа ", jt_name(t))};
      slot s;
      s.type = t;
      s.off = alloc();
      if(t == jt::f64_) stf(rbp, s.off, xmm0);
      else st(rbp, s.off, rax);
      params.push_back({p.name, s});
    }
    interpreter::observation const * o = obs(n);
    jt result = o && o->seen ? jt_of(o->v, &an) : jt::unit;
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
    bnd(*exit);
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
    if(!emit_lang(n, "jump_unless", {jt::bool_}, "target", &else_)) {
      code.test(rax, rax);
      code.jcc(e, else_);
    }
    jt t = in_region([&] { return gen_value(i.then_); });
    if(!emit_lang(n, "jump", {}, "target", &done)) code.jmp(done);
    bnd(else_);
    jt u = i.else_ ? in_region([&] { return gen_value(i.else_); }) : jt::unit;
    bnd(done);
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
    if(v.type && db.is<n_ident>(v.type)) { // T x = e is imaginable (§5.8)
      auto want = db.strings.text(db.as<n_ident>(v.type).name);
      // i64 → f64: literal (parsing checked for accuracy); run-time - value for 2^53 - conceivability test, it is not in machine code
      if(want == "f64" && t == jt::i64_ && db.is<n_int>(v.init)) {
        cvt_f(xmm0, rax);
        t = jt::f64_;
      } else if(want != "auto" && want != jt_name(t)) throw jit_unsupported{n, cat(want, " из ", jt_name(t))};
    }
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
      jmp_to(epilogue); // epilogue restores rsp
    } else {
      if(depth > fr.depth) stack_free(int32_t(8 * (depth - fr.depth))); // transient
      jmp_to(*fr.exit);
    }
    return jt::unit;
  }

  jt gen(node_id n, n_require const & r) {
    using namespace x64;
    if(gen_value(r.cond) != jt::bool_) throw jit_unsupported{r.cond, "require не bool"};
    jmp_unless(fault(jit_fault_kind::require, n));
    return jt::unit;
  }

  // kernel cycle: break/continue - transition to the output/beginning of the nearest
  // loop in the embedding chain (construction block - part of the caller: break from
  // the body for leaves the loop inside for).
  jt gen(node_id n, n_loop const & l) {
    using namespace x64;
    label top, done;
    bnd(top);
    loops.push_back({&top, &done, depth});
    in_region([&] { return gen_value(l.body); });
    loops.pop_back();
    if(!emit_lang(n, "jump", {}, "target", &top)) code.jmp(top);
    bnd(done);
    return jt::unit;
  }

  // A branch or a body of a cycle: the spawn inside it coagulates only with for in it.
  jt in_region(auto && f) {
    regions.push_back(++region_counter);
    jt r = f();
    uint32_t id = regions.back();
    regions.pop_back();
    for(auto const & g: generators)
      if(g.region == id) throw jit_unsupported{g.at, "spawn без for по его потоку в той же области"};
    return r;
  }

  jt gen(node_id n, n_break const &) { return jump_loop(n, true); }
  jt gen(node_id, n_hide const &) { return jt::unit; } // Hiding the name - only when compiling
  jt gen(node_id n, n_continue const &) { return jump_loop(n, false); }

  jt jump_loop(node_id n, bool brk) {
    if(loops.empty()) throw jit_unsupported{n, "break/continue вне цикла машинного кода"};
    loop_frame const & lf = loops.back();
    if(depth > lf.depth) stack_free(int32_t(8 * (depth - lf.depth))); // transient
    if(!emit_lang(n, "jump", {}, "target", brk ? lf.done : lf.top)) code.jmp(brk ? *lf.done : *lf.top);
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
          if(depth > tf->depth) stack_free(int32_t(8 * (depth - tf->depth)));
          jmp_to(*c.at);
          ++throw_jumps;
          return jt::unit;
        }
    to_result(ty); // Not caught here: second way out
    code.mov_addr(rcx, &jit_thrown);
    st(rcx, 0, rax);
    code.mov_addr(rcx, &jit_thrown_type);
    store_const(rcx, 0, int32_t(ty));
    code.mov_addr(rcx, &jit_fault_node);
    store_const(rcx, 0, int32_t(n.value));
    code.mov_addr(rcx, &jit_fault);
    store_const(rcx, 0, int32_t(jit_fault_kind::thrown));
    jmp_to(epilogue);
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
    interpreter::observation const * o = obs(n);
    jt result = o && o->seen ? jt_of(o->v, &an) : jt::unit;
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
    jmp_to(*end);
    for(std::size_t k = 0; k < handlers.size(); ++k) {
      auto const & l = db.as<n_lambda>(handlers[k]);
      catch_info const & ci = tf.catches[k];
      bnd(*ci.at);
      jt cb = in_context(hs.ctx, [&] {
        scopes.emplace_back();
        if(ci.binds) scopes.back().push_back({db.as<n_param>(l.params[0]).name, ci.var});
        jt r = gen_value(l.body);
        pop_scopes(scopes.size() - 1);
        return r;
      });
      if(cb != result && cb != jt::unit) throw jit_unsupported{handlers[k], "тип catch"};
      jmp_to(*end);
    }
    bnd(*end);
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

  jit_machine<A> & eng;
  ast_db & db;
  node_id fn;
  jit_fn & out;
  interpreter & an;
  A code;
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
  uint32_t lang_ops = 0;
  uint32_t lang_bytes = 0;
  std::vector<std::string> lang_from; // code generator language modules
  uint32_t gpr_used = 0, xmm_used = 0; // occupied variable registers (pool bits)
  std::vector<std::pair<int8_t, int32_t>> live_xmm; // f64 in registers: (register, home slot)
  std::vector<std::pair<x64::reg, int32_t>> saved; // saved registers of caller
  std::unordered_map<uint32_t, uint32_t> weights; // Name weight: mentions × 8 per cycle level
  uint64_t ctx = 0; // Embedded context (as in parsing)
  std::vector<frame> frames;
  std::vector<context> contexts;
  std::deque<x64::label> inline_exits; // insertion

  // Generator → cycle (O11): spawn waiting for its for; control areas (branches and
  // cycles) - so that for is executed exactly as many times as spawn.
  struct generator {
    slot w; // Spwn body (expr) and its context
    uint32_t region;
    std::set<int32_t> captured; // Slots of names the body sees outside
    node_id at;
  };
  std::vector<generator> generators;
  std::vector<uint32_t> regions{0};
  uint32_t region_counter = 0;
  // Walking Convolution: The body of the generator W, in which write(s, v) is the body of the consumer B.
  struct fusion {
    int32_t stream_off;
    jt elem;
    node_id body; // block for: (x) { B }
    uint32_t body_ctx;
    std::vector<std::vector<std::pair<symbol, slot>>> full_scopes; // field
    std::vector<frame> full_frames;
    uint32_t full_base;
    uint64_t full_ctx;
    std::set<int32_t> outer, captured; // W is visible from the outside; what W reads
    bool in_b = false;
    uint32_t closes = 0;
  };
  std::optional<fusion> fuse;
};

// Cheap syntactic filter to static parsing: what code generator obviously can’t do is not worth
// disassembling (parsing can calculate a lot with compile-time).
std::optional<std::pair<node_id, std::string>> jit_prefilter(ast_db const & db, node_id module, node_id n) {
  if(!n) return std::nullopt;
  if(db.is<n_str>(n) || (db.is<n_lambda>(n) && !db.as<n_lambda>(n).block) || db.is<n_try_compile>(n) || db.is<n_construct>(n) || db.is<n_hole>(n) || db.is<n_unpack>(n) || db.is<n_pat>(n) || db.is<n_spread>(n))
    return std::pair{n, cat("узел ", kind_name(db[n]))};
  if(db.is<n_fn>(n) && db[n].parent && !db.is<n_module>(db[n].parent)) return std::pair{n, std::string("локальная функция")};
  if(db.is<n_ct>(n)) return std::pair{n, std::string("ct — по месту вызова (канонический порядок)")};
  if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) {
    auto name = db.strings.text(db.as<n_ident>(db.as<n_call>(n).callee).name);
    bool known = name == "f64" || name == "i64" || name == "range" || name == "zero" || name == "typeof" || name == "is_index" || name == "intercept" // pure: convolution or code generator
        || name.starts_with("stream_") || name == "yield"; // Flow operations - VM call
    for(uint32_t i = 0; i < db.size() && !known; ++i) // functions of any database module (use)
      known = db.is<n_fn>(node_id(i)) && db.strings.text(db.as<n_fn>(node_id(i)).name) == name;
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

// Does the body create threads (call stream(…), including prelude through module functions).
bool jit_creates_streams(ast_db const & db, node_id n) {
  if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee) && db.strings.text(db.as<n_ident>(db.as<n_call>(n).callee).name) == "stream") return true;
  bool found = false;
  db.for_each_child(n, [&](node_id c) { found = found || jit_creates_streams(db, c); });
  return found;
}

template<typename A>
void jit_machine<A>::compile(node_id fn, jit_fn & out) {
  out.deps[fn.value] = db.node_revision(fn);
  out.deps[mod.value] = db.node_revision(mod);
  for(node_id p: db.as<n_fn>(fn).params) {
    if(db.as<n_param>(p).type && db.is<n_pat>(db.as<n_param>(p).type)) {
      out.reason = "параметр-образец (распаковка) — интерпретатор";
      return;
    }
    if(db.as<n_param>(p).pack && db.as<n_param>(p).mode == param_mode::let) {
      out.reason = "хвост аргументов ...xs — интерпретатор";
      return;
    }
  }
  if(auto bad = jit_prefilter(db, mod, db.as<n_fn>(fn).body)) {
    auto lc = locate(db.source(db[bad->first].where.source), db[bad->first].where.begin);
    out.reason = cat(bad->second, " (строка ", lc.line, ")");
    return;
  }
  // Static parsing instance: Types and compile-time node (value-approach)
  for(jt t: out.params)
    if(t == jt::other || t == jt::unit) {
      out.reason = "тип параметра";
      return;
    }
  auto shapes_for = [&](interpreter & in) {
    std::vector<value> shapes;
    for(jt t: out.params) {
      if(is_stream(t)) { // flow-parameter: parsing flow with element type
        auto & ss = in.root->streams;
        ss.emplace_back();
        ss.back().typed = true;
        ss.back().shape = jt_shape(elem_of(t));
        shapes.push_back(in.stream_value(ss.size() - 1));
        continue;
      }
      shapes.push_back(jt_shape(t));
    }
    return shapes;
  };
  // threads created by instance: the type of elements is known only after writing (later
  // created) - the second pass starts with the types of the first, like the parsing program
  std::map<std::pair<uint64_t, uint64_t>, value> seeded;
  if(jit_creates_streams(db, db.as<n_fn>(fn).body)) {
    interpreter first(db, mod, {"jit"}, mode::check);
    first.set_fuel(200000);
    auto shapes = shapes_for(first);
    std::size_t from = first.root->streams.size();
    try {
      first.analyze_instance(fn, shapes);
    } catch(diagnostic const &) {
    }
    for(std::size_t k = from; k < first.root->streams.size(); ++k)
      if(auto const & s = first.root->streams[k]; s.typed && !interpreter::unknown(s.shape)) seeded.try_emplace({s.born_ctx, s.born_at}, s.shape);
  }
  interpreter an(db, mod, {"jit"}, mode::check);
  an.set_fuel(200000);
  an.stream_shapes = std::move(seeded);
  auto shapes = shapes_for(an);
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
    an_for_lang = &an;
    if(lang_compile(fn, out)) return;
    jit_codegen<A>(*this, fn, out, an).run();
  } catch(jit_unsupported const & u) {
    auto lc = locate(db.source(db[u.node].where.source), db[u.node].where.begin);
    out.reason = cat(u.why, " (строка ", lc.line, ")");
  } catch(diagnostic const & d) {
    out.reason = cat("разбор: ", d.what());
  }
}

} // namespace tz

#include "a64_check.hpp"

namespace tz {

// Machine code flow operation: op 0 - new(cap = bits), 1 - write(id, bits), 2 - read, 3 -
// more, 4 - close, 5 - yield; t_node - value type (jt) and node. The lock parks fiber right
// here on its stack; the error and cancellation of fiber is in jit_pending.
//
uint64_t jit_stream_call(uint64_t op, uint64_t id, uint64_t bits, uint64_t t_node) {
  interpreter & vm = *jit_vm; // before switching: after it jit_vm can be alien
  jt t = jt(t_node >> 32);
  node_id at{uint32_t(t_node)};
  try {
    span where; // place - only if the error (below): on each exchange it is not necessary
    if(op == 5) {
      vm.yield();
      return 0;
    }
    if(op == 0) {
      where = vm.db[at].where; // place of creation - element type key
      std::vector<value> a{value(int64_t(bits))};
      return vm.stream_id(vm.stream_op("stream_new", a, where), where);
    }
    static constexpr uint8_t ops[] = {0, interpreter::s_write, interpreter::s_read, interpreter::s_more, interpreter::s_close};
    value x = op != 1 ? value() : t == jt::f64_ ? value(std::bit_cast<double>(bits)) : t == jt::bool_ ? value(bits != 0) : value(int64_t(bits));
    value r = vm.stream_do(ops[op], std::size_t(id), x, where);
    if(op == 3) return r.is<bool>() && r.as<bool>();
    if(op == 2) {
      if(t == jt::i64_ && r.is<int64_t>()) return uint64_t(r.as<int64_t>());
      if(t == jt::f64_ && r.is<double>()) return std::bit_cast<uint64_t>(r.as<double>());
      if(t == jt::bool_ && r.is<bool>()) return r.as<bool>();
      jit_fault = uint64_t(jit_fault_kind::layout); // type guaranteed by parsing
      jit_fault_node = at.value;
    }
    return 0;
  } catch(diagnostic & d) {
    if(!d.where.end) d.where = vm.db[at].where;
    jit_pending = std::make_exception_ptr(d);
    jit_fault = uint64_t(jit_fault_kind::vm_error);
    jit_fault_node = at.value;
    return 0;
  } catch(...) {
    jit_pending = std::current_exception();
    jit_fault = uint64_t(jit_fault_kind::vm_error);
    jit_fault_node = at.value;
    return 0;
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
  if(params.size() != vals.size() || rest_param(f)) return std::nullopt; // tail ...xs - interpreter
  for(uint32_t i = 0; i < params.size(); ++i) {
    auto m = db.as<n_param>(params[i]).mode;
    if(m == param_mode::expr || m == param_mode::own || m == param_mode::out) return std::nullopt;
    // scalar that the function changes: there is no return entry from the interpreter
    if(!vals[i].is<array_v>() && writes_param(f, i)) return std::nullopt;
  }
  std::vector<jt> param_types;
  for(auto const & v: vals) {
    jt t = jt_of(v, this);
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
    else if(is_stream(param_types[i])) raw[i] = stream_id(v, {});
    else { // array: pointer to array_v caller (alias) or to a copy
      value * cell = i < args.size() ? place_or_null(args[i], env) : nullptr;
      raw[i] = reinterpret_cast<uint64_t>(cell && cell->is<array_v>() ? &cell->as<array_v>() : &vals[i].as<array_v>());
    }
  }
  // AArch64 verification: same call to qemu before execution to host (inputs - as is)
  std::optional<a64_check::outcome> shadow;
  std::vector<a64_check::arg> sargs;
  if(a64 && a64->calls[key]++ < 3) {
    bool ok = true;
    for(uint32_t i = 0; i < vals.size() && ok; ++i) {
      a64_check::arg a;
      if(param_types[i] == jt::arr_i64 || param_types[i] == jt::arr_f64) {
        auto const * arr = reinterpret_cast<array_v const *>(raw[i]);
        a.array = true;
        for(uint32_t j = 0; j < i && a.same_as < 0; ++j)
          if(raw[j] == raw[i] && (param_types[j] == jt::arr_i64 || param_types[j] == jt::arr_f64)) a.same_as = int(j);
        for(uint32_t k = 0; k < arr->size() && ok; ++k) {
          value e = arr->get(k);
          if(e.is<int64_t>()) a.items.push_back(uint64_t(e.as<int64_t>()));
          else if(e.is<double>()) a.items.push_back(std::bit_cast<uint64_t>(e.as<double>()));
          else ok = false;
        }
      } else a.bits = raw[i];
      sargs.push_back(std::move(a));
    }
    if(ok) shadow = a64->run(f, param_types, sargs);
  }
  jit_fault = 0;
  jit_vm = this;
  auto entry = reinterpret_cast<uint64_t (*)(uint64_t const *)>(code->entry);
  uint64_t bits;
  if(code->by_lang) { // Compiler code in language: array - {данные, размер}, behind arguments - error slots
    std::vector<uint64_t> area;
    for(uint32_t i = 0; i < vals.size(); ++i) {
      if(param_types[i] == jt::arr_i64 || param_types[i] == jt::arr_f64) {
        auto * arr = reinterpret_cast<array_v *>(raw[i]);
        void * data = jit_array_data(arr, param_types[i] == jt::arr_i64 ? 1 : 2);
        if(!data) throw diagnostic("внутренняя ошибка JIT: раскладка массива не совпала с типом экземпляра", db[f].where);
        area.push_back(reinterpret_cast<uint64_t>(data));
        area.push_back(arr->size());
      } else area.push_back(raw[i]);
    }
    std::size_t fault_at = area.size();
    area.push_back(0);
    area.push_back(0);
    bits = entry(area.data());
    jit_fault = area[fault_at]; // Error - from the slot that gave VM (not the host address in the code)
    jit_fault_node = area[fault_at + 1];
  } else bits = entry(raw.data());
  auto fault = jit_fault_kind(jit_fault);
  if(shadow) a64->compare(*shadow, sargs, raw, code->result, bits, cat(db.strings.text(db.as<n_fn>(f).name), "(", jit_engine::types_str(param_types), ")"));
  if(fault == jit_fault_kind::vm_error) { // Error/Cancellation within the VM operation
    jit_fault = 0;
    std::rethrow_exception(std::exchange(jit_pending, nullptr));
  }
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
