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

// The value of one interpreter is in the types of another: the type numbers of the registers
// have their own (the order in which parsing met the names), the common ones are the names.
value port_value(value const & x, type_registry const & from, type_registry & to) {
  if(x.is<rt_v>()) {
    value const * sh = shape_of(x);
    return sh ? erase(port_value(*sh, from, to)) : x;
  }
  if(x.is<type_v>()) return value(type_v{to.named(from.name(x.as<type_v>().type))});
  if(x.is<agg_v>()) {
    agg_v a{to.named(from.name(x.as<agg_v>().type)), {}};
    for(auto const & [k, v]: x.as<agg_v>().fields) a.fields.push_back({k, port_value(v, from, to)});
    return value(std::move(a));
  }
  if(x.is<array_v>()) {
    std::vector<value> xs;
    for(std::size_t k = 0; k < x.as<array_v>().size(); ++k) xs.push_back(port_value(x.as<array_v>().get(k), from, to));
    return value(array_v::of(std::move(xs)));
  }
  if(x.is<split_v>()) {
    split_v s;
    for(auto const & a: x.as<split_v>().alts) s.alts.push_back(port_value(a, from, to));
    return value(std::move(s));
  }
  return x;
}

// An interpreter executing machine code (VM operations from code through it).
interpreter * jit_vm = nullptr;
// Memory of the context of calling the machine code values that VM has created for the code;
// the code holds their addresses. To call (address in the field of arguments): fiber is
// switched inside the machine code.
//
struct jit_call_mem {
  std::deque<value> own;
  std::vector<value> print_args; // Print: Arguments of the line to the last
  value * add(value v) {
    own.push_back(std::move(v));
    return &own.back();
  }
};
// Excluding the language from node that VM computed: machine code is labeled vm_error
std::optional<interpreter::flow> jit_pending_flow;
uint64_t jit_stream_call(uint64_t op, uint64_t id, uint64_t bits, uint64_t t_node);

// VM service for compiler code in language: address - in argument area (not in code), answer -
// {значение, признак ошибки} in two registers C ABI; error - delayed exception VM
// (jit_pending).
struct jit_service_result {
  uint64_t value, error;
};
jit_service_result jit_vm_service(uint64_t op, uint64_t id, uint64_t bits, uint64_t t_node) {
  uint64_t saved = std::exchange(jit_fault, 0);
  uint64_t v = jit_stream_call(op, id, bits, t_node);
  uint64_t e = jit_fault;
  jit_fault = saved;
  return {v, e};
}

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
  bool by_lang = false; // all code - compiler in language (jit_compile)
  std::string lang_note; // statistician
  std::string lang_error; // the compiler in the language could not - the reason (performs the interpreter)
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
      if(f->ok && f->by_lang) out += cat("  jit  ", name, "(", types_str(f->params), ") → ", f->bytes, " байт машинного кода, компилятор языка", f->lang_note.empty() ? "" : cat(", ", f->lang_note), "\n");
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
  // → bytes. Empty - does not know: the interpreter will perform.
  interpreter * an_for_lang = nullptr; // parsing instance: chain trees for compiler language
  bool lang_compile(node_id fn, jit_fn & out) {
    node_id lc;
    for(uint32_t i = db.size(); i-- > 0;)
      if(db.is<n_fn>(node_id(i)) && db.strings.text(db.as<n_fn>(node_id(i)).name) == "jit_compile") {
        lc = node_id(i);
        break;
      }
    if(!lc) return false;
    std::vector<value> types; // Types: Type-values, not strings
    auto type_value = [&](interpreter & in, jt t) { return value(type_v{in.types.named(jt_name(t))}); };
    value r;
    try {
      interpreter comp(db, mod, {"jit"}, mode::run);
      comp.jit_analysis = an_for_lang;
      for(jt t: out.params) types.push_back(type_value(comp, t));
      // entity context (amd64, arm64), not a string
      r = comp.analyze_instance(lc, {value(fn_v{comp.sym(A::target), {}, nullptr}), comp.code_value(fn), value(array_v::of(std::move(types))), type_value(comp, out.result)});
    } catch(diagnostic const & d) {
      out.lang_error = d.what();
      if(std::getenv("TZ_LANG_DEBUG")) put(stderr, render(d, "?", db)); // compiler debugging
      return false;
    }
    if(r.is<str_v>()) { // Failure of the compiler in the language
      out.lang_error = r.as<str_v>();
      if(std::getenv("TZ_LANG_DEBUG")) put(stderr, cat("компилятор языка: ", db.strings.text(db.as<n_fn>(fn).name), ": ", out.lang_error, "\n"));
    }
    if(!r.is<array_v>() || r.as<array_v>().size() == 0) return false;
    if(r.as<array_v>().get(0).is<array_v>()) { // {bytes, note for statistics}
      value note = r.as<array_v>().size() > 1 ? r.as<array_v>().get(1) : value();
      if(note.is<str_v>()) out.lang_note = note.as<str_v>();
      r = r.as<array_v>().get(0);
    }
    A code;
    for(uint32_t k = 0; k < r.as<array_v>().size(); ++k) code.byte(uint8_t(r.as<array_v>().get(k).as<int64_t>()));
    out.deps[lc.value] = db.node_revision(lc);
    out.bytes = code.size();
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

// Cheap syntactic filter to static parsing: what code generator obviously can’t do is not worth
// disassembling (parsing can calculate a lot with compile-time).
std::optional<std::pair<node_id, std::string>> jit_prefilter(ast_db const & db, node_id module, node_id n) {
  if(!n) return std::nullopt;
  // strings, aggregates and calls of kernel functions - VM values (service: node calculates
  // VM, author's decision 2026-10-01); obviously not - unpacking, ct
  if(db.try_compile_body(n) || db.is<n_hole>(n) || db.is<n_unpack>(n) || db.is<n_pat>(n) || db.is<n_spread>(n))
    return std::pair{n, cat("узел ", kind_name(db[n]))};
  if(db.is<n_ct>(n)) return std::pair{n, std::string("ct — по месту вызова (канонический порядок)")};
  // ct-variable module is read in place in canonical order (§7.3) - not the way of execution
  if(db.is<n_ident>(n) && module && db.is<n_module>(module))
    for(node_id item: db.as<n_module>(module).items)
      if(db.is<n_var>(item) && db.as<n_var>(item).ct && db.as<n_var>(item).name == db.as<n_ident>(n).name)
        return std::pair{n, std::string("ct-переменная модуля — по месту (канонический порядок)")};
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
  interpreter an(db, mod, {"jit"}, mode::check);
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
      if(auto const & s = first.root->streams[k]; s.typed && !interpreter::unknown(s.shape)) seeded.try_emplace({s.born_ctx, s.born_at}, port_value(s.shape, first.types, an.types));
  }
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
    // code generation - complete compiler in language (jit_compile prelude); failed -
    // interpreter with cause
    if(lang_compile(fn, out)) return;
    out.reason = out.lang_error.empty() ? "компилятор языка: нет jit_compile" : cat("компилятор языка: ", out.lang_error);
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
uint64_t jit_value_bits(value const & v, uint64_t kind) {
  if(kind == 2 && v.is<double>()) return std::bit_cast<uint64_t>(v.as<double>());
  if(kind == 3 && v.is<bool>()) return v.as<bool>();
  if(kind == 1 && v.is<int64_t>()) return uint64_t(v.as<int64_t>());
  jit_fault = uint64_t(jit_fault_kind::layout);
  return 0;
}
value jit_bits_value(uint64_t kind, uint64_t b) {
  return kind == 2 ? value(std::bit_cast<double>(b)) : kind == 3 ? value(b != 0) : value(int64_t(b));
}
uint64_t jit_eval_node(interpreter & vm, node_id at, uint64_t * buf, uint64_t want) {
  auto & mem = *reinterpret_cast<jit_call_mem *>(buf[0]);
  uint64_t n = buf[1];
  scope * s = vm.new_scope(vm.globals);
  for(uint64_t i = 0; i < n; ++i) {
    uint64_t * e = buf + 2 + 3 * i;
    symbol name = vm.db.as<n_ident>(node_id{uint32_t(e[0])}).name;
    if(e[1] == 9) s->alias(name, reinterpret_cast<value *>(e[2])); // The value of VM is at the cell address
    else s->declare(name, jit_bits_value(e[1], e[2]));
  }
  interpreter::flow r = vm.eval(at, s, 0);
  for(uint64_t i = 0; i < n; ++i) { // modified scalars -- back
    uint64_t * e = buf + 2 + 3 * i;
    if(e[1] != 9) e[2] = jit_value_bits(*s->find(vm.db.as<n_ident>(node_id{uint32_t(e[0])}).name), e[1]);
  }
  vm.drop_scope(s);
  if(r.kind == interpreter::flow_kind::thr) {
    jit_pending_flow = std::move(r);
    jit_fault = uint64_t(jit_fault_kind::vm_error);
    jit_fault_node = at.value;
    return 0;
  }
  if(!r.normal()) throw diagnostic("внутренняя ошибка JIT: return/break в узле, который вычисляет VM", vm.db[at].where);
  if(want == 0) return 0;
  if(want == 9) return reinterpret_cast<uint64_t>(mem.add(std::move(r.v)));
  uint64_t b = jit_value_bits(r.v, want);
  if(jit_fault) jit_fault_node = at.value;
  return b;
}

uint64_t jit_stream_call(uint64_t op, uint64_t id, uint64_t bits, uint64_t t_node) {
  interpreter & vm = *jit_vm; // before switching: after it jit_vm can be alien
  jt t = jt(uint8_t(t_node >> 32));
  bool last = (t_node >> 40) & 1; // print: final argument
  node_id at{uint32_t(t_node)};
  try {
    span where; // place - only if the error (below): on each exchange it is not necessary
    if(op == 5) {
      vm.yield();
      return 0;
    }
    // print (author's decision 2026-10-01: memory and output - in VM, through the service): 6 - value (bits, type), 7 - string
    // literal node; id - table of call values, bit 40 node - the last argument 8 - node at calculates the interpreter (what the
    // compiler in the language does not know): id - buffer {память вызова, n, (узел имени, вид, биты) × n}, type 1-3 - scalar, 9 -
    // the address of the cell value of VM; modified s - back to the buffer; bits - the type of result
    //
    if(op == 8) return jit_eval_node(vm, at, reinterpret_cast<uint64_t *>(id), bits);
    if(op == 6 || op == 7) {
      auto & pa = reinterpret_cast<jit_call_mem *>(id)->print_args;
      if(op == 7) pa.push_back(value(vm.db.as<n_str>(at).value));
      else if(t == jt::f64_) pa.push_back(value(std::bit_cast<double>(bits)));
      else if(t == jt::bool_) pa.push_back(value(bits != 0));
      else if(t == jt::i64_) pa.push_back(value(int64_t(bits)));
      if(last) {
        std::vector<value> a = std::move(pa);
        pa.clear();
        vm.builtin(vm.sym("print"), a, vm.db[at].where);
      }
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
  // The main program is executed once - no way for JIT (author's decision
  // 2026-10-01):
// 
  // 
  if(db[f].parent == mod && db.strings.text(db.as<n_fn>(f).name) == "main") return std::nullopt;
  if(db.is<n_block>(db[f].parent)) return std::nullopt; // Local function - closure: built into the enclosing code
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
  std::vector<value *> cells(vals.size()); // Argument array cells: the compiler code in the language holds their addresses
  for(uint32_t i = 0; i < vals.size(); ++i) {
    value const & v = vals[i];
    if(v.is<int64_t>()) raw[i] = uint64_t(v.as<int64_t>());
    else if(v.is<bool>()) raw[i] = v.as<bool>();
    else if(v.is<double>()) std::memcpy(&raw[i], &v.as<double>(), 8);
    else if(is_stream(param_types[i])) raw[i] = stream_id(v, {});
    else { // array: pointer to array_v caller (alias) or to a copy
      value * cell = i < args.size() ? place_or_null(args[i], env) : nullptr;
      cells[i] = cell && cell->is<array_v>() ? cell : &vals[i];
      raw[i] = reinterpret_cast<uint64_t>(&cells[i]->as<array_v>());
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
  jit_call_mem mem;
  if(code->by_lang) { // Compiler code in language: array - cell address (data and size code reads from it), behind the arguments - error slots
    std::vector<uint64_t> area;
    for(uint32_t i = 0; i < vals.size(); ++i) {
      if(param_types[i] == jt::arr_i64 || param_types[i] == jt::arr_f64) {
        value * c = cells[i];
        if(array_v const * b = c->as<array_v>().borrowed) c = cell_of(b); // loan
        array_v const & arr = c->as<array_v>();
        auto want = param_types[i] == jt::arr_i64 ? array_v::layout::ints : array_v::layout::floats;
        if(!arr.empty() && arr.kind() != want) throw diagnostic("внутренняя ошибка JIT: раскладка массива не совпала с типом экземпляра", db[f].where);
        area.push_back(reinterpret_cast<uint64_t>(c));
      } else area.push_back(raw[i]);
    }
    std::size_t fault_at = area.size();
    area.insert(area.end(), 4, 0); // type of error, node, thrown value, its type
    area.push_back(reinterpret_cast<uint64_t>(&jit_vm_service)); // VM service (streams, print, node)
    area.push_back(reinterpret_cast<uint64_t>(&mem)); // memory
    bits = entry(area.data());
    jit_fault = area[fault_at]; // Error - from slots given by VM (not the host address in the code)
    jit_fault_node = area[fault_at + 1];
    jit_thrown = area[fault_at + 2];
    jit_thrown_type = area[fault_at + 3];
  } else bits = entry(raw.data());
  auto fault = jit_fault_kind(jit_fault);
  if(shadow) a64->compare(*shadow, sargs, raw, code->result, bits, cat(db.strings.text(db.as<n_fn>(f).name), "(", jit_engine::types_str(param_types), ")"));
  if(fault == jit_fault_kind::vm_error && jit_pending_flow) { // Excluding the language from node computed by VM
    jit_fault = 0;
    flow r = std::move(*jit_pending_flow);
    jit_pending_flow.reset();
    return r;
  }
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
    // The compiler code on the language puts the index and size in the words of the value - the message as the interpreter
    if(fault == jit_fault_kind::bounds && code->by_lang)
      fail_rt("out_of_bounds", cat("индекс ", int64_t(jit_thrown), " вне границ массива размера ", jit_thrown_type), db[at].where);
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
