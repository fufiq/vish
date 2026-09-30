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

#include "fiber.hpp"
#include "os.hpp"
#include "value.hpp"

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <set>

namespace tz {

// Lexical scope. Variables are cells; alias indicates the caller cell rather than copying
// it. Areas live in the interpreter arena (no pile on each and no reference counters):
// block/call/iteration take the area and return it at the output; the area captured by the
// lambda lives until the end of the run. Cell Book (Verification Mode): When the cell was
// born and when its value was last replaced entirely. On this stand the facts about values
// (the index is valid until the array is replaced) and parsing cycle (which cells born
// before iteration, it changes - including through blocks of structures). journal entries
// of the parsing section (cycle iteration, if branch): cells born before and changed in
// it; for the branch, and their former values.
//
//
struct write_log {
  uint64_t start = 0;
  std::set<value *> cells;
  std::map<value *, value> old;
  bool keep_old = false;
};

struct cell_book {
  uint64_t clock = 0;
  std::unordered_map<value const *, std::pair<uint64_t, uint64_t>> cells; // (birth, version)
  std::vector<write_log *> logs;

  void born(value const * c) {
    ++clock;
    cells[c] = {clock, clock};
  }
  uint64_t version(value const * c) const {
    auto it = cells.find(c);
    return it == cells.end() ? 0 : it->second.second;
  }
  std::unordered_map<value const *, uint32_t> changed_at; // last disability support (node +1)

  void invalidated(value * c, node_id at) {
    written(c, true);
    changed_at[c] = at.value + 1;
  }
  void written(value * c, bool whole) {
    auto & e = cells[c];
    if(whole) e.second = ++clock;
    for(auto * l: logs)
      if(e.first <= l->start && l->cells.insert(c).second && l->keep_old) l->old.emplace(c, *c);
  }
};

struct scope {
  scope * parent = nullptr;
  std::vector<std::pair<symbol, value *>> vars;
  std::deque<value> storage;
  bool captured = false;
  cell_book * book = nullptr;

  value * declare(symbol name, value v) {
    storage.push_back(std::move(v));
    vars.push_back({name, &storage.back()});
    if(book) book->born(&storage.back());
    return &storage.back();
  }

  void alias(symbol name, value * target) { vars.push_back({name, target}); }

  value * find(symbol name) {
    for(auto it = vars.rbegin(); it != vars.rend(); ++it)
      if(it->first == name) return it->second;
    return parent ? parent->find(name) : nullptr;
  }
};

// clang-format off
enum class mode : uint8_t { run, check };
// clang-format on

// --- reactive cache of static parsing (§10.1) ---
//
// Granularity is functions, not files. instance functions (key: function + compile-time /
// run-time -arguments + continuation) are remembered along with the result, dependencies
// (whose bodies were executed, with node revisions) and report events. After editing,
// only instance with changed dependencies are recalculated, the rest are reproduced. Cash
// lives in a long-lived VM session (tz vm).
//
struct report_event {
  enum kind_t : uint8_t { instance,
    folded,
    stage,
    split,
    excluded,
    live,
    proven,
    unproven,
    rt_check,
    throws,
    resolved } kind; // resolved: the fn call selected overload b (node function number)
  std::string a, b, c; // meaning depends on the species
  span where;
  std::vector<std::string> alts;
  uint32_t fn = 0;
};

struct check_cache {
  struct entry {
    value result;
    std::map<uint32_t, uint64_t> deps; // Node of function → revision at parsing
    std::vector<report_event> events;
    std::vector<std::tuple<uint32_t, value, bool, bool, bool, bool>> journal; // Observations (the latter in a folded call)
    bool effect = false;
  };
  std::map<std::string, entry> entries;
  uint32_t hits = 0, recomputed = 0;
  std::vector<std::string> recomputed_names;
};

struct interpreter {
  // residual program inputs: known arguments and strings of stdin (§13.2).
  struct known_inputs {
    std::vector<std::string> stdin_lines;
  };

  // Parent is a process (or sample) within the same VM: a common type registry and scheduler.
  interpreter(ast_db & db, node_id module, std::vector<std::string> args, mode m = mode::run,
      std::optional<known_inputs> known = std::nullopt, check_cache * cache = nullptr, interpreter * parent = nullptr)
      : db(db), mod(module), run_mode(m), cache(cache), inputs(std::move(known)), types(parent ? parent->types : own_types),
        root(parent ? parent->root : this) {
    sym_intercept = sym("intercept");
    sym_append = sym("append");
    sym_spawn = sym("fiber_start");
    sym_process = sym("process");
    sym_yield = sym("yield");
    sym_is_index = sym("is_index");
    globals = new_scope(nullptr);
    array_v a;
    for(auto & s: args) a.push(value(s));
    value args_v(std::move(a));
    if(checking() && !inputs) args_v = erase(value(array_v::of({value(std::string("x"))}))); // arguments - runtime
    globals->declare(sym("args"), std::move(args_v));
    globals->declare(sym("stdin"), value(agg_v{types.named("stream"), {}})); // descriptor, no value
    for(node_id m: {db.prelude, module}) { // prelude - first
      if(!m || (m == db.prelude && m == module)) continue;
      for(node_id item: db.as<n_module>(m).items)
        if(!db.is<n_extend>(item)) register_item(item);
    }
    for(node_id item: db.as<n_module>(module).items) // Expansion - after announcements
      if(db.is<n_extend>(item)) register_item(item);
  }

  // entity operator node: infix+/prefix-/postfix++.
  symbol operator_entity(n_op const & o) {
    uint64_t k = (uint64_t(o.op.value) << 2) | (o.args.size() == 2 ? 1 : o.postfix ? 2
                                                                                   : 0);
    auto [it, fresh] = op_entities.try_emplace(k, symbol{});
    if(fresh) it->second = sym(cat(o.args.size() == 2 ? "infix" : o.postfix ? "postfix"
                                                                            : "prefix",
                  str(o.op)));
    return it->second;
  }

  // The prelude operator, whose body is built-in primitive over parameters in order
  // (operator (a) + (b) => add(a, b)): the VM operation is executed directly.
  bool intrinsic_operator(node_id f) const {
    auto const & fn = db.as<n_fn>(f);
    if(!fn.expr_body || !db.is<n_call>(fn.body) || !db.is<n_ident>(db.as<n_call>(fn.body).callee)) return false;
    static constexpr std::array prims{"add", "sub", "mul", "div", "rem", "eq", "ne", "lt", "gt", "le", "ge", "and", "or", "neg", "not"};
    auto callee = str(db.as<n_ident>(db.as<n_call>(fn.body).callee).name);
    if(std::ranges::find(prims, callee) == prims.end()) return false;
    auto const & args = db.as<n_call>(fn.body).args;
    if(args.size() != fn.params.size()) return false;
    for(std::size_t k = 0; k < args.size(); ++k)
      if(!db.is<n_ident>(args[k]) || db.as<n_ident>(args[k]).name != db.as<n_param>(fn.params[k]).name) return false;
    return true;
  }

  static bool operator_name(std::string_view n) {
    for(std::string_view role: {"infix", "prefix", "postfix"})
      if(n.size() > role.size() && n.starts_with(role) && std::string_view("+-*/%<>=!&|^~?@$\\").contains(n[role.size()])) return true;
    return false;
  }

  // Announcement in current elaboration: from a module or generated emit.
  void register_item(node_id item) {
    if(db.is<n_fn>(item) && operator_name(str(db.as<n_fn>(item).name)) && !intrinsic_operator(item))
      user_ops.insert(db.as<n_fn>(item).name.value);
    if(db.is<n_fn>(item)) functions[db.as<n_fn>(item).name].push_back(item);
    else if(db.is<n_struct>(item)) structs[db.as<n_struct>(item).name] = item;
    else if(db.is<n_extend>(item)) { // fields and attributes to entity, functions to overload
      auto const & e = db.as<n_extend>(item);
      if(e.target) { // overload of a specific function: under its current name
        symbol name = db.as<n_fn>(node_id(e.target)).name;
        for(node_id i: e.items) functions[name].push_back(i);
        return;
      }
      for(node_id a: e.attrs) ext_attrs[e.name].push_back(a);
      for(node_id i: e.items) {
        if(db.is<n_fn>(i)) functions[db.as<n_fn>(i).name].push_back(i);
        else ext_fields[e.name].push_back(i);
      }
    }
  }

  // Elaboration: Modular ct { } is executed once, in module order, before main and
  // before parsing (order instantiation, D14). They can generate ads (emit) - those
  // are registered in this elaboration.
  void elaborate() {
    if(elaborated) return;
    elaborated = true;
    for(node_id item: db.as<n_module>(mod).items) {
      if(db.is<n_ct>(item)) {
        ++ct_depth;
        flow f = eval(db.as<n_ct>(item).body, globals, 0);
        --ct_depth;
        if(f.kind == flow_kind::thr) uncaught(f);
      }
      if(db.is<n_var>(item) && db.as<n_var>(item).ct) { // ct x = e - in the order of the module
        auto const & v = db.as<n_var>(item);
        ++ct_depth;
        flow f = eval(v.init, globals, 0);
        --ct_depth;
        if(f.kind == flow_kind::thr) uncaught(f);
        if(!f.v.known()) fail(cat("ct ", str(v.name), ": значение должно быть известно при компиляции"), db[item].where);
        ct_cells.insert(globals->declare(v.name, f.v));
        ct_names.push_back(v.name);
      }
    }
  }

  // Unit fields: declaration + extension (in module order).
  std::vector<node_id> fields_of_struct(symbol name) const {
    std::vector<node_id> out;
    if(auto it = structs.find(name); it != structs.end())
      for(node_id f: db.as<n_struct>(it->second).fields) out.push_back(f);
    if(auto it = ext_fields.find(name); it != ext_fields.end())
      for(node_id f: it->second) out.push_back(f);
    return out;
  }

  // Launch main(). Returns the exit code.
  int run_main() {
    auto it = functions.find(sym("main"));
    if(it == functions.end()) throw diagnostic("нет функции main", {});
    elaborate();
    // ct-state - in canonical order, not along the execution branches; for JIT - also the types of
    // thread elements at the place of creation (types - from parsing, not from statistics)
    if(!checking() && (!ct_names.empty() || (jit && calls(mod, sym("stream"))))) canonical_ct();
    it = functions.find(sym("main"));
    call_site site{db[it->second.front()].where, "main"};
    flow r;
    try {
      r = call_overloaded(it->second, {}, globals, 0, site);
      if(r.kind == flow_kind::thr) uncaught(r);
      if(!checking()) join_all(); // VM awaits fiber program
    } catch(...) {
      cancel_fibers();
      throw;
    }
    if(r.kind == flow_kind::brk || r.kind == flow_kind::cont) fail("break/continue вне цикла", site.where);
    return r.v.is<int64_t>() ? int(r.v.as<int64_t>()) : 0;
  }

  // Observations of node mode (for residual program).
  struct observation {
    value v;
    bool seen = false; // knot was executed and gave meaning
    bool diverted = false; // At least go back/break.
    bool effect = false; // The result was a runtime effect.
    bool present = false; // knot
  };

  // Observing node in One Embedding Context
//.
  observation const * observed(node_id n, uint64_t at) const {
    build_observed();
    auto it = observations_ctx.find({n.value, at});
    return it == observations_ctx.end() ? nullptr : &it->second;
  }

  // The body context of a function called from a node call in a ctx context.
  static uint64_t inline_context(uint64_t ctx, uint32_t call) { return (ctx ^ (uint64_t(call) + 0x9e3779b97f4a7c15ull)) * 0x100000001b3ull; }

  observation const * observed(node_id n) const {
    build_observed();
    auto it = observations.find(n.value);
    return it == observations.end() ? nullptr : &it->second;
  }

  bool live(node_id fn) const { return live_fns.contains(fn.value); }

  // Launch track (debugger, author's decision): execution path (§13.2) - node, value,
  // depth of calls, how much is brought to this step. Just what VM already knows:
  //
// - free of symbol tables
  // and "debugging build"; built-in calls are the same steps.
  struct trace_rec {
    uint32_t node;
    uint32_t depth;
    uint32_t out;
    std::string value;
  };
  std::vector<trace_rec> * tracer = nullptr;
  static constexpr std::size_t trace_limit = 200000;

  // Layers of function - contexts of its use
// 
// 
// Layer is key
  // instance: what was executed in the body, the types and known values of node when
  // compiled; compile-time - the call is folded entirely, run-time - is needed in runtime.
  struct layer_info {
    bool ct = true;
    std::map<uint32_t, std::vector<std::string>> types;
    std::map<uint32_t, std::string> literal;
  };
  std::map<uint32_t, std::map<std::string, layer_info>> layers; // function → key instance → layer

  node_id enclosing_fn(node_id n) const {
    for(; n; n = db[n].parent)
      if(db.is<n_fn>(n)) return n;
    return {};
  }

  void note_layer(node_id f, std::string const & key, std::size_t from, bool folded) {
    layer_info & l = layers[f.value][key];
    l.ct = l.ct && folded;
    for(std::size_t i = from; i < journal.size(); ++i) {
      auto const & e = journal[i];
      if(enclosing_fn(node_id(e.node)) != f) continue; // node of nested calls - their layers
      auto & ts = l.types[e.node];
      if(!e.seen) continue;
      if(std::string t = types.describe(e.v); !std::ranges::contains(ts, t)) ts.push_back(t);
      if(auto lit = literal(e.v)) l.literal[e.node] = *lit;
    }
  }

  // Types of node in all contexts where it was executed (instance, embedding
  // locations):
// 
// For editor: Types, members after `.`.
  // Calls folded into compile-time are also contexts of use: their observations are included.
  std::map<uint32_t, std::vector<std::string>> node_types() const {
    std::map<uint32_t, std::vector<std::string>> out;
    for(auto const & e: journal) {
      if(!e.seen) continue;
      auto & ts = out[e.node];
      auto add = [&](value const & v) {
        std::string t = types.describe(v);
        if(!std::ranges::contains(ts, t)) ts.push_back(t);
      };
      if(e.v.is<split_v>()) // splitting - each alternative separately
        for(auto const & a: e.v.as<split_v>().alts) add(a);
      else add(e.v);
    }
    return out;
  }

  // Show the meaning as literal, if possible.
  std::optional<std::string> literal(value const & v) const {
    if(!v.known()) return std::nullopt;
    if(v.is<int64_t>() || v.is<double>() || v.is<bool>()) return show_v(v);
    if(v.is<std::string>()) return cat(in_quotes(v.as<std::string>()));
    if(v.is<array_v>()) {
      std::string s = "array{";
      for(uint32_t i = 0; i < v.as<array_v>().size(); ++i) {
        auto item = literal(v.as<array_v>().get(i));
        if(!item) return std::nullopt;
        s += (i ? ", " : "") + *item;
      }
      return s + "}";
    }
    return std::nullopt;
  }

  // Static parsing of one instance (for JIT): parameters are erased values of given
  // types; the result is the type (or compile-time) of the result.
  value analyze_instance(node_id f, std::vector<value> args) {
    elaborate();
    auto const & fn = db.as<n_fn>(f);
    scope * inner = new_scope(globals);
    scope_hold hold{*this, inner};
    for(uint32_t i = 0; i < fn.params.size() && i < args.size(); ++i) inner->declare(db.as<n_param>(fn.params[i]).name, args[i]);
    flow r = run_frame(fn.body, inner, fn.name, fn.outline);
    return r.v;
  }

  // Static parsing step budget (for JIT: compiling calculations should not be
  // unlimited).
  void set_fuel(uint64_t steps) { fuel = steps; }

  // Connect JIT (run mode): instance is executed by machine code, if JIT supports
  // them, otherwise by the interpreter.
  void use_jit(struct jit_engine * engine) { jit = engine; }

  // All print output is in line (for tests), otherwise in stdout.
  void capture_output(std::string * to) { captured = to; }

  // instance: function + (runtime-parts) of arguments → how many times.
  std::string instances() const {
    std::string out;
    for(auto const & [key, count]: instance_counts) out += cat("  ", key, "  ×", count, "\n");
    return out;
  }

  // Canonical order ct (§7.3 p. 4): the result does not depend on which branches are
  // executed - the ct-code of the program executes parsing of the entire program, and
  // the execution takes its results at the call places and the final ct-variables. Is
  // there a name(…) call in the subtree?
  bool calls(node_id n, symbol name) const {
    if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee) && db.as<n_ident>(db.as<n_call>(n).callee).name == name) return true;
    bool found = false;
    db.for_each_child(n, [&](node_id c) { found = found || calls(c, name); });
    return found;
  }

  void canonical_ct() {
    // parsing is in the order of the text: the spawn body reads the stream before the
    // sequel - read "" run-time. The next pass starts with the types of streams of the
    // previous one while they are refined (usually two passes).
    interpreter * last = nullptr;
    for(int pass = 0; pass < 4; ++pass) {
      interpreter & a = level_runs.emplace_back(db, mod, std::vector<std::string>{"ct"}, mode::check, std::nullopt, nullptr, this);
      a.fuel = 2000000;
      a.stream_shapes = stream_shapes;
      std::size_t first = root->streams.size();
      try {
        a.run_main();
      } catch(diagnostic const &) {
        if(!ct_names.empty()) throw; // ct is part of the program; thread types are just a JIT hint
        stream_shapes.clear();
        return;
      }
      last = &a;
      // parsing threads (root common): type of elements at the place of creation; different - not type
      std::map<std::pair<uint64_t, uint64_t>, value> found;
      std::set<std::pair<uint64_t, uint64_t>> mixed;
      for(std::size_t k = first; k < root->streams.size(); ++k) {
        auto const & s = root->streams[k];
        if(!s.typed || unknown(s.shape) || s.net || s.file_fd >= 0) continue;
        std::pair key{s.born_ctx, s.born_at};
        auto [it, fresh] = found.try_emplace(key, s.shape);
        if(!fresh && types.describe(it->second) != types.describe(s.shape)) mixed.insert(key);
      }
      for(auto const & key: mixed) found.erase(key);
      auto shown = [&](auto const & m) {
        std::string out;
        for(auto const & [k, v]: m) out += cat(k.first, ":", k.second, "=", types.describe(v), ";");
        return out;
      };
      bool same = shown(found) == shown(stream_shapes);
      stream_shapes = std::move(found);
      if(same || !jit) break;
    }
    interpreter & a = *last;
    for(auto const & [k, v]: a.ct_memo) ct_memo.try_emplace(k, v);
    for(symbol n: a.ct_names)
      if(value * mine = globals->find(n); mine && ct_cells.contains(mine))
        if(value * theirs = a.globals->find(n)) *mine = *theirs;
  }

  // Verification mode report.
  std::string report(std::string_view file, std::string_view src) const {
    std::string out;
    (void)src;
    auto loc = [&](span w) {
      auto lc = locate(db.source(w.source), w.begin);
      auto const & label = db.source_label(w.source);
      return cat(label.empty() ? file : std::string_view(label), ":", lc.line);
    };
    // The places inside prelude (body for/while/try) are not program code: the report
    // shows its instance and exceptions, but not its stages and indexes.
    auto mine = [&](span w) { return !db.prelude || w.source != db[db.prelude].where.source; };
    out += "стадии:\n";
    for(auto const & s: stages)
      if(mine(s.where)) out += cat("  ", loc(s.where), " ", s.path, s.name, " = ", s.what, "\n");
    if(!splits.empty()) {
      out += "расщепления:\n";
      for(auto const & s: splits)
        if(mine(s.where)) out += cat("  ", loc(s.where), " ", s.name, " → ", s.alts.size(), " продолжения: ", join(s.alts), "\n");
    }
    if(!excluded.empty()) {
      out += "исключённые блоки try_compile:\n";
      for(auto const & e: excluded) out += cat("  ", loc(e.where), " ", e.path, e.why, "\n");
    }
    if(!folded.empty()) {
      out += "свёрнуто в кт:\n";
      for(auto const & [name, count]: folded) out += cat("  ", name, "  ×", count, "\n");
    }
    if(!param_writes.empty()) {
      out += "функции меняют параметры (место вызывающего):\n";
      for(auto const & w: param_writes) out += cat("  ", w, "\n");
    }
    if(!throws_of.empty()) {
      out += "исключения (выведены «покраской»):\n";
      for(auto const & [inst, ts]: throws_of) {
        std::string list;
        for(auto const & t: ts) list += (list.empty() ? "" : ", ") + t;
        out += cat("  ", inst, " бросает: ", list, "\n");
      }
    }
    if(!ct_order.empty()) {
      out += "порядок инстанцирования (ct):\n";
      for(auto const & [w, inst, v]: ct_order)
        if(mine(w)) out += cat("  ", loc(w), " в ", inst, " → ", v, "\n");
    }
    std::string proofs;
    for(auto const & [w, what, node]: proven_list)
      if(mine(w) && !unproven.contains(node)) proofs += cat("  ", loc(w), " ", what, "\n");
    if(!proofs.empty()) out += "индексы, доказанные в границах (проверка снята):\n" + proofs;
    out += "экземпляры (рантайм-часть):\n" + instances();
    // Staticity level (§12.3): what is left to check in runtime and what exceptions are possible
    std::string lv;
    for(auto const & [key, count]: instance_counts) {
      if(prelude_instance(key)) continue;
      auto ck = rt_checks.find(key);
      auto th = throws_of.find(key);
      std::size_t nc = ck == rt_checks.end() ? 0 : ck->second.size();
      std::size_t nt = th == throws_of.end() ? 0 : th->second.size();
      lv += cat("  ", key, ": ", nc == 0 && nt == 0 ? "всё доказано статически" : cat("рт-проверок ", nc, ", исключений ", nt), "\n");
      if(ck != rt_checks.end())
        for(auto const & [id, c]: ck->second)
          if(mine(c.first)) lv += cat("    ", loc(c.first), " ", c.second, "\n");
    }
    if(!lv.empty()) out += "статичность экземпляров:\n" + lv;
    return out;
  }

  bool prelude_instance(std::string const & key) const {
    if(!db.prelude) return false;
    auto name = key.substr(0, key.find('('));
    for(node_id item: db.as<n_module>(db.prelude).items)
      if(db.is<n_fn>(item) && db.strings.text(db.as<n_fn>(item).name) == name) return true;
    return false;
  }

  // static_level(f) is the static level of a function as a value (§12.3): parsing of the
  // entire program, for all instance f is the number of run-time checks and possible
  // exceptions.
  value static_level(symbol f, span where) {
    agg_v g{types.named("static_level"), {}};
    if(level_probe) { // inside parsing for static_level - value unknown (external requirements checked)
      g.fields = {{sym("checks"), erase(value(int64_t(0)))}, {sym("throws"), erase(value(int64_t(0)))}, {sym("detail"), erase(value(std::string()))}};
      return value(std::move(g));
    }
    if(level_runs.empty()) {
      level_runs.emplace_back(db, mod, std::vector<std::string>{"level"}, mode::check, std::nullopt, nullptr, this);
      interpreter & a = level_runs.back();
      a.level_probe = true;
      a.fuel = 2000000;
      try {
        a.run_main();
      } catch(diagnostic const & d) {
        fail(cat("static_level: разбор программы не прошёл: ", d.what()), where);
      }
    }
    interpreter & a = level_runs.back();
    auto prefix = cat(str(f), "(");
    std::set<std::string> checks, throws;
    std::string detail;
    for(auto const & [key, n]: a.instance_counts) {
      if(!key.starts_with(prefix)) continue;
      if(auto it = a.rt_checks.find(key); it != a.rt_checks.end())
        for(auto const & [id, c]: it->second)
          if(checks.insert(id).second) {
            auto lc = locate(db.source(c.first.source), c.first.begin);
            detail += cat(detail.empty() ? "" : "; ", c.second, " в строке ", lc.line);
          }
      if(auto it = a.throws_of.find(key); it != a.throws_of.end()) throws.insert(it->second.begin(), it->second.end());
    }
    if(!throws.empty()) detail += cat(detail.empty() ? "" : "; ", "исключения: ", join(std::vector<std::string>(throws.begin(), throws.end())));
    level_details.push_back(cat(str(f), ": ", detail.empty() ? "всё доказано" : detail));
    g.fields = {{sym("checks"), value(int64_t(checks.size()))}, {sym("throws"), value(int64_t(throws.size()))}, {sym("detail"), value(detail)}};
    return value(std::move(g));
  }

  // Indexing proved by parsing within the array boundaries (“top-up optimization”: the
  // language knows that the check is removed, and shows it in the report). The index is
  // proven in all disassembled calculations of node (JIT removes the check).
  bool bounds_proven(node_id n) const { return proven.contains(n.value) && !unproven.contains(n.value); }
  // Does the function change its k parameter (inference, see writes_param).
  bool param_written(node_id f, uint32_t k) const { return writes_param(f, k); }
  // --- flow ---

  // clang-format off
  enum class flow_kind : uint8_t { normal, ret, brk, cont, thr }; // thr - second exit path (exception)
  // clang-format on

  struct flow {
    flow_kind kind = flow_kind::normal;
    value v;
    uint32_t target = 0; // a frame from which comes the return

    bool normal() const { return kind == flow_kind::normal; }
  };

  static flow ok(value v = {}) { return flow{flow_kind::normal, std::move(v), 0}; }

  struct call_site {
    span where;
    std::string what;
    uint32_t node = 0; // call node: from the chain of such node is formed the context of embedding
    uint32_t frame = 0; // call-point
  };

  struct frame_rec {
    uint32_t id;
    symbol name;
    bool outline;
    bool rt_effect; // in the frame was runtime - effect (I/O) - the call does not fold
    uint32_t caller = 0; // the frame in which the call was calculated (the block of the design is the frame of its calling)
  };

  // Redefinition of context operations (§8.2):
//.
  struct override_rec {
    symbol entity; // infix+
    node_id fn; // using operator …
    symbol context; // function-context (checked)
  };

  // The state of execution, each fiber (switches with the stack).
  struct exec_state {
    std::vector<frame_rec> frames;
    std::vector<call_site> stack, throw_stack;
    std::vector<std::pair<uint32_t, bool>> frame_floors;
    std::vector<std::vector<value> *> throw_sinks;
    std::vector<override_rec> overrides;
    uint32_t override_depth = 0;
    uint64_t ctx = 0;
    std::string path;
  };

  struct fiber_rec {
    uint32_t id = 0;
    enum state_t : uint8_t { ready,
      blocked,
      done } state = ready;
    void * rsp = nullptr;
    std::optional<fiber_stack> stack; // No - the main host stack
    exec_state exec;
    node_id body;
    scope * env = nullptr;
    uint32_t frame = 0;
    uint64_t body_ctx = 0;
    std::optional<diagnostic> error;
    std::optional<diagnostic> pending; // Error that fiber will get when it wakes up (mutual lock)
    bool cancel = false; // end of the program: spin the fiber stack
    bool direct = false; // In the queue first: awakened by exchange (direct transmission)
    symbol entry; // Process input: function and arguments (M3c)
    std::vector<value> entry_args;
    uint32_t proc = 0;
    interpreter * vm = nullptr;
  };

  // Flow: Value buffer and waiting fiber. The value in the stream goes by
  // displacement (write(own …)): exchange without copies at the language level.
  struct stream_rec {
    std::deque<value> buf;
    std::size_t cap = 16;
    bool closed = false;
    std::deque<fiber_rec *> readers, writers;
    value shape; // Element type: parsing - from records; execution - from parsing of the entire program (JIT)
    bool typed = false;
    uint64_t born_ctx = 0, born_at = 0; // Place of creation: context of embedding and place in the text
    // host (M3b): network - event stream, connections - peers; file - stream of pieces
    struct peer {
      int fd = -1;
      bool open = true;
      std::deque<std::string> out; // outgoing: held until completion of the application
      std::size_t sent = 0;
      bool sending = false;
    };
    bool net = false, connected = false, file_write = false, io_busy = false;
    int listen_fd = -1, file_fd = -1;
    std::vector<peer> peers;
    sockaddr_in addr{};
    uint64_t offset = 0;
    std::string io_buf;
  };
  struct proc_meta_rec {
    interpreter * vm = nullptr;
    bool done = false, failed = false;
    std::string error;
    std::vector<fiber_rec *> waiters;
  };

  bool checking() const { return run_mode == mode::check; }

  // --- call-trace errors ---

  // runtime - VM error - exclusion of the language of its type (caught catch(T e)).
  [[noreturn]] void fail_rt(std::string_view type, std::string msg, span where) {
    diagnostic d(std::move(msg), where, diag_kind::runtime);
    d.type = type;
    if(!path.empty()) d.note(cat("в продолжении ", path), where);
    for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
    throw d;
  }

  [[noreturn]] void fail(std::string msg, span where, diag_kind kind = diag_kind::instantiation) {
    diagnostic d(std::move(msg), where, kind);
    if(!path.empty()) d.note(cat("в продолжении ", path), where);
    for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
    throw d;
  }

  symbol sym(std::string_view s) { return db.strings.intern(s); }
  std::string_view str(symbol s) const { return db.strings.text(s); }
  std::string show_v(value const & x) const { return show(x, types, db.strings); }

  static std::string join(std::vector<std::string> const & xs) {
    std::string s;
    for(auto & x: xs) s += (s.empty() ? "" : " | ") + x;
    return s;
  }

#define TZ_TRY(var, expr)               \
  flow var##_f = (expr);                \
  if(!var##_f.normal()) return var##_f; \
  value var = std::move(var##_f.v)

  // --- arena ---

  scope * new_scope(scope * parent) {
    scope * s;
    if(!free_scopes.empty()) {
      s = free_scopes.back();
      free_scopes.pop_back();
    } else {
      s = &scope_arena.emplace_back();
    }
    s->parent = parent;
    s->book = checking() ? &book : nullptr;
    return s;
  }

  void drop_scope(scope * s) {
    if(s->captured) return;
    s->vars.clear();
    s->storage.clear();
    s->parent = nullptr;
    free_scopes.push_back(s);
  }

  // Lambda holds his entourage and all his ancestors.
  static void capture(scope * s) {
    for(; s && !s->captured; s = s->parent) s->captured = true;
  }

  // Copy of the area for cloned continuation (checkout mode).
  scope * clone_scope(scope * from) {
    scope * c = new_scope(from->parent);
    for(auto & [name, cell]: from->vars) c->declare(name, *cell);
    return c;
  }

  struct scope_hold { // The area returns to the arena and when excluded
    interpreter & vm;
    scope * s;
    ~scope_hold() { vm.drop_scope(s); }
  };

  // Iteration of the cycle writes in journal cells (removed and when excluded).
  struct log_hold {
    interpreter & vm;
    write_log * log;
    ~log_hold() { std::erase(vm.book.logs, log); }
  };
  struct loop_hold {
    interpreter & vm;
    ~loop_hold() { vm.loops.pop_back(); }
  };

  // --- facts about values (safe index, §12.1) ---
  //
  // 
// Fact is tied.
  // to the cell of the array, its name and version: replaced the array - the fact is
  // invalid; the growth of the array does not break it.
  struct fact {
    enum kind_t : uint8_t { index_of,
      size_of,
      range_of } kind;
    value const * cell;
    symbol name;
    uint64_t version;
    auto operator<=>(fact const &) const = default;
  };

  uint32_t add_fact(fact f) {
    auto [it, fresh] = fact_ids.try_emplace(f, uint32_t(facts.size() + 1));
    if(fresh) facts.push_back(f);
    return it->second;
  }

  fact const * fact_of(value const & v, fact::kind_t k) const {
    if(!v.proof || v.proof > facts.size() || facts[v.proof - 1].kind != k) return nullptr;
    return &facts[v.proof - 1];
  }

  bool holds(fact const & f, value const * cell, symbol name) const {
    return f.cell == cell && f.name == name && book.version(cell) == f.version;
  }

  // Write to place: The cell version (replacement of the whole), journal iteration, and the
  // fact of the array-value itself (r - range(…)) is removed when the elements change.
  void written(node_id target, scope * env) {
    if(!checking()) return;
    bool whole = db.is<n_ident>(target);
    while(db.is<n_index>(target) || db.is<n_member>(target))
      target = db.is<n_index>(target) ? db.as<n_index>(target).object : db.as<n_member>(target).object;
    if(!db.is<n_ident>(target)) return;
    value * cell = env->find(db.as<n_ident>(target).name);
    if(!cell || cell->is<thunk_v>()) return;
    if(cell->is<ref_v>()) { // recording through a link - in the support element
      cell = cell->as<ref_v>().cell;
      whole = false;
    }
    if(!whole) cell->proof = 0;
    book.written(cell, whole);
  }

  void trace_step(node_id n, flow const & f) {
    if(tracer->size() >= trace_limit) return;
    std::string v = f.kind == flow_kind::normal ? show_v(f.v) : f.kind == flow_kind::thr ? "исключение" : f.kind == flow_kind::ret ? "return" : "";
    if(v.size() > 60) { // boundary of the symbol UTF-8
      std::size_t cut = 57;
      while(cut > 0 && (static_cast<unsigned char>(v[cut]) & 0xC0) == 0x80) --cut;
      v = v.substr(0, cut) + "…";
    }
    tracer->push_back({n.value, uint32_t(frames.size()), captured ? uint32_t(captured->size()) : 0, std::move(v)});
  }

  flow eval(node_id n, scope * env, uint32_t frame) {
    if(!checking()) {
      if(!tracer) return std::visit([&](auto const & k) { return eval_node(n, k, env, frame); }, db[n].kind);
      flow f = std::visit([&](auto const & k) { return eval_node(n, k, env, frame); }, db[n].kind);
      trace_step(n, f);
      return f;
    }
    if(fuel && --*fuel == 0) fail("бюджет статического разбора исчерпан", db[n].where);
    // Check mode: observation - what value the node received and whether there was a
    // runtime effect (necessary for residual program, §13.2)
    uint64_t effects_before = effects;
    flow f = std::visit([&](auto const & k) { return eval_node(n, k, env, frame); }, db[n].kind);
    observe(n, f, effects != effects_before);
    return f;
  }

  // All changes to the report go through emit: so they can be written to the
  // instance cache and reproduced when hit.
  void emit(report_event ev) {
    for(auto * rec: recording) rec->push_back(ev);
    switch(ev.kind) {
      case report_event::instance: ++instance_counts[ev.a]; break;
      case report_event::folded: ++folded[ev.a]; break;
      case report_event::stage: stages.push_back({ev.where, ev.a, ev.b, ev.c}); break;
      case report_event::split: splits.push_back({ev.where, ev.a, ev.alts}); break;
      case report_event::excluded: excluded.push_back({ev.where, ev.a, ev.b}); break;
      case report_event::live: live_fns.insert(ev.fn); break;
      case report_event::proven:
        if(proven.insert(ev.fn).second) proven_list.push_back({ev.where, ev.a, ev.fn});
        break;
      case report_event::unproven: unproven.insert(ev.fn); break;
      case report_event::rt_check: rt_checks[ev.a][cat(ev.where.source, ":", ev.where.begin, ":", ev.b)] = {ev.where, ev.b}; break;
      case report_event::throws: throws_of[ev.a].insert(ev.b); break;
      case report_event::resolved: {
        auto & ts = resolved_calls[ev.fn];
        uint32_t t = 0;
        std::from_chars(ev.b.data(), ev.b.data() + ev.b.size(), t);
        if(!std::ranges::contains(ts, t)) ts.push_back(t);
        break;
      }
    }
  }

  // Observations are written in journal: the call, completely folded into compile-time,
  // rolls back its part - it will not be in runtime, and its values should not interfere
  // with the convolution of living code.
  struct obs_entry {
    uint32_t node;
    value v;
    bool seen, diverted, effect;
    uint64_t ctx = 0; // Embedded context (node chain)
    // inside a call folded in compile-time: runtime doesn't have it (residual program
    // doesn't see it), but it's a context of use - types for editor
    bool folded = false;
  };

  void observe(node_id n, flow const & f, bool effect) {
    if(!f.normal()) journal.push_back({n.value, {}, false, true, effect, ctx});
    else journal.push_back({n.value, f.v, true, false, effect, ctx});
    observations_dirty = true;
  }

  void mark_executed(node_id n) {
    journal.push_back({n.value, {}, false, false, false, ctx});
    observations_dirty = true;
  }

  void build_observed() const {
    if(!observations_dirty) return;
    observations.clear();
    observations_ctx.clear();
    auto add = [&](observation & o, obs_entry const & e) {
      o.effect |= e.effect;
      o.diverted |= e.diverted;
      o.present = true;
      if(e.seen) {
        o.v = o.seen ? const_cast<interpreter *>(this)->merge(o.v, e.v) : e.v;
        o.seen = true;
      }
    };
    for(auto const & e: journal) {
      if(e.folded) continue;
      add(observations[e.node], e);
      add(observations_ctx[{e.node, e.ctx}], e);
    }
    observations_dirty = false;
  }

  // The value of the variable; thunk (expr parameter) is calculated when mentioned.
  flow read(value * cell, span where = {}) {
    if(cell->is<ref_v>()) return ok(deref(cell->as<ref_v>(), where));
    if(cell->is<thunk_v>()) {
      thunk_v t = cell->as<thunk_v>();
      if(db.is<n_lambda>(t.expr) && db.as<n_lambda>(t.expr).block)
        fail("блок конструкции вводит имена — его вызывают с аргументами: body(x)", db[t.expr].where);
      uint64_t saved = std::exchange(ctx, t.ctx); // expression of the caller in its context
      flow r;
      try {
        r = eval(t.expr, t.env, t.frame);
      } catch(...) {
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
  value merge(value const & a, value const & b) {
    std::vector<value> alts;
    auto add = [&](value const & x) {
      if(x.is<split_v>()) {
        for(auto & y: x.as<split_v>().alts) add_alt(alts, y);
      } else add_alt(alts, x);
    };
    add(a);
    add(b);
    return alts.size() == 1 ? alts[0] : value(split_v{std::move(alts)});
  }

  void add_alt(std::vector<value> & alts, value const & x) {
    if(unknown(x)) {
      if(alts.empty()) alts.push_back(x);
      return;
    } // ⊥ ⊔ T = T
    if(alts.size() == 1 && unknown(alts[0])) {
      alts[0] = x;
      return;
    }
    for(auto & y: alts) {
      if(types.describe(y) != types.describe(x)) continue;
      if(state_of(y) != state_of(x)) continue; // Another state (ct field, hide) - another alternative
      if(y.known() && x.known() && show_v(y) == show_v(x)) return; // calculus
      y = erase(y); // same type, run-time
      return;
    }
    alts.push_back(x);
  }

  // Apply f to each split value alternative.
  value lift(value const & x, auto && f) {
    if(!x.is<split_v>()) return f(x);
    value out;
    bool first = true;
    for(auto const & a: x.as<split_v>().alts) {
      value r = f(a);
      out = first ? r : merge(out, r);
      first = false;
    }
    return out;
  }

  // bool, known statically; nullopt is run-time-bool (branch by runtime).
  std::optional<bool> known_bool(value const & v, span where) {
    if(v.is<bool>()) return v.as<bool>();
    if(unknown(v)) return std::nullopt;
    if(v.is<rt_v>() && types.describe(v) == "bool") return std::nullopt;
    if(v.is<split_v>()) return std::nullopt;
    fail(cat("условие должно быть bool, получено ", types.describe(v)), where);
  }

  bool truthy(value const & v, span where) {
    auto b = known_bool(v, where);
    if(!b) fail("условие неизвестно статически", where);
    return *b;
  }

  // Returns accumulated by branches under run-time-condition: frame → values.
  void record_return(flow const & f) {
    may_return[f.target].push_back(f.v);
    // return to external frame (return(caller) over run-time-branch): calls for which the
    // target is external cannot be cached
    for(auto & [floor, outer]: frame_floors)
      if(f.target <= floor) outer = true;
  }

  // Only instance is cached with no side effect on the caller: no expr-arguments
  // (inline-function is part of the caller) and no mut/own/out-parameters (changes
  // of the caller cells cannot be reproduced).
  bool has_ct(node_id n) const {
    if(!n) return false;
    if(db.is<n_ct>(n)) return true;
    bool found = false;
    db.for_each_child(n, [&](node_id c) { found = found || has_ct(c); });
    return found;
  }

  bool memoizable(node_id f, std::vector<value> const & vals) const {
    for(auto const & v: vals)
      if(v.is<thunk_v>()) return false;
    if(has_ct(db.as<n_fn>(f).body)) return false; // ct - at the place of the call, not by argument
    for(node_id p: db.as<n_fn>(f).params) {
      auto m = db.as<n_param>(p).mode;
      if(m == param_mode::mut || m == param_mode::own || m == param_mode::out || m == param_mode::expr) return false;
    }
    return true;
  }

  // The result of branching under run-time-condition: a path that does not
  // continue (return, throw, break) is registered as possible and does not go
  // further; parsing continues along a different path.
  flow join_branches(flow a, flow b) {
    if(a.normal() && b.normal()) return ok(merge(a.v, b.v));
    if(a.kind == flow_kind::thr && b.kind == flow_kind::thr) return flow{flow_kind::thr, merge(a.v, b.v), a.target};
    if(a.kind == flow_kind::ret && b.kind == flow_kind::ret && a.target == b.target)
      return flow{flow_kind::ret, merge(a.v, b.v), a.target};
    if(a.kind == b.kind && (a.kind == flow_kind::brk || a.kind == flow_kind::cont)) return a;
    auto aside = [&](flow const & f) {
      if(f.kind == flow_kind::thr) record_throw(f.v); // second
      else if(f.kind == flow_kind::ret) record_return(f);
      else if(f.kind == flow_kind::brk) may_break();
      // continue: итерация кончилась раньше — следующая разбирается и так
    };
    if(a.normal()) return aside(b), a;
    if(b.normal()) return aside(a), b;
    // Both paths do not continue: the cycle flow (break/continue) is more important - it is local
    bool a_loop = a.kind == flow_kind::brk || a.kind == flow_kind::cont;
    bool b_loop = b.kind == flow_kind::brk || b.kind == flow_kind::cont;
    if(b_loop && !a_loop) std::swap(a, b);
    else if(!a_loop && b.kind == flow_kind::thr) std::swap(a, b);
    aside(b);
    return a;
  }

  void may_break() {
    if(loops.empty()) fail("break вне цикла", {});
    loops.back().may_break = true;
  }

  // --- node: Literaries and names ---

  flow eval_node(node_id, n_int const & x, scope *, uint32_t) { return ok(value(x.value)); }
  flow eval_node(node_id, n_float const & x, scope *, uint32_t) { return ok(value(x.value)); }
  flow eval_node(node_id, n_str const & x, scope *, uint32_t) { return ok(value(x.value)); }

  flow eval_node(node_id n, n_ident const & x, scope * env, uint32_t) {
    if(value * cell = env->find(x.name)) {
      not_hidden(*cell, x.name, db[n].where);
      return read(cell, db[n].where);
    }
    if(str(x.name) == "true") return ok(value(true));
    if(str(x.name) == "false") return ok(value(false));
    if(auto t = types.builtin(str(x.name))) return ok(value(type_v{t}));
    if(structs.contains(x.name)) return ok(value(type_v{types.named(str(x.name))})); // entity struct - value
    if(auto it = functions.find(x.name); it != functions.end()) {
      for(node_id f: it->second)
        if(inline_required(f))
          fail(cat("функция ", str(x.name), " использует семантику инлайна (expr-параметр или return(…)) и не может быть значением; вызывайте её напрямую"),
              db[n].where);
      return ok(value(fn_v{x.name, {}, nullptr}));
    }
    fail(cat("неизвестное имя ", str(x.name)), db[n].where);
  }

  // --- blockages ---

  flow eval_node(node_id n, n_block const & b, scope * env, uint32_t frame) {
    scope * inner = new_scope(env);
    scope_hold hold{*this, inner};
    return run_block(b, 0, inner, frame);
  }

  // Execute the block instructions starting with from. In verification mode,
  // declaring a split-value variable clones a continuation.
  flow run_block(n_block const & b, uint32_t from, scope * inner, uint32_t frame) {
    value last;
    for(uint32_t i = from; i < b.items.size(); ++i) {
      node_id s = b.items[i];
      if(checking() && implicit_declaration(s, inner)) { // x = e without name - as an announcement: splitting
        auto const & o = db.as<n_op>(s);
        mark_executed(s);
        TZ_TRY(init, eval(o.args[1], inner, frame));
        implicit_decls.insert(s.value);
        if(init.is<split_v>()) return split_continuation(b, i, s, init, inner, frame);
        emit({report_event::stage, path, std::string(str(db.as<n_ident>(o.args[0]).name)), init.known() ? cat("кт ", show_v(init)) : cat("рт ", types.describe(init)), db[s].where, {}});
        inner->declare(db.as<n_ident>(o.args[0]).name, std::move(init));
        continue;
      }
      if(checking() && db.is<n_var>(s) && !db.as<n_var>(s).ref) {
        auto const & v = db.as<n_var>(s);
        mark_executed(s); // instruction
        TZ_TRY(init, eval(v.init, inner, frame));
        if(init.is<split_v>()) return split_continuation(b, i, s, init, inner, frame);
        declare_var(s, v, std::move(init), inner);
        continue;
      }
      flow f = eval(s, inner, frame);
      if(!f.normal()) return f;
      last = std::move(f.v);
    }
    return ok(b.has_value ? std::move(last) : value());
  }

  flow split_continuation(n_block const & b, uint32_t i, node_id s, value const & init, scope * inner, uint32_t frame) {
    n_var v = db.is<n_var>(s) ? db.as<n_var>(s) : n_var{db.as<n_ident>(db.as<n_op>(s).args[0]).name, {}, db.as<n_op>(s).args[1]};
    std::vector<std::string> names;
    for(auto const & a: init.as<split_v>().alts) names.push_back(types.describe(a));
    emit({report_event::split, std::string(str(v.name)), {}, {}, db[s].where, names});

    flow result;
    bool first = true;
    for(auto const & alt: init.as<split_v>().alts) {
      scope * cont = clone_scope(inner);
      scope_hold hold{*this, cont};
      std::string saved = path;
      path += cat("[", str(v.name), ": ", types.describe(alt), "] ");
      declare_var(s, v, alt, cont);
      flow f = run_block(b, i + 1, cont, frame);
      path = saved;
      result = first ? f : join_branches(result, f);
      first = false;
    }
    return result;
  }

  bool implicit_declaration(node_id s, scope * env) const {
    if(!db.is<n_op>(s) || db.strings.text(db.as<n_op>(s).op) != "=" || !db.is<n_ident>(db.as<n_op>(s).args[0])) return false;
    return !env->find(db.as<n_ident>(db.as<n_op>(s).args[0]).name);
  }
  // The assignment that declares the variable (x = e without a name) is for projection.
  bool declares(node_id n) const { return implicit_decls.contains(n.value); }

  void declare_var(node_id s, n_var const & v, value init, scope * env) {
    if(v.type && db.is<n_ident>(v.type)) check_type(init, db.as<n_ident>(v.type).name, db[v.init].where);
    if(checking())
      emit({report_event::stage, path, std::string(str(v.name)),
          init.known() ? cat("кт ", show_v(init)) : cat("рт ", types.describe(init)), db[s].where, {}});
    env->declare(v.name, std::move(init));
  }

  flow eval_node(node_id n, n_var const & v, scope * env, uint32_t frame) {
    if(v.ref) return declare_ref(n, v, env, frame);
    TZ_TRY(init, eval(v.init, env, frame));
    declare_var(n, v, std::move(init), env);
    return ok();
  }

  flow eval_node(node_id n, n_fn const & f, scope * env, uint32_t) {
    if(f.scoped) { // using operator ... - to the end of the frame of the function-context, for all executable
      overrides.push_back({f.name, n, frames.empty() ? symbol{} : frames.back().name});
      return ok();
    }
    capture(env);
    env->declare(f.name, value(fn_v{{}, n, env})); // local function = lambda with a name
    return ok();
  }

  flow eval_node(node_id n, n_return const & r, scope * env, uint32_t frame) {
    value v;
    if(r.value) {
      TZ_TRY(x, eval(r.value, env, frame));
      v = std::move(x);
    }
    uint32_t target = r.target ? return_target(db.as<n_ident>(r.target).name, frame, db[n].where) : frame;
    return flow{flow_kind::ret, std::move(v), target};
  }

  // return(f) is the output from the external function f in the call chain;
  // return(caller) is the output from the calling function.
// The outline boundary is not allowed: there is no inline.
  // Chain - call locations, not stack: the call in for(…) { … } block is made from
  // the function that wrote the block, not from the for itself.
  uint32_t return_target(symbol name, uint32_t frame, span where) {
    auto at = [&](uint32_t id) -> frame_rec const * {
      for(auto it = frames.rbegin(); it != frames.rend(); ++it)
        if(it->id == id) return &*it;
      return nullptr;
    };
    frame_rec const * cur = at(frame);
    if(!cur) fail("return вне функции", where);
    bool to_caller = str(name) == "caller";
    while(frame_rec const * up = cur->caller ? at(cur->caller) : nullptr) {
      if(cur->outline)
        fail(cat("return(", str(name), ") через outline-функцию ", str(cur->name), ": у неё нет семантики инлайна"), where);
      // Operator frame is transparent: x? is a try(x) call record, not a new function
      if(up->name && operator_name(str(up->name))) {
        cur = up;
        continue;
      }
      if(to_caller || up->name == name) return up->id;
      cur = up;
    }
    fail(cat("return(", str(name), "): нет такой внешней функции в цепочке вызовов"), where);
  }

  flow eval_node(node_id n, n_require const & r, scope * env, uint32_t frame) {
    TZ_TRY(c, eval(r.cond, env, frame));
    auto b = known_bool(c, db[r.cond].where);
    if(b && !*b) {
      std::string why; // static_level - with a list of unproven
      for(auto const & d: level_details) why += cat("; ", d);
      fail_rt("require_failed", cat("нарушено требование require", why), db[n].where);
    }
    if(checking() && !b) rt_check("require_failed", db[n].where);
    return ok(); // run-time - Condition: runtime (§5.4)
  }

  // --- branching ---

  // --- unpacking (P12) ---
  //
  // 
// 
// 
// 
//.

  enum class unpacked { no, yes, maybe }; // Maybe - only in parsing (length/value - run-time)
  static unpacked both(unpacked a, unpacked b) {
    if(a == unpacked::no || b == unpacked::no) return unpacked::no;
    return a == unpacked::maybe || b == unpacked::maybe ? unpacked::maybe : unpacked::yes;
  }

  // Sample checks (types and values of elements) - its specificity among overloads.
  uint32_t pattern_checks(node_id pat) const {
    uint32_t n = 0;
    for(node_id x: db.as<n_pat>(pat).items) {
      if(db.is<n_pat>(x)) n += 1 + pattern_checks(x);
      else if(!db.is<n_param>(x) || db.as<n_param>(x).type) ++n;
    }
    return n;
  }

  // Value in sample: names in into; values-elements are calculated in env.
  unpacked unpack_into(node_id pat, value const & v, scope * into, scope * env, uint32_t frame, span where) {
    auto const & p = db.as<n_pat>(pat);
    std::string_view kind = str(db.as<n_ident>(p.type).name);
    value const * x = shape_of(v);
    if(unknown(v) || !x) return unpacked::maybe; // Type unknown even at parsing
    auto elem = [&](node_id item, value const & e) -> unpacked {
      if(db.is<n_pat>(item)) return unpack_into(item, e, into, env, frame, where);
      if(db.is<n_param>(item)) {
        auto const & b = db.as<n_param>(item);
        if(b.type) {
          auto want = str(db.as<n_ident>(b.type).name);
          if(types.describe(e) != want && types.name(types.of(e)) != want) return unpacked::no;
        }
        if(str(b.name) != "_") into->declare(b.name, e);
        return unpacked::yes;
      }
      flow f = eval(item, env, frame); // Equality of the element
      value eq = binary("==", e, f.v, db[item].where);
      if(!eq.is<bool>()) return unpacked::maybe;
      return eq.as<bool>() ? unpacked::yes : unpacked::no;
    };
    if(kind == "array") {
      if(!x->is<array_v>()) return unpacked::no;
      auto const & a = x->as<array_v>();
      std::size_t rest = p.items.size();
      for(std::size_t k = 0; k < p.items.size(); ++k)
        if(db.is<n_param>(p.items[k]) && db.as<n_param>(p.items[k]).pack) {
          if(rest != p.items.size()) fail("в образце — один ...остаток", where);
          rest = k;
        }
      std::size_t fixed = p.items.size() - (rest < p.items.size() ? 1 : 0);
      bool sized = v.known() || !v.is<rt_v>(); // length known: value (not erased array)
      if(sized && (rest < p.items.size() ? a.size() < fixed : a.size() != fixed)) return unpacked::no;
      unpacked r = sized ? unpacked::yes : unpacked::maybe;
      value any = sized ? value() : element_type(v, where);
      std::size_t tail = rest < p.items.size() ? p.items.size() - rest - 1 : 0;
      for(std::size_t k = 0; k < p.items.size(); ++k) {
        if(k == rest) { // ...the remainder is an array of remaining
          auto const & b = db.as<n_param>(p.items[k]);
          value part = v;
          if(sized) {
            std::vector<value> xs;
            for(std::size_t j = k; j + tail < a.size(); ++j) xs.push_back(a.get(uint32_t(j)));
            part = value(array_v::of(std::move(xs)));
          }
          if(str(b.name) != "_") into->declare(b.name, part);
          continue;
        }
        std::size_t at = k < rest ? k : a.size() - (p.items.size() - k);
        r = both(r, elem(p.items[k], sized ? a.get(uint32_t(at)) : any));
        if(r == unpacked::no) return r;
      }
      return r;
    }
    if(!structs.contains(db.as<n_ident>(p.type).name)) fail(cat("образец ", kind, "{…}: не struct и не array"), db[p.type].where);
    if(!x->is<agg_v>() || types.name(x->as<agg_v>().type) != kind) return unpacked::no;
    auto const & fs = x->as<agg_v>().fields;
    if(fs.size() != p.items.size()) fail(cat("образец ", kind, "{…}: полей ", fs.size(), ", элементов ", p.items.size()), where);
    unpacked r = unpacked::yes;
    for(std::size_t k = 0; k < fs.size() && r != unpacked::no; ++k) {
      if(db.is<n_param>(p.items[k]) && db.as<n_param>(p.items[k]).pack) fail("...остаток — только в array{…}", db[p.items[k]].where);
      r = both(r, elem(p.items[k], fs[k].second));
    }
    return r;
  }

  // Condition: success of unpacking - bool (in parsing unknown - run-time).
  flow unpack_test(node_id n, scope * env, uint32_t frame, scope * into) {
    auto const & u = db.as<n_unpack>(n);
    TZ_TRY(v, eval(u.value, env, frame));
    unpacked r = unpack_into(u.pattern, v, into, env, frame, db[n].where);
    if(r == unpacked::maybe) return ok(erase(value(true)));
    return ok(value(r == unpacked::yes));
  }

  // Instructions: names - in the current area; not unpacked - exception.
  flow eval_node(node_id n, n_unpack const & u, scope * env, uint32_t frame) {
    TZ_TRY(v, eval(u.value, env, frame));
    unpacked r = unpack_into(u.pattern, v, env, env, frame, db[n].where);
    if(r == unpacked::no)
      fail_rt("unpack_failed", cat("распаковка не удалась: ", printer(db).print(u.pattern), " ← ", checking() ? types.describe(v) : show_v(v)), db[n].where);
    if(r == unpacked::maybe) rt_check("unpack_failed", db[n].where);
    return ok();
  }

  flow eval_node(node_id n, n_pat const &, scope *, uint32_t) { fail("образец — только в распаковке", db[n].where); }

  flow eval_node(node_id n, n_if const & i, scope * env, uint32_t frame) {
    // if(образец = значение): Success is a condition, the sample names are only visible in the
    scope * then_env = env;
    std::optional<scope_hold> then_hold;
    if(db.is<n_unpack>(i.cond)) {
      then_env = new_scope(env);
      then_hold.emplace(*this, then_env);
    }
    TZ_TRY(c, db.is<n_unpack>(i.cond) ? unpack_test(i.cond, env, frame, then_env) : eval(i.cond, env, frame));
    auto b = known_bool(c, db[i.cond].where);
    auto proof = index_check(i.cond, env);
    auto then_ = [&] {
      if(!proof || !proof->positive) return eval(i.then_, then_env, frame);
      uint32_t saved = std::exchange(proof->cell->proof, proof->fact); // if(is_index(k, xs)) { xs[k] }
      flow t = eval(i.then_, env, frame);
      if(proof->cell->proof == proof->fact) proof->cell->proof = saved;
      return t;
    };
    // if(! is_index(k, xs) ) output; - then k - safe index xs
    auto after = [&](flow f, bool then_left) {
      if(proof && !proof->positive && then_left && f.normal()) proof->cell->proof = proof->fact;
      return f;
    };
    if(b) {
      if(*b) {
        flow t = then_();
        bool left = !t.normal();
        return after(std::move(t), left);
      }
      return after(i.else_ ? eval(i.else_, env, frame) : ok(), true);
    }
    // run-time -condition (only in check mode): both branches are disassembled from
    // the same state, after - the merger of the recorded cells (different types /
    // states - splitting continuation, the same type - run-time )
    write_log lt{book.clock, {}, {}, true};
    flow t;
    {
      book.logs.push_back(&lt);
      log_hold h{*this, &lt};
      t = then_();
    }
    bool left = !t.normal();
    std::map<value *, value> then_state;
    for(value * c: lt.cells) {
      then_state[c] = *c;
      *c = lt.old.at(c);
    }
    write_log le{book.clock, {}, {}, true};
    flow e;
    {
      book.logs.push_back(&le);
      log_hold h{*this, &le};
      e = i.else_ ? eval(i.else_, env, frame) : ok();
    }
    std::set<value *> touched = lt.cells;
    touched.insert(le.cells.begin(), le.cells.end());
    for(value * c: touched) {
      value tv = then_state.contains(c) ? then_state[c] : le.old.contains(c) ? le.old.at(c)
                                                                             : *c;
      if(!t.normal() && e.normal()) continue; // Then left (return/throw/break): state of
      if(t.normal() && !e.normal()) *c = std::move(tv);
      else *c = merge(tv, *c);
    }
    return after(join_branches(std::move(t), std::move(e)), left);
  }

  // is_index(k, xs)/! is_index(k, xs) over names: check in runtime, after which k is
  // the proven index xs (fact on the value of k).
  struct index_proof {
    value * cell;
    uint32_t fact;
    bool positive;
  };
  std::optional<index_proof> index_check(node_id cond, scope * env) {
    if(!checking()) return std::nullopt;
    bool positive = true;
    if(db.is<n_op>(cond) && str(db.as<n_op>(cond).op) == "!" && db.as<n_op>(cond).args.size() == 1) {
      positive = false;
      cond = db.as<n_op>(cond).args[0];
    }
    if(!db.is<n_call>(cond)) return std::nullopt;
    auto const & c = db.as<n_call>(cond);
    if(!db.is<n_ident>(c.callee) || str(db.as<n_ident>(c.callee).name) != "is_index" || c.args.size() != 2) return std::nullopt;
    if(functions.contains(db.as<n_ident>(c.callee).name)) return std::nullopt; // is_index is not a language test.
    if(!db.is<n_ident>(c.args[0]) || !db.is<n_ident>(c.args[1])) return std::nullopt;
    value * k = env->find(db.as<n_ident>(c.args[0]).name);
    value * a = env->find(db.as<n_ident>(c.args[1]).name);
    if(!k || !a || k->is<thunk_v>() || a->is<thunk_v>()) return std::nullopt;
    return index_proof{k, add_fact({fact::index_of, a, db.as<n_ident>(c.args[1]).name, book.version(a)}), positive};
  }

  // The core cycle. Break/continue is the second exit path, it is caught by the loop. In
  // check mode, iterations with a known outcome are executed; once the output depends on
  // runtime (break by run-time-condition), the number of iterations is unknown: the
  // cells that the iteration changes are erased to the type and the body is disassembled
  // to a fixed point. Cells are assembled by entries, not by names in the text - so are
  // the entries of construction blocks (for(…) { s += x; }).
  flow eval_node(node_id n, n_loop const & l, scope * env, uint32_t frame) {
    if(!checking()) {
      for(;;) {
        flow f = eval(l.body, env, frame);
        if(f.kind == flow_kind::brk) return ok();
        if(f.kind == flow_kind::ret || f.kind == flow_kind::thr) return f;
      }
    }
    loops.emplace_back();
    loop_hold lh{*this};
    for(uint32_t guard = 0;; ++guard) {
      loops.back().may_break = false;
      write_log log{book.clock};
      flow f;
      {
        book.logs.push_back(&log);
        log_hold wh{*this, &log};
        f = eval(l.body, env, frame);
      }
      if(!loops.back().may_break) {
        if(f.kind == flow_kind::brk) return ok();
        if(f.kind == flow_kind::ret || f.kind == flow_kind::thr) return f;
        if(guard > 100000) fail("цикл не сошёлся при статическом разборе", db[n].where);
        continue;
      }
      if(f.kind == flow_kind::ret) record_return(f);
      if(f.kind == flow_kind::thr) record_throw(f.v);
      return abstract_loop(l.body, env, frame, std::move(log.cells), n);
    }
  }

  flow abstract_loop(node_id body, scope * env, uint32_t frame, std::set<value *> cells, node_id n) {
    for(uint32_t pass = 0; pass < 8; ++pass) {
      std::vector<std::string> before;
      for(value * c: cells) {
        *c = erase(*c);
        before.push_back(types.describe(*c));
      }
      write_log log{book.clock};
      flow f;
      {
        book.logs.push_back(&log);
        log_hold wh{*this, &log};
        f = eval(body, env, frame);
      }
      if(f.kind == flow_kind::ret) record_return(f);
      if(f.kind == flow_kind::thr) record_throw(f.v);
      std::size_t had = cells.size();
      cells.insert(log.cells.begin(), log.cells.end());
      std::vector<std::string> after;
      for(value * c: cells) after.push_back(types.describe(erase(*c)));
      if(cells.size() == had && before == after) {
        for(value * c: cells) *c = erase(*c); // after the cycle - the value of any iteration
        return ok();
      }
    }
    fail("типы переменных цикла не сошлись", db[n].where);
  }

  flow eval_node(node_id, n_break const &, scope *, uint32_t) { return flow{flow_kind::brk, {}, 0}; }
  flow eval_node(node_id, n_continue const &, scope *, uint32_t) { return flow{flow_kind::cont, {}, 0}; }

  // Does the function change its k-th parameter (place of caller)? It is not forbidden
  // and does not require marking - it is displayed.
//  и
  // Seen in the check report. Mut is an optional explicit mark.
  bool writes_param(node_id f, uint32_t k) const {
    auto const & fn = db.as<n_fn>(f);
    if(k >= fn.params.size()) return false;
    auto m = db.as<n_param>(fn.params[k]).mode;
    if(m == param_mode::mut || m == param_mode::out || m == param_mode::own || m == param_mode::expr) return true;
    if(m == param_mode::ct) return false;
    uint64_t key = (uint64_t(f.value) << 32) | k;
    if(auto it = writes.find(key); it != writes.end()) {
      if(writes_open.contains(key)) writes_guess_used = true;
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
    if(!writes_open.empty()) {
      compute();
      return writes[key];
    }
    auto trues = [&] { return std::ranges::count_if(writes, [](auto const & e) { return e.second; }); };
    for(;;) {
      auto before = trues();
      writes_guess_used = false;
      std::erase_if(writes, [](auto const & e) { return !e.second; });
      compute();
      if(!writes_guess_used || trues() == before) return writes[key];
    }
  }

  // Does the code change the place with that name: assigning (including to an element/field)
  // or transferring to a parameter that changes the function being called? Shadow, stop.
  bool writes_name(node_id n, symbol name) const {
    if(!n || db.is<n_fn>(n)) return false;
    if(db.is<n_hide>(n)) return db.as<n_hide>(n).name == name; // Hide Self - Changing the location of the caller
    if(db.is<n_var>(n) && db.as<n_var>(n).ref) { // auto&e = name[i] - the entry in e changes the name
      node_id t = db.as<n_var>(n).init;
      while(db.is<n_index>(t)) t = db.as<n_index>(t).object;
      if(db.is<n_ident>(t) && db.as<n_ident>(t).name == name) return true;
    }
    if(db.is<n_lambda>(n)) { // construction block - part of the code; closure - no
      if(!db.as<n_lambda>(n).block) return false;
      for(node_id pn: db.as<n_lambda>(n).params)
        if(db.as<n_param>(pn).name == name) return false; // shadow
      return writes_name(db.as<n_lambda>(n).body, name);
    }
    if(db.is<n_block>(n)) {
      for(node_id k: db.as<n_block>(n).items) {
        if(writes_name(k, name)) return true;
        if(db.is<n_var>(k) && db.as<n_var>(k).name == name) return false;
      }
      return false;
    }
    if(db.is<n_op>(n)) {
      auto const & o = db.as<n_op>(n);
      auto op = str(o.op);
      if((op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") && !o.args.empty()) {
        node_id t = o.args[0];
        while(db.is<n_index>(t) || db.is<n_member>(t))
          t = db.is<n_index>(t) ? db.as<n_index>(t).object : db.as<n_member>(t).object;
        if(db.is<n_ident>(t) && db.as<n_ident>(t).name == name) return true;
      }
    }
    // The call (and the operator declared to be: a++ with a mut parameter) changes the place-argument
    auto via = [&](symbol callee, node_list const & args) {
      if(auto it = functions.find(callee); it != functions.end())
        for(uint32_t k = 0; k < args.size(); ++k) {
          node_id a = args[k];
          while(db.is<n_index>(a) || db.is<n_member>(a))
            a = db.is<n_index>(a) ? db.as<n_index>(a).object : db.as<n_member>(a).object;
          if(!db.is<n_ident>(a) || db.as<n_ident>(a).name != name) continue;
          for(node_id fn: it->second)
            if(writes_param(fn, k)) return true;
        }
      return false;
    };
    if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee) && via(db.as<n_ident>(db.as<n_call>(n).callee).name, db.as<n_call>(n).args)) return true;
    if(db.is<n_op>(n) && !user_ops.empty()) {
      auto const & o = db.as<n_op>(n);
      symbol f = db.strings.intern(cat(o.args.size() == 2 ? "infix" : o.postfix ? "postfix"
                                                                                : "prefix",
          str(o.op)));
      if(user_ops.contains(f.value) && via(f, o.args)) return true;
    }
    bool w = false;
    db.for_each_child(n, [&](node_id c) { w = w || writes_name(c, name); });
    return w;
  }

  // ct-code is executed at instantiation - once on instance (call location,
  // embedding context), in canonical order:
// 
//.
  flow eval_node(node_id n, n_ct const & c, scope * env, uint32_t frame) {
    auto key = std::pair{n.value, ctx};
    if(auto it = ct_memo.find(key); it != ct_memo.end()) return ok(it->second);
    ++ct_depth;
    flow f;
    try {
      f = eval(c.body, env, frame);
    } catch(...) {
      --ct_depth;
      throw;
    }
    --ct_depth;
    if(!f.normal()) return f;
    if(checking() && !f.v.known()) fail(cat("ct: значение не вычислимо при компиляции (рт ", types.describe(f.v), ")"), db[n].where);
    ct_memo[key] = f.v;
    if(checking() && !level_probe)
      ct_order.push_back({db[n].where, stack.empty() ? std::string("модуль") : stack.back().what, f.v.known() ? show_v(f.v) : std::string("рт")});
    return f;
  }

  // SFINAE block (§5.4):
// runtime - errors don't get jammed.
  // --- exceptions (§8.1 ) ---
  //
  // Second exit:
// In the interpreter, this is the view
  // Flow as address return; in run-time check mode, the cast is recorded in the
  // nearest try collector, and parsing goes both ways.

  void record_throw(value const & v) {
    if(!throw_sinks.empty()) throw_sinks.back()->push_back(v);
  }

  // Check remaining in the runtime (boundary, divider, require, overflow): possible
  // exclusion and static level string of all instances in the chain§12.3:).
  //
  void rt_check(std::string_view type, span where) {
    record_throw(vm_error_shape(type));
    for(auto const & k: instance_keys) emit({report_event::rt_check, k, std::string(type), {}, where, {}});
  }

  // Exclusion Type for catch(T e): The name of the value tag (aggregate is its type).
  std::string exception_type(value const & v) const {
    value const * s = shape_of(v);
    if(!s) return "?";
    return std::string(types.name(types.of(*s)));
  }

  // runtime is a VM error as an exception value: an aggregate of its type with a message.
  value vm_error_value(diagnostic const & d) {
    agg_v e{types.named(d.type.empty() ? "vm_error" : d.type), {}};
    e.fields.push_back({sym("message"), value(std::string(d.what()))});
    return value(std::move(e));
  }

  // Sample VM error for “painting” (message - runtime).
  value vm_error_shape(std::string_view type) {
    agg_v e{types.named(type), {}};
    e.fields.push_back({sym("message"), erase(value(std::string("x")))});
    return value(std::move(e));
  }

  [[noreturn]] void uncaught(flow const & r) {
    std::vector<std::string> names;
    for(auto const & alt: r.v.is<split_v>() ? r.v.as<split_v>().alts : std::vector<value>{r.v}) names.push_back(exception_type(alt));
    std::string what = checking() ? cat("возможно непойманное исключение ", join(names)) : cat("непойманное исключение ", join(names), ": ", show_v(r.v));
    diagnostic d(what, r.target ? db[node_id(r.target)].where : span{}, diag_kind::runtime);
    for(auto it = throw_stack.rbegin(); it != throw_stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
    throw d;
  }

  flow eval_node(node_id n, n_throw const & t, scope * env, uint32_t frame) {
    TZ_TRY(v, eval(t.value, env, frame));
    rt_effect();
    throw_stack = stack; // trailing
    return flow{flow_kind::thr, std::move(v), n.value};
  }

  // intercept(тело, блоки) is the exception kernel (§8.1), on it the library try { …
  // } catch(E e) { … }: body and blocks are expr-arguments, blocks are n_pack pack
  // from catch clauses. Catch the first block, whose parameter takes the type of
  // value thrown (auto - any). Break/continue is no exception: they are caught by
  // loop, catch(auto e) does not see them.
  flow intercept(node_list const & args, scope * env, uint32_t frame, span where) {
    if(args.size() != 2) fail("intercept(тело, блоки)", where);
    thunk_v hs = thunk_arg(args[1], env, where);
    node_list handlers = db.is<n_pack>(hs.expr) ? db.as<n_pack>(hs.expr).items : node_list{hs.expr};
    std::vector<value> thrown; // throws on the run-time-branches of the body (check mode)
    throw_sinks.push_back(&thrown);
    flow r;
    try {
      r = eval(args[0], env, frame);
    } catch(diagnostic const & d) {
      if(d.kind != diag_kind::runtime) {
        throw_sinks.pop_back();
        throw;
      }
      r = flow{flow_kind::thr, vm_error_value(d), 0}; // VM error: language exclusion
    }
    throw_sinks.pop_back();
    bool has_normal = r.kind != flow_kind::thr;
    if(r.kind == flow_kind::thr) {
      if(r.v.is<split_v>())
        for(auto const & a: r.v.as<split_v>().alts) thrown.push_back(a);
      else thrown.push_back(r.v);
    }
    flow result = has_normal ? r : flow{};
    bool have = has_normal;
    for(node_id h: handlers) {
      if(!db.is<n_lambda>(h) || db.as<n_lambda>(h).params.size() != 1) fail("catch: блок с одним параметром — catch(E e) { … }", db[h].where);
      auto const & l = db.as<n_lambda>(h);
      auto const & p = db.as<n_param>(l.params[0]);
      std::string want = p.type && db.is<n_ident>(p.type) ? std::string(str(db.as<n_ident>(p.type).name)) : std::string();
      std::vector<value> hit, rest;
      for(auto & v: thrown) (want.empty() || exception_type(v) == want ? hit : rest).push_back(std::move(v));
      thrown = std::move(rest);
      if(hit.empty()) continue;
      value caught = hit[0];
      for(std::size_t k = 1; k < hit.size(); ++k) caught = merge(caught, hit[k]);
      scope * inner = new_scope(hs.env);
      scope_hold hold{*this, inner};
      if(str(p.name) != "_") inner->declare(p.name, caught);
      uint64_t saved = std::exchange(ctx, hs.ctx);
      flow cb;
      try {
        cb = eval(l.body, inner, hs.frame);
      } catch(...) {
        ctx = saved;
        throw;
      }
      ctx = saved;
      result = have ? join_branches(result, cb) : cb;
      have = true;
      if(!checking()) break; // in runtime throw one - caught first suitable catch
    }
    if(thrown.empty()) return have ? result : ok();
    // Not caught here - further down the second path
    value up = thrown[0];
    for(std::size_t k = 1; k < thrown.size(); ++k) up = merge(up, thrown[k]);
    if(have) {
      for(auto const & v: thrown) record_throw(v);
      return result;
    }
    return flow{flow_kind::thr, up, r.target};
  }

  // expr-argument by name: thunk, unfolded to the expression of the caller (a
  // block transmitted further than another construction, too he).
  thunk_v thunk_arg(node_id arg, scope * env, span where) {
    value * cell = db.is<n_ident>(arg) ? env->find(db.as<n_ident>(arg).name) : nullptr;
    if(!cell || !cell->is<thunk_v>()) fail("ожидался expr-параметр", where);
    thunk_v t = cell->as<thunk_v>();
    while(db.is<n_ident>(t.expr))
      if(value * c = t.env->find(db.as<n_ident>(t.expr).name); c && c->is<thunk_v>()) t = c->as<thunk_v>();
      else break;
    return t;
  }

  // Calling a construction block with arguments: body(xs[k]) in for. The block is
  // the part of the caller (semantics inline): return and break from it go further.
  flow call_block(thunk_v const & t, node_list const & args, scope * env, uint32_t frame, span where) {
    auto const & l = db.as<n_lambda>(t.expr);
    if(l.params.size() != args.size()) fail(cat("блок ждёт ", l.params.size(), " аргументов, передано ", args.size()), where);
    scope * inner = new_scope(t.env);
    scope_hold hold{*this, inner};
    for(uint32_t i = 0; i < args.size(); ++i) {
      auto const & p = db.as<n_param>(l.params[i]);
      TZ_TRY(v, eval(args[i], env, frame));
      if(p.type && db.is<n_ident>(p.type)) check_type(v, db.as<n_ident>(p.type).name, db[args[i]].where);
      inner->declare(p.name, std::move(v));
    }
    uint64_t saved = std::exchange(ctx, t.ctx);
    flow r;
    try {
      r = eval(l.body, inner, t.frame);
    } catch(...) {
      ctx = saved;
      throw;
    }
    ctx = saved;
    return r;
  }

  flow eval_node(node_id, n_struct const &, scope *, uint32_t) { return ok(); } // declaration
  flow eval_node(node_id, n_extend const &, scope *, uint32_t) { return ok(); } // Expansion - when loading the module

  // --- code as a value (§7.4) ---

  value code_value(node_id n) {
    agg_v c{types.named("code"), {}};
    c.fields.push_back({sym("node"), value(int64_t(n.value))});
    return value(std::move(c));
  }

  node_id code_node(value const & v, span where) {
    if(v.is<agg_v>() && types.name(v.as<agg_v>().type) == "code") return node_id(uint32_t(v.as<agg_v>().fields[0].second.as<int64_t>()));
    fail("ожидался код (quote)", where);
  }

  // The value > node for inserting #x.
  node_id value_node(value const & v, span where) {
    if(v.is<int64_t>()) return db.add(n_int{v.as<int64_t>()}, where);
    if(v.is<double>()) return db.add(n_float{v.as<double>()}, where);
    if(v.is<std::string>()) return db.add(n_str{v.as<std::string>()}, where);
    if(v.is<bool>()) return db.add(n_ident{sym(v.as<bool>() ? "true" : "false")}, where);
    if(v.is<type_v>()) return db.add(n_ident{sym(types.name(v.as<type_v>().type))}, where);
    if(v.is<fn_v>() && v.as<fn_v>().name) return db.add(n_ident{v.as<fn_v>().name}, where);
    if(v.is<agg_v>() && types.name(v.as<agg_v>().type) == "code") return code_node(v, where);
    fail(cat("#: значение типа ", types.describe(v), " нельзя вставить в код"), where);
  }

  bool has_splice(node_id n) const {
    if(!n) return false;
    if(db.is<n_splice>(n)) return true;
    bool found = false;
    db.for_each_child(n, [&](node_id c) { found = found || has_splice(c); });
    return found;
  }

  flow eval_node(node_id n, n_quote const &, scope * env, uint32_t frame) {
    if(!has_splice(n)) return ok(code_value(n)); // without inserts - the node itself, without a copy
    std::string err;
    node_id copy = db.clone(n, [&](node_id x) -> std::optional<node_id> {
      if(!db.is<n_splice>(x)) return std::nullopt;
      symbol name = db.as<n_splice>(x).name;
      value * cell = env->find(name);
      if(!cell) fail(cat("#", str(name), ": нет такого значения"), db[x].where);
      flow v = read(cell);
      if(!v.v.known()) fail(cat("#", str(name), ": значение неизвестно при компиляции"), db[x].where);
      return value_node(v.v, db[x].where);
    });
    return ok(code_value(copy));
  }

  flow eval_node(node_id n, n_splice const & s, scope *, uint32_t) { fail(cat("#", str(s.name), " вне quote"), db[n].where); }

  flow eval_node(node_id n, n_try_compile const & t, scope * env, uint32_t frame) {
    auto saved_may = may_return;
    try {
      return eval(t.body, env, frame);
    } catch(diagnostic const & d) {
      if(d.kind != diag_kind::instantiation) throw;
      may_return = std::move(saved_may);
      emit({report_event::excluded, path, d.what(), {}, db[n].where, {}});
      return ok();
    }
  }

  flow eval_node(node_id n, n_lambda const & l, scope * env, uint32_t) {
    capture(env);
    return ok(value(fn_v{{}, n, env}));
  }

  flow eval_node(node_id n, n_construct const & c, scope * env, uint32_t frame) {
    std::vector<value> items;
    for(node_id item: c.items) {
      TZ_TRY(x, eval(item, env, frame));
      items.push_back(std::move(x));
    }
    auto tname = str(db.as<n_ident>(c.type).name);
    if(auto it = structs.find(db.as<n_ident>(c.type).name); it != structs.end()) { // Name {a, b} - by position n
      auto all = fields_of_struct(it->first);
      if(items.size() > all.size()) fail(cat(tname, ": полей ", all.size(), ", значений ", items.size()), db[n].where);
      agg_v g{types.named(tname), {}};
      for(std::size_t k = 0; k < all.size(); ++k) {
        auto const & f = db.as<n_var>(all[k]);
        if(k < items.size()) g.fields.push_back({f.name, std::move(items[k])});
        else if(f.init) {
          TZ_TRY(d, eval(f.init, globals, frame));
          g.fields.push_back({f.name, std::move(d)});
        } else fail(cat(tname, ": не задано поле ", str(f.name)), db[n].where);
        if(f.ct) pin(g.fields.back().second, f.name, db[n].where);
      }
      return ok(value(std::move(g)));
    }
    if(tname != "array") fail(cat("конструктор ", tname, "{} пока не поддержан"), db[n].where);
    for(auto const & x: items)
      if(!x.known()) return ok(erase(value(array_v::of({x})))); // At least one run-time element is an array of run-time
    return ok(value(array_v::of(std::move(items))));
  }

  // --- field access ---

  flow eval_node(node_id n, n_member const & m, scope * env, uint32_t frame) {
    value r;
    if(value const * o = peek(m.object, env); o && o->is<array_v>()) r = member(*o, m.name, db[n].where);
    else {
      TZ_TRY(obj, eval(m.object, env, frame));
      r = lift(obj, [&](value const & o) { return member(o, m.name, db[n].where); });
    }
    // xs.size - the size of this array (fact: range gives its indexes from it)
    if(checking() && str(m.name) == "size" && db.is<n_ident>(m.object))
      if(value const * cell = peek(m.object, env); cell && (cell->is<array_v>() || shape_of(*cell)))
        r.proof = add_fact({fact::size_of, cell, db.as<n_ident>(m.object).name, book.version(cell)});
    return ok(std::move(r));
  }

  value member(value const & obj, symbol name, span where) {
    if(obj.is<rt_v>()) {
      value const * shape = shape_of(obj);
      if(!shape) return value(rt_v{});
      return erase(member(*shape, name, where));
    }
    if(str(name) == "size") {
      if(obj.is<array_v>()) return value(int64_t(obj.as<array_v>().size()));
      if(obj.is<std::string>()) return value(int64_t(obj.as<std::string>().size()));
    }
    if(obj.is<agg_v>())
      for(auto & [k, v]: obj.as<agg_v>().fields)
        if(k == name) return v;
    fail(cat("у значения типа ", types.describe(obj), " нет поля ", str(name)), where);
  }

  // A variable cell without copying a value (an array by name is not copied for a
  // single element or size).
  value const * peek(node_id n, scope * env) {
    if(!db.is<n_ident>(n)) return nullptr;
    value const * c = env->find(db.as<n_ident>(n).name);
    if(c) not_hidden(*c, db.as<n_ident>(n).name, db[n].where);
    return c && !c->is<thunk_v>() ? c : nullptr;
  }

  // v.[f] is a field by descriptor (from fields(T)) or by line name; the descriptor is
  // known when compiled: the field cycle is disclosed (§7.4).
  flow eval_node(node_id n, n_field const & f, scope * env, uint32_t frame) {
    TZ_TRY(key, eval(f.key, env, frame));
    std::string name;
    if(key.is<std::string>()) name = key.as<std::string>();
    else if(key.is<agg_v>() && !key.as<agg_v>().fields.empty() && key.as<agg_v>().fields[0].second.is<std::string>())
      name = key.as<agg_v>().fields[0].second.as<std::string>();
    else fail(cat("v.[f]: дескриптор поля должен быть известен при компиляции, получено ", types.describe(key)), db[n].where);
    TZ_TRY(obj, eval(f.object, env, frame));
    return ok(lift(obj, [&](value const & o) { return member(o, sym(name), db[n].where); }));
  }

  flow eval_node(node_id n, n_index const & i, scope * env, uint32_t frame) {
    span where = db[n].where;
    if(value const * arr = peek(i.object, env); arr && arr->is<array_v>()) {
      TZ_TRY(idx, eval(i.index, env, frame));
      if(idx.known()) {
        value e = arr->as<array_v>().get(checked_index(*arr, idx, where));
        if(checking()) e.proof = index_result(i, *arr, idx, env);
        return ok(std::move(e));
      }
      return index_rt(n, i, *arr, idx, env);
    }
    TZ_TRY(obj, eval(i.object, env, frame));
    TZ_TRY(idx, eval(i.index, env, frame));
    return index_rt(n, i, obj, idx, env);
  }

  flow index_rt(node_id n, n_index const & i, value const & obj, value const & idx, scope * env) {
    span where = db[n].where;
    // index type - property of instance, not values: place of use instantiation error
    // (§13.4), including when array and index is run-time
    auto index_type = [&](value const & x) {
      if(std::string t = types.describe(x); t != "i64") fail(cat("индекс должен быть i64, получено ", t), where);
    };
    if(idx.is<split_v>())
      for(auto const & a: idx.as<split_v>().alts) index_type(a);
    else index_type(idx);
    bool proven_here = checking() && index_holds(i, idx, env);
    return ok(lift(obj, [&](value const & o) {
      if(o.is<rt_v>() && idx.known()) { // tuple by compile-time - the exact type of element
        value const * shape = shape_of(o);
        if(shape && shape->is<array_v>() && is_tuple(shape->as<array_v>()))
          return erase(shape->as<array_v>().get(checked_index(*shape, idx, where)));
      }
      if(o.is<rt_v>() || !idx.known()) {
        if(checking()) {
          note_index(n, proven_here);
          if(!proven_here) rt_check("out_of_bounds", where); // run-time-index not proven
        }
        value e = element_type(o, where);
        if(checking()) e.proof = index_result(i, o, idx, env);
        return e;
      }
      value e = o.as<array_v>().get(checked_index(o, idx, where));
      if(checking()) e.proof = index_result(i, o, idx, env);
      return e;
    }));
  }

  // An index is a safe index of this array (a fact on the value of the index).
  bool index_holds(n_index const & i, value const & idx, scope * env) const {
    fact const * f = fact_of(idx, fact::index_of);
    if(!f || !db.is<n_ident>(i.object)) return false;
    value const * cell = env->find(db.as<n_ident>(i.object).name);
    return cell && holds(*f, cell, db.as<n_ident>(i.object).name);
  }

  // The range(xs.size) element on the safe index is the safe xs index.
  uint32_t index_result(n_index const & i, value const & arr, value const & idx, scope * env) {
    fact const * r = fact_of(arr, fact::range_of);
    if(!r || !index_holds(i, idx, env)) return 0;
    return add_fact({fact::index_of, r->cell, r->name, r->version});
  }

  void note_index(node_id n, bool proven_here) {
    if(!proven_here) {
      emit({report_event::unproven, {}, {}, {}, {}, {}, n.value});
      return;
    }
    auto const & i = db.as<n_index>(n);
    std::string text = db.is<n_ident>(i.object) && db.is<n_ident>(i.index)
        ? cat(str(db.as<n_ident>(i.object).name), "[", str(db.as<n_ident>(i.index).name), "]", index_write == n ? " (запись)" : "")
        : std::string("индекс");
    emit({report_event::proven, text, {}, {}, db[n].where, {}, n.value});
  }

  // Type of element according to the run-time index. tuple - splitting by element
  // type: the sequel is understood for each.
  value element_type(value const & arr, span where) {
    if(unknown(arr)) return arr;
    value const * shape = shape_of(arr);
    if(!shape || !shape->is<array_v>()) fail(cat("индексация значения типа ", types.describe(arr)), where);
    auto const & a = shape->as<array_v>();
    if(a.empty()) return value(rt_v{});
    value t = erase(a.get(0));
    if(is_tuple(a))
      for(uint32_t i = 1; i < a.size(); ++i) t = merge(t, erase(a.get(i)));
    return t;
  }

  // Proven index of the array element: OutOfBounds (§12.1) - runtime - error.
  uint32_t checked_index(value const & arr, value const & idx, span where) {
    if(!arr.is<array_v>()) fail(cat("индексация значения типа ", types.describe(arr)), where);
    if(!idx.is<int64_t>()) fail("индекс должен быть i64", where);
    int64_t k = idx.as<int64_t>();
    uint32_t n = arr.as<array_v>().size();
    if(k < 0 || uint64_t(k) >= n) fail_rt("out_of_bounds", cat("индекс ", k, " вне границ массива размера ", n), where);
    return uint32_t(k);
  }

  flow eval_node(node_id n, n_hole const & h, scope *, uint32_t) {
    fail(cat("исполнение неразобранного фрагмента: ", h.reason), db[n].where);
  }

  flow eval_node(node_id n, auto const &, scope *, uint32_t) {
    fail(cat("узел ", kind_name(db[n]), " нельзя исполнить"), db[n].where);
  }

  // --- Operator chain: parsing by brute force (§4.4) ---
  //
  // 
//.
  // The tree of the chain chooses the types: all parsing in which each instance operator
  // (SFINAE) fits exactly one - it, otherwise an error. The same [[associative]] operator
  // in a row is one tree. parsing for instantiation: for free chain name types, with
  // cache. Types of operands and applications are considered by a separate interpreter in
  // parsing mode (sample): no side effects and no computation of operands (a & & f() does
  // not cause f).
  flow eval_node(node_id n, n_chain const & c, scope * env, uint32_t frame) {
    node_id tree = resolve_chain(n, c, env);
    if(!tree) return ok(value(rt_v{})); // The type of operand is still displayed (recursion).
    if(checking()) {
      auto [it, fresh] = chain_trees.try_emplace({n.value, ctx}, tree);
      if(!fresh && it->second != tree) it->second = node_id{}; // In this context, different trees
    }
    return eval(tree, env, frame);
  }

  // A chain tree in an embedding context (for machine code); no, not one.
  node_id chain_tree(node_id n, uint64_t at) const {
    auto it = chain_trees.find({n.value, at});
    return it == chain_trees.end() ? node_id{} : it->second;
  }

  void free_names(node_id n, std::vector<symbol> & out) const {
    if(!n) return;
    if(db.is<n_ident>(n)) {
      if(std::ranges::find(out, db.as<n_ident>(n).name) == out.end()) out.push_back(db.as<n_ident>(n).name);
      return;
    }
    if(db.is<n_lambda>(n) || db.is<n_fn>(n)) return;
    db.for_each_child(n, [&](node_id ch) { free_names(ch, out); });
  }

  node_id resolve_chain(node_id n, n_chain const & c, scope * env) {
    auto & names = chain_names[n.value];
    if(names.empty()) {
      for(node_id a: c.atoms) free_names(a, names);
      if(names.empty()) names.push_back(symbol{}); // literary
    }
    std::string key = cat(n.value, ":");
    bool cacheable = true;
    for(symbol s: names) {
      value const * cell = s ? env->find(s) : nullptr;
      if(!cell) {
        key += "-,";
        continue;
      }
      if(cell->is<thunk_v>()) cacheable = false; // type of expression of the caller - only a breakdown
      else if(unknown(*cell)) return {};
      else if(cell->is<array_v>() || cell->is<split_v>() || (cell->is<rt_v>() && types.of(*cell).value == 6)) key += types.describe(*cell) + ",";
      else key += cat(types.of(*cell).value, ",");
    }
    if(cacheable)
      if(auto it = resolved_chains.find(key); it != resolved_chains.end()) return it->second;
    node_id tree = enumerate_chain(n, c, env);
    if(tree && cacheable) resolved_chains[key] = tree;
    return tree;
  }

  interpreter & prober() {
    if(probes.empty()) {
      probes.emplace_back(db, mod, std::vector<std::string>{}, mode::check, std::nullopt, nullptr, this);
      probes.back().fuel = 200000;
    }
    return probes.back();
  }

  // Visible names - in one area of the sample, the values are erased to type;
  // expr-parameter - the expression of the calling in the erased copy of its region.
  scope * probe_scope(interpreter & p, scope * env, std::map<scope *, scope *> & seen) {
    if(auto it = seen.find(env); it != seen.end()) return it->second;
    scope * s = p.new_scope(p.globals);
    seen[env] = s;
    std::vector<symbol> declared;
    for(scope * e = env; e; e = e->parent)
      for(auto it = e->vars.rbegin(); it != e->vars.rend(); ++it) {
        if(std::ranges::find(declared, it->first) != declared.end()) continue;
        declared.push_back(it->first);
        value const & v = *it->second;
        if(v.is<thunk_v>()) {
          thunk_v t = v.as<thunk_v>();
          t.env = probe_scope(p, t.env, seen);
          t.frame = 0;
          s->declare(it->first, value(t));
        } else {
          value e = erase(v);
          e.proof = 0; // Facts - their own parsing
          s->declare(it->first, std::move(e));
        }
      }
    return s;
  }

  node_id enumerate_chain(node_id n, n_chain const & c, scope * env) {
    interpreter & p = prober();
    std::map<scope *, scope *> seen;
    scope * ps = probe_scope(p, env, seen);
    struct drop_all {
      interpreter & p;
      std::map<scope *, scope *> & seen;
      ~drop_all() {
        for(auto & [from, to]: seen) p.drop_scope(to);
      }
    } hold{p, seen};
    uint32_t k = uint32_t(c.atoms.size());
    std::vector<value> shapes;
    for(node_id a: c.atoms) {
      flow f = p.eval(a, ps, 0);
      if(!f.normal() || unknown(f.v)) return {}; // type still unknown
      shapes.push_back(f.v);
    }
    // dp[i][j] - parsing of atoms i..j: type, number of trees (1 or "many"), root
    struct entry {
      value shape;
      uint32_t count;
      int32_t op = -1; // operator number at the root; -1 - atom
      uint32_t li = 0, ri = 0; // parsing left and right
    };
    std::vector<std::vector<std::vector<entry>>> dp(k, std::vector<std::vector<entry>>(k));
    for(uint32_t i = 0; i < k; ++i) dp[i][i].push_back({shapes[i], 1});
    std::string why;
    for(uint32_t len = 2; len <= k; ++len)
      for(uint32_t i = 0; i + len - 1 < k; ++i) {
        uint32_t j = i + len - 1;
        for(uint32_t m = i; m < j; ++m) { // operator m - between atoms m and m + 1
          bool assoc = op_associative(c.ops[m]);
          for(uint32_t li = 0; li < dp[i][m].size(); ++li)
            for(uint32_t ri = 0; ri < dp[m + 1][j].size(); ++ri) {
              entry const & r = dp[m + 1][j][ri];
              if(assoc && r.op >= 0 && c.ops[uint32_t(r.op)] == c.ops[m]) continue; // a + (b + c) ≡ (a + b) + c
              auto res = probe_apply(p, c.ops[m], dp[i][m][li].shape, r.shape, why);
              if(!res) continue;
              uint32_t cnt = std::min(2u, dp[i][m][li].count * r.count);
              auto & cell = dp[i][j];
              auto same = std::ranges::find_if(cell, [&](entry const & e) { return types.describe(e.shape) == types.describe(*res); });
              if(same != cell.end()) same->count = 2; // other parsing of the same type
              else cell.push_back({*res, cnt, int32_t(m), li, ri});
            }
        }
      }
    auto const & top = dp[0][k - 1];
    uint32_t total = 0;
    for(auto const & e: top) total += e.count;
    if(total == 0) fail(cat("ни один разбор цепочки не инстанцируется", why.empty() ? "" : cat(": ", why)), db[n].where);
    if(total > 1) {
      diagnostic d("цепочка операторов неоднозначна: подходят несколько разборов — расставьте скобки", db[n].where);
      for(auto const & e: top) d.note(cat("разбор типа ", types.describe(e.shape)), db[n].where);
      throw d;
    }
    // the only tree in node (one per chain and types: cache above)
    auto build = [&](auto & self, uint32_t i, uint32_t j, uint32_t e) -> node_id {
      entry const & x = dp[i][j][e];
      if(x.op < 0) return c.atoms[i];
      uint32_t m = uint32_t(x.op);
      node_id l = self(self, i, m, x.li);
      node_id r = self(self, m + 1, j, x.ri);
      node_id t = db.add(n_op{c.ops[m], {l, r}}, {db[l].where.begin, db[r].where.end});
      db[t].parent = n;
      return t;
    };
    return build(build, 0, k - 1, 0);
  }

  bool op_associative(symbol op) {
    auto [it, fresh] = assoc_ops.try_emplace(op.value, false);
    if(fresh)
      if(auto f = functions.find(sym(cat("infix", str(op)))); f != functions.end())
        for(node_id fn: f->second)
          for(node_id a: db.as<n_fn>(fn).attrs)
            if(db.is<n_ident>(a) && str(db.as<n_ident>(a).name) == "associative") it->second = true;
    return it->second;
  }

  // Type of application of infix operator to samples (or failure - SFINAE).
  std::optional<value> probe_apply(interpreter & p, symbol op, value const & l, value const & r, std::string & why) {
    std::string key = cat(str(op), "|", types.describe(l), "|", types.describe(r));
    if(auto it = applied.find(key); it != applied.end()) return it->second;
    auto & node = apply_nodes[op.value];
    if(!node) {
      node_id a = db.add(n_ident{sym("__l")}, {});
      node_id b = db.add(n_ident{sym("__r")}, {});
      node = db.add(n_op{op, {a, b}}, {});
      db[a].parent = db[b].parent = node;
    }
    scope * s = p.new_scope(p.globals);
    scope_hold hold{p, s};
    value el = erase(l), er = erase(r);
    el.proof = er.proof = 0;
    s->declare(sym("__l"), std::move(el));
    s->declare(sym("__r"), std::move(er));
    std::optional<value> out;
    try {
      flow f = p.eval(node, s, 0);
      if(f.normal() && !unknown(f.v)) out = f.v;
    } catch(diagnostic const & d) {
      if(d.kind == diag_kind::runtime) out = value(rt_v{});
      else why = d.what();
    }
    applied[key] = out;
    return out;
  }

  // --- references to elements (§12.1, M2c) ---
  //
  //
//  
// The link is tied to a support - an array buffer. Operations,
  // Disabled support (append, [[invalidates(xs)]]), change its version; the link with the
  // old version is "dangerous-dangerous": in parsing - an error (in the way where
  // disabled), in execution - the exception of invalidated_alias. Recording in other
  // elements and values of elements - "dangerous-safe", allowed.

  // auto&e = v[i] / place-argument of the variable parameter: reference to the element.
  std::optional<value> element_ref(node_id place_node, scope * env, uint32_t frame, node_id born) {
    if(!db.is<n_index>(place_node) || !db.is<n_ident>(db.as<n_index>(place_node).object)) return std::nullopt;
    auto const & i = db.as<n_index>(place_node);
    value * cell = env->find(db.as<n_ident>(i.object).name);
    if(!cell || cell->is<thunk_v>() || cell->is<ref_v>()) return std::nullopt;
    not_hidden(*cell, db.as<n_ident>(i.object).name, db[place_node].where);
    flow fi = eval(i.index, env, frame);
    if(!fi.normal()) return std::nullopt;
    value idx = fi.v;
    span where = db[place_node].where;
    ref_v r;
    r.cell = cell;
    r.born = born.value + 1;
    if(!checking()) {
      if(!cell->is<array_v>()) return std::nullopt;
      r.index = checked_index(*cell, idx, where);
      r.gen = cell->as<array_v>().gen;
      return value(r);
    }
    if(!cell->is<array_v>() && !(shape_of(*cell) && shape_of(*cell)->is<array_v>())) return std::nullopt;
    if(cell->is<array_v>() && idx.known()) r.index = checked_index(*cell, idx, where);
    else {
      r.rt_index = true;
      bool proven_here = index_holds(i, idx, env);
      note_index(place_node, proven_here);
      if(!proven_here) rt_check("out_of_bounds", where);
    }
    r.gen = book.version(cell);
    return value(r);
  }

  flow declare_ref(node_id n, n_var const & v, scope * env, uint32_t frame) {
    if(db.is<n_ident>(v.init)) { // auto&a = b is the second name of the same cell.
      value * cell = env->find(db.as<n_ident>(v.init).name);
      if(!cell || cell->is<thunk_v>()) fail("auto& — ссылка на переменную или элемент массива", db[v.init].where);
      not_hidden(*cell, db.as<n_ident>(v.init).name, db[v.init].where);
      env->alias(v.name, cell);
      return ok();
    }
    auto r = element_ref(v.init, env, frame, n);
    if(!r) fail("auto& — ссылка на переменную или элемент массива переменной", db[v.init].where);
    env->declare(v.name, std::move(*r));
    return ok();
  }

  void ref_valid(ref_v const & r, span where) {
    if(checking()) {
      if(book.version(r.cell) == r.gen) return;
      diagnostic d("ссылка на элемент использована после инвалидации опоры (опасное-опасное)", where);
      d.note("ссылка взята здесь", db[node_id(r.born - 1)].where);
      if(auto it = book.changed_at.find(r.cell); it != book.changed_at.end() && it->second) d.note("опора инвалидирована здесь", db[node_id(it->second - 1)].where);
      if(!path.empty()) d.note(cat("в продолжении ", path), where);
      for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
      throw d;
    }
    if(!r.cell->is<array_v>() || r.cell->as<array_v>().gen != r.gen)
      fail_rt("invalidated_alias", "ссылка на элемент использована после инвалидации опоры", where);
  }

  value deref(ref_v const & r, span where) {
    ref_valid(r, where);
    value const & a = *r.cell;
    if(!r.rt_index && a.is<array_v>()) return a.as<array_v>().get(checked_index(a, value(r.index), where));
    return element_type(a, where);
  }

  flow ref_assign(ref_v const & r, std::string_view op, value const & rhs, span where) {
    ref_valid(r, where);
    value cur = deref(r, where);
    value res = op == "=" ? rhs : binary(op.substr(0, 1), cur, rhs, where);
    if(!r.rt_index && r.cell->is<array_v>()) {
      if(!checking() || (res.known() && cur.known())) {
        r.cell->as<array_v>().set(uint32_t(r.index), res);
        return ok();
      }
    }
    if(!unknown(cur) && !unknown(res) && types.describe(erase(cur)) != types.describe(erase(res)))
      fail(cat("элемент массива типа ", types.describe(cur), " нельзя заменить значением типа ", types.describe(res)), where);
    if(r.cell->is<array_v>()) *r.cell = erase(*r.cell); // element on run-time -index changed - array run-time
    return ok();
  }

  // append(xs, x) is the growth of the array, the core push; the support xs is disabled.
  flow append(node_list const & args, scope * env, uint32_t frame, node_id n) {
    span where = db[n].where;
    if(args.size() != 2 || !db.is<n_ident>(args[0])) fail("append(массив, значение)", where);
    value * cell = env->find(db.as<n_ident>(args[0]).name);
    if(!cell || cell->is<thunk_v>() || cell->is<ref_v>()) fail("append: первый аргумент — переменная-массив", where);
    not_hidden(*cell, db.as<n_ident>(args[0]).name, where);
    TZ_TRY(x, eval(args[1], env, frame));
    if(!checking()) {
      if(!cell->is<array_v>()) fail(cat("append: ожидался массив, получено ", types.describe(*cell)), where);
      cell->as<array_v>().push(x);
      cell->as<array_v>().gen = ++array_gen;
      return ok();
    }
    invalidate(cell, n);
    value const * shape = shape_of(*cell);
    if(!shape || !shape->is<array_v>()) fail(cat("append: ожидался массив, получено ", types.describe(*cell)), where);
    if(!shape->as<array_v>().empty() && !unknown(x) && types.describe(erase(shape->as<array_v>().get(0))) != types.describe(erase(x)))
      fail(cat("append: элемент типа ", types.describe(x), " в массив ", types.describe(*cell)), where);
    if(cell->is<array_v>() && x.known()) cell->as<array_v>().push(x);
    else *cell = erase(value(array_v::of({shape->as<array_v>().empty() ? x : shape->as<array_v>().get(0)})));
    return ok();
  }

  // The place of disability for the track is in the program code, not inside prelude.
  void invalidate(value * cell, node_id at) {
    bool lib = db.prelude && at && db[at].where.source == db[db.prelude].where.source;
    uint32_t keep = book.changed_at[cell];
    book.invalidated(cell, at);
    if(lib && keep) book.changed_at[cell] = keep;
  }

  // [[invalidates(p)]] - An operation declares that it disables the support of the
  // parameter p (semantics is an attribute of the library, not a compiler rule).
  void declared_invalidations(node_id f, node_list const & args, scope * env, uint32_t site) {
    if(!checking()) return;
    auto const & fn = db.as<n_fn>(f);
    for(node_id a: fn.attrs) {
      if(!db.is<n_call>(a) || !db.is<n_ident>(db.as<n_call>(a).callee) || str(db.as<n_ident>(db.as<n_call>(a).callee).name) != "invalidates") continue;
      for(node_id pn: db.as<n_call>(a).args) {
        if(!db.is<n_ident>(pn)) continue;
        for(uint32_t k = 0; k < fn.params.size() && k < args.size(); ++k)
          if(db.as<n_param>(fn.params[k]).name == db.as<n_ident>(pn).name && db.is<n_ident>(args[k]))
            if(value * cell = env->find(db.as<n_ident>(args[k]).name); cell && !cell->is<thunk_v>())
              invalidate(cell->is<ref_v>() ? cell->as<ref_v>().cell : cell, node_id(site));
      }
    }
  }

  // --- fiber and streams (§8.3, M3a) ---
  //
  // 
// ; Fiber.
  // coroutine stack: The interpreter and machine code run on its stack as usual; the
  // blocking operation parks the fiber and gives the kernel to another. Exchange -
  // threads with blocking write/read:
// parsing spawn mode
  // It's sorted out on the spot, flows are just types. The scheduler, threads, and
  // expectation of host are at the root of VM: all processes (M3c) common to fiber.

  void swap_exec(exec_state & e) {
    std::swap(frames, e.frames);
    std::swap(stack, e.stack);
    std::swap(throw_stack, e.throw_stack);
    std::swap(frame_floors, e.frame_floors);
    std::swap(throw_sinks, e.throw_sinks);
    std::swap(overrides, e.overrides);
    std::swap(override_depth, e.override_depth);
    std::swap(ctx, e.ctx);
    std::swap(path, e.path);
  }

  // fiber is now executable (in any VM process); the main root is the host stack.
  fiber_rec & running() {
    if(!root->running_fiber) {
      root->fibers.emplace_back();
      root->fibers.back().vm = root;
      root->running_fiber = &root->fibers.back();
    }
    return *root->running_fiber;
  }

  fiber_rec & new_fiber() {
    running();
    fiber_rec & f = fibers.emplace_back();
    f.id = uint32_t(fibers.size() - 1);
    f.vm = this;
    f.stack.emplace(std::size_t(64) << 20);
    f.rsp = f.stack->start(&fiber_main, &f);
    root->ready.push_back(&f);
    return f;
  }

  // spawn(expr body) is the new fiber; the body is closed (the area lives beyond the call).
  flow spawn(node_list const & args, scope * env, uint32_t frame, span where) {
    if(args.size() != 1) fail("spawn(тело)", where);
    thunk_v t = thunk_arg(args[0], env, where);
    if(checking()) { // parsing: the body is in place; fiber launch - runtime effect
      rt_effect();
      uint64_t saved = std::exchange(ctx, t.ctx); // as in performance: the body in the context where it is written
      flow r;
      try {
        r = eval(t.expr, t.env, t.frame);
      } catch(...) {
        ctx = saved;
        throw;
      }
      ctx = saved;
      (void)r;
      return ok();
    }
    capture(t.env);
    fiber_rec & f = new_fiber();
    f.body = t.expr;
    f.env = t.env;
    f.frame = t.frame;
    f.body_ctx = t.ctx;
    return ok();
  }

  static void fiber_main(void * arg) {
    auto & f = *static_cast<fiber_rec *>(arg);
    interpreter & vm = *f.vm;
    // boundary of the host stack - only from the first run (it makes the main
    // stack); fiber running another fiber would get the boundaries of that - and
    // going to the main stack would inform ASan of other boundary
    void const * from_bottom = nullptr;
    std::size_t from_size = 0;
    fiber_started(&from_bottom, &from_size);
    if(!vm.root->host_bottom) {
      vm.root->host_bottom = from_bottom;
      vm.root->host_size = from_size;
    }
    try {
      if(f.entry) vm.process_main(f); // input
      else {
        vm.ctx = f.body_ctx;
        flow r = vm.eval(f.body, f.env, f.frame);
        if(r.kind == flow_kind::thr) vm.uncaught(r);
      }
    } catch(fiber_cancel const &) {
    } catch(diagnostic const & d) {
      f.error = d; // uncaught completes fiber with the track; the rest work
    } catch(...) {
      f.error = diagnostic("внутренняя ошибка в файбере", {});
    }
    f.state = fiber_rec::done;
    if(f.entry) vm.process_done(f);
    for(fiber_rec * w: std::exchange(vm.root->joiners, {})) vm.wake(w);
    vm.schedule(); // They don't come back
  }

  void wake(fiber_rec * f) {
    if(f->state != fiber_rec::blocked) return;
    f->state = fiber_rec::ready;
    root->ready.push_back(f);
  }

  // Direct transmission: awakened by exchange through the stream is executed as
  // follows - its stack and data are still hot
// * My: * no more handoff_limit in a row
  // Otherwise, the fiber pair will take away the core from the rest forever. The car, too.
  static constexpr uint32_t handoff_limit = 128;
  void wake_direct(fiber_rec * f) {
    if(f->state != fiber_rec::blocked) return;
    f->state = fiber_rec::ready;
    if(root->handoff_run >= handoff_limit) return root->ready.push_back(f);
    f->direct = true;
    root->ready.push_front(f);
  }

  // Give the kernel to the next finished fiber. "" There are no ready ones, the current
  // one is locked, no one will wake it up: mutual lock.
  void schedule() {
    interpreter & r = *root;
    fiber_rec & me = running();
    auto drop_stale = [&] {
      while(!r.ready.empty() && r.ready.front()->state != fiber_rec::ready) r.ready.pop_front();
    };
    drop_stale();
    while(r.ready.empty() && r.io_pending > 0 && me.state != fiber_rec::ready) { // We are all waiting for host: sleep in io_uring
      for(auto const & c: r.ring->wait()) io_complete(c);
      drop_stale();
    }
    if(r.ready.empty()) {
      if(me.state == fiber_rec::ready) return;
      // No One Wakes Up: The Error Gets the Basic fiber
      fiber_rec & main_f = r.fibers[0];
      main_f.pending = diagnostic("все файберы ждут друг друга (взаимная блокировка)", {}, diag_kind::runtime);
      main_f.state = fiber_rec::ready;
      if(&me == &main_f) {
        auto d = *std::exchange(main_f.pending, std::nullopt);
        fail_rt("deadlock", d.what(), stack.empty() ? span{} : stack.back().where);
      }
      r.ready.push_back(&main_f);
    }
    fiber_rec & next = *r.ready.front();
    r.ready.pop_front();
    r.handoff_run = next.direct ? r.handoff_run + 1 : 0;
    next.direct = false;
    if(&next == &me) return;
    me.vm->swap_exec(me.exec);
    next.vm->swap_exec(next.exec);
    r.running_fiber = &next;
    void const * bottom = next.stack ? next.stack->base : r.host_bottom;
    std::size_t size = next.stack ? next.stack->size : r.host_size;
    switch_to(&me.rsp, next.rsp, bottom, size);
    // They come back here when someone switched to me.
  }

  struct fiber_cancel {}; // No exception to the language: catch the program does not see it

  void park() {
    running().state = fiber_rec::blocked;
    schedule();
    if(running().cancel) throw fiber_cancel{};
    if(auto & p = running().pending) {
      std::string what = p->what();
      p.reset();
      fail_rt("deadlock", what, stack.empty() ? span{} : stack.back().where);
    }
  }

  void yield() {
    root->ready.push_back(&running());
    schedule();
  }

  void each_fiber(auto && f) {
    for(auto & x: root->fibers) f(x);
    for(auto & p: root->procs)
      for(auto & x: p.fibers) f(x);
  }

  // fiber, left to wait (the program ended in error), are unwinded: their areas and
  // values are released as in the usual output.
  void cancel_fibers() {
    if(root != this || root->fibers.empty() || root->running_fiber != &root->fibers[0]) return;
    each_fiber([&](fiber_rec & f) {
      if(f.state == fiber_rec::done || !f.stack) return;
      f.cancel = true;
      f.state = fiber_rec::ready;
      ready.push_front(&f);
      fibers[0].state = fiber_rec::ready;
      ready.push_back(&fibers[0]);
      schedule();
    });
  }

  // The end of the main: VM awaits fiber and processes; the program's fiber error is its error
  // 
//.
  void join_all() {
    if(root != this || fibers.size() <= 1) return;
    for(;;) {
      bool alive = false;
      each_fiber([&](fiber_rec & f) {
        if(&f != &fibers[0] && f.state != fiber_rec::done) alive = true;
      });
      if(!alive) break;
      joiners.push_back(&fibers[0]);
      park();
    }
    for(auto & f: fibers)
      if(f.error) throw *f.error;
  }

  // --- VM processes (M3c) ---
  //
  // 
// ; The process is its own.
  // Interpreter: its globals, domains, and fiber; common - code base, type registry,
  // and scheduler Between processes, only data is moved (without alias, circuits and
  // serialization, §9.2).

  bool transferable(value const & v) const {
    if(v.is<thunk_v>() || v.is<ref_v>() || v.is<split_v>()) return false;
    if(v.is<fn_v>()) return v.as<fn_v>().name && !v.as<fn_v>().env; // entity - function - yes, closure - no
    if(v.is<array_v>()) {
      auto const & a = v.as<array_v>();
      for(uint32_t i = 0; i < a.size(); ++i)
        if(!transferable(a.get(i))) return false;
    }
    if(v.is<agg_v>())
      for(auto const & [k, f]: v.as<agg_v>().fields)
        if(!transferable(f)) return false;
    return true;
  }

  flow start_process(node_list const & args, scope * env, uint32_t frame, span where) {
    if(args.empty() || !db.is<n_ident>(args[0])) fail("process(функция, аргументы…)", where);
    symbol fname = db.as<n_ident>(args[0]).name;
    if(!functions.contains(fname)) fail(cat("process: нет функции ", str(fname)), where);
    node_list rest(args.begin() + 1, args.end());
    if(checking()) { // parsing: Process input is treated as a call
      rt_effect();
      flow r = call_named(fname, rest, env, frame, where);
      if(r.kind == flow_kind::thr) record_throw(r.v);
      return ok(value(agg_v{types.named("process"), {{sym("id"), erase(value(int64_t(0)))}}}));
    }
    std::vector<value> vals;
    for(node_id a: rest) {
      TZ_TRY(v, eval(a, env, frame));
      if(!transferable(v)) fail(cat("process: аргумент типа ", types.describe(v), " не передаётся между процессами (только данные)"), db[a].where);
      vals.push_back(std::move(v));
    }
    interpreter & child = root->procs.emplace_back(db, mod, std::vector<std::string>{"process"}, mode::run, std::nullopt, nullptr, root);
    child.jit = jit;
    child.captured = captured;
    auto & meta = root->proc_meta.emplace_back();
    meta.vm = &child;
    fiber_rec & f = child.new_fiber();
    f.entry = fname;
    f.entry_args = std::move(vals);
    f.proc = uint32_t(root->proc_meta.size() - 1);
    return ok(value(agg_v{types.named("process"), {{sym("id"), value(int64_t(f.proc))}}}));
  }

  void process_main(fiber_rec & f) {
    elaborate();
    auto it = functions.find(f.entry);
    call_site site{{}, std::string(str(f.entry)), 0, 0};
    flow r = dispatch_split(it->second, f.entry_args, 0, {}, globals, 0, site);
    if(r.kind == flow_kind::thr) uncaught(r);
  }

  void process_done(fiber_rec & f) {
    auto & m = root->proc_meta[f.proc];
    m.done = true;
    if(f.error) {
      m.failed = true;
      m.error = f.error->what();
      f.error.reset(); // Process outcome, not program error
    }
    for(fiber_rec * w: std::exchange(m.waiters, {})) wake(w);
  }

  proc_meta_rec & proc_of(value const & p, span where) {
    value const * v = shape_of(p);
    if(!v || !v->is<agg_v>() || types.name(v->as<agg_v>().type) != "process") fail("ожидался процесс", where);
    value const & id = v->as<agg_v>().fields[0].second;
    if(!id.is<int64_t>() || uint64_t(id.as<int64_t>()) >= root->proc_meta.size()) fail("неизвестный процесс", where);
    return root->proc_meta[std::size_t(id.as<int64_t>())];
  }

  value process_op(std::string_view n, std::vector<value> & a, span where) {
    if(checking()) {
      rt_effect();
      return n == "process_wait" ? erase(value(true)) : erase(value(std::string("x")));
    }
    auto & m = proc_of(a[0], where);
    while(!m.done) {
      m.waiters.push_back(&running());
      park();
    }
    if(n == "process_wait") return value(!m.failed);
    return value(m.error);
  }

  // --- host : io_uring (M3b) ---
  //
  //
// 
// 
// 
// The net.
  // one event stream{peer, kind, data} per process listener; sending - send(net, peer,
  // data) without waiting; files are streams of pieces. Applications - io uring, the
  // scheduler waits for completions when there are no ready-made filters.

  enum io_op : uint64_t { op_accept = 1,
    op_recv,
    op_send,
    op_connect,
    op_fread,
    op_fwrite };
  static uint64_t io_tag(io_op op, std::size_t stream, std::size_t peer) { return (uint64_t(op) << 56) | (uint64_t(stream) << 24) | uint64_t(peer); }

  os::uring & io() {
    auto & r = *root;
    if(!r.ring) r.ring.emplace();
    if(!r.ring->ok) fail_rt("io_error", "io_uring недоступен в этой системе", {});
    return *r.ring;
  }

  value make_event(int64_t peer, std::string_view kind, std::string data) {
    return value(agg_v{types.named("event"), {{sym("peer"), value(peer)}, {sym("kind"), value(std::string(kind))}, {sym("data"), value(std::move(data))}}});
  }

  void deliver(stream_rec & s, value v) {
    s.buf.push_back(std::move(v));
    if(!s.readers.empty()) {
      wake(s.readers.front());
      s.readers.pop_front();
    }
  }

  void peer_closed(stream_rec & s, std::size_t p, std::string why = {}) {
    auto & pr = s.peers[p];
    if(!pr.open) return;
    pr.open = false;
    if(pr.fd >= 0) {
      ::close(pr.fd);
      pr.fd = -1;
    }
    if(!s.closed) deliver(s, make_event(int64_t(p), "close", std::move(why)));
  }

  void send_next(std::size_t sid, std::size_t p) {
    auto & pr = root->streams[sid].peers[p];
    if(pr.sending || pr.out.empty() || pr.fd < 0) return;
    pr.sending = true;
    ++root->io_pending;
    io().send(pr.fd, pr.out.front().data() + pr.sent, pr.out.front().size() - pr.sent, io_tag(op_send, sid, p));
  }

  void arm_recv(std::size_t sid, std::size_t p) {
    auto & pr = root->streams[sid].peers[p];
    if(pr.fd < 0) return;
    ++root->io_pending;
    io().recv_all(pr.fd, io_tag(op_recv, sid, p));
  }

  // The completion of the kernel application is an event in the stream, the awakening of those waiting.
  void io_complete(os::uring::completion const & c) {
    if(!c.tag) return; // cancellation
    auto op = io_op(c.tag >> 56);
    std::size_t sid = (c.tag >> 24) & 0xFFFFFFFF, p = c.tag & 0xFFFFFF;
    bool more = c.flags & IORING_CQE_F_MORE;
    if(!more) --root->io_pending;
    stream_rec & s = root->streams[sid];
    switch(op) {
      case op_accept:
        if(c.res >= 0 && !s.closed) {
          s.peers.push_back({c.res});
          deliver(s, make_event(int64_t(s.peers.size() - 1), "open", {}));
          arm_recv(sid, s.peers.size() - 1);
        } else if(c.res >= 0) ::close(c.res);
        if(!more && !s.closed && s.listen_fd >= 0) {
          ++root->io_pending;
          io().accept_all(s.listen_fd, io_tag(op_accept, sid, 0));
        }
        break;
      case op_recv: {
        auto & pr = s.peers[p];
        if(c.res > 0) {
          unsigned bid = c.flags >> IORING_CQE_BUFFER_SHIFT;
          std::string data(root->ring->buffer(bid), std::size_t(c.res));
          root->ring->give_back(bid);
          if(pr.open && !s.closed) deliver(s, make_event(int64_t(p), "data", std::move(data)));
          if(!more && pr.open) arm_recv(sid, p);
        } else if(c.res == -ENOBUFS) {
          if(!more && pr.open) arm_recv(sid, p);
        } else if(!more) peer_closed(s, p, c.res < 0 ? std::string(std::strerror(-c.res)) : std::string());
        break;
      }
      case op_send: {
        auto & pr = s.peers[p];
        pr.sending = false;
        if(c.res < 0) {
          peer_closed(s, p, std::strerror(-c.res));
          break;
        }
        pr.sent += std::size_t(c.res);
        if(pr.sent >= pr.out.front().size()) {
          pr.out.pop_front();
          pr.sent = 0;
        }
        send_next(sid, p);
        break;
      }
      case op_connect:
        if(c.res < 0) {
          s.peers[0].open = false;
          ::close(s.peers[0].fd);
          s.peers[0].fd = -1;
          deliver(s, make_event(0, "close", std::strerror(-c.res)));
          break;
        }
        s.connected = true;
        deliver(s, make_event(0, "open", {}));
        arm_recv(sid, 0);
        send_next(sid, 0);
        break;
      case op_fread:
        s.io_busy = false;
        if(c.res > 0) {
          s.offset += uint64_t(c.res);
          deliver(s, value(std::string(s.io_buf.data(), std::size_t(c.res))));
        } else {
          s.closed = true; // end of file (or reading error)
          for(fiber_rec * r: std::exchange(s.readers, {})) wake(r);
        }
        break;
      case op_fwrite:
        s.io_busy = false;
        if(c.res > 0) {
          s.offset += uint64_t(c.res);
          s.peers[0].sent += std::size_t(c.res);
          if(s.peers[0].sent >= s.peers[0].out.front().size()) {
            s.peers[0].out.pop_front();
            s.peers[0].sent = 0;
          }
        } else if(!s.peers[0].out.empty()) s.peers[0].out.clear(); // record
        file_write_next(sid);
        break;
    }
  }

  void file_write_next(std::size_t sid) {
    stream_rec & s = root->streams[sid];
    auto & q = s.peers[0];
    if(s.io_busy || q.out.empty()) {
      if(q.out.empty())
        for(fiber_rec * w: std::exchange(s.writers, {})) wake(w);
      return;
    }
    s.io_busy = true;
    ++root->io_pending;
    io().write(s.file_fd, q.out.front().data() + q.sent, q.out.front().size() - q.sent, s.offset, io_tag(op_fwrite, sid, 0));
  }

  uint32_t stream_id(value const & s, span where) {
    value const * v = shape_of(s);
    if(!v || !v->is<agg_v>() || types.name(v->as<agg_v>().type) != "stream" || v->as<agg_v>().fields.empty()) fail("ожидался поток", where);
    value const & id = v->as<agg_v>().fields[0].second;
    if(!id.is<int64_t>() || id.as<int64_t>() < 0 || uint64_t(id.as<int64_t>()) >= root->streams.size()) fail("неизвестный поток", where);
    return uint32_t(id.as<int64_t>());
  }

  // parsing: The value of the flow carries the type of elements, if it is known at creation
  // - the fusion (cycle, branches) erases the number, but not the type.
  value stream_value(std::size_t id) {
    agg_v a{types.named("stream"), {{sym("id"), value(int64_t(id))}}};
    if(checking() && id < root->streams.size() && root->streams[id].typed) a.fields.push_back({sym("elem"), erase(root->streams[id].shape)});
    return value(std::move(a));
  }

  // Network and file operations (net *, file *): in parsing, only types.
  value io_op_call(std::string_view n, std::vector<value> & a, span where) {
    auto & ss = root->streams;
    if(checking()) {
      rt_effect();
      rt_check("io_error", where);
      if(n == "net_listen" || n == "net_connect" || n == "file_open" || n == "file_create") {
        ss.emplace_back();
        ss.back().typed = true;
        ss.back().shape = n.starts_with("net_") ? erase(make_event(0, "x", "x")) : erase(value(std::string("x")));
        return stream_value(ss.size() - 1);
      }
      if(n == "net_port") return erase(value(int64_t(1)));
      return value();
    }
    if(n == "net_listen") {
      if(a.size() != 1 || !a[0].is<int64_t>()) fail("listen(порт)", where);
      int fd = os::listen_tcp(int(a[0].as<int64_t>()));
      if(fd < 0) fail_rt("io_error", cat("listen: ", std::strerror(errno)), where);
      ss.emplace_back();
      ss.back().listen_fd = fd;
      ss.back().net = true;
      ++root->io_pending;
      io().accept_all(fd, io_tag(op_accept, ss.size() - 1, 0));
      return stream_value(ss.size() - 1);
    }
    if(n == "net_connect") {
      if(a.size() != 2 || !a[0].is<std::string>() || !a[1].is<int64_t>()) fail("connect(адрес, порт)", where);
      ss.emplace_back();
      stream_rec & s = ss.back();
      s.net = true;
      if(!os::ipv4(a[0].as<std::string>(), int(a[1].as<int64_t>()), s.addr)) fail_rt("io_error", "connect: неверный адрес", where);
      int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if(fd < 0) fail_rt("io_error", cat("connect: ", std::strerror(errno)), where);
      s.peers.push_back({fd});
      ++root->io_pending;
      io().connect(fd, &s.addr, io_tag(op_connect, ss.size() - 1, 0));
      return stream_value(ss.size() - 1);
    }
    if(n == "file_open" || n == "file_create") {
      if(a.size() != 1 || !a[0].is<std::string>()) fail(cat(n == "file_open" ? "open" : "create", "(путь)"), where);
      int fd = n == "file_open" ? ::open(a[0].as<std::string>().c_str(), O_RDONLY | O_CLOEXEC)
                                : ::open(a[0].as<std::string>().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
      if(fd < 0) fail_rt("io_error", cat(a[0].as<std::string>(), ": ", std::strerror(errno)), where);
      ss.emplace_back();
      ss.back().file_fd = fd;
      ss.back().file_write = n == "file_create";
      ss.back().peers.push_back({});
      return stream_value(ss.size() - 1);
    }
    std::size_t sid = stream_id(a[0], where);
    stream_rec & s = ss[sid];
    if(n == "net_port") return value(int64_t(s.listen_fd >= 0 ? os::bound_port(s.listen_fd) : 0));
    if(n == "net_send") { // In the outgoing stream: does not wait (the core will send)
      if(a.size() != 3 || !a[1].is<int64_t>() || !a[2].is<std::string>()) fail("send(сеть, peer, байты)", where);
      auto p = std::size_t(a[1].as<int64_t>());
      if(!s.net || p >= s.peers.size()) fail_rt("io_error", cat("send: нет соединения ", p), where);
      if(!s.peers[p].open) return value(false);
      s.peers[p].out.push_back(std::move(a[2].as<std::string>()));
      if(s.connected || s.listen_fd >= 0) send_next(sid, p);
      return value(true);
    }
    if(n == "net_drop") {
      auto p = std::size_t(a[1].as<int64_t>());
      if(p < s.peers.size() && s.peers[p].fd >= 0) {
        io().cancel_fd(s.peers[p].fd);
        s.peers[p].open = false;
        ::close(s.peers[p].fd);
        s.peers[p].fd = -1;
      }
      return value();
    }
    fail(cat("неизвестная операция ", n), where);
  }

  // Flow host: Closing cancels orders; file adds queue.
  void close_host(std::size_t sid) {
    stream_rec & s = root->streams[sid];
    if(s.file_fd >= 0 && s.file_write) {
      while(!s.peers[0].out.empty()) {
        s.writers.push_back(&running());
        park();
      }
    }
    if(s.listen_fd >= 0) {
      io().cancel_fd(s.listen_fd);
      ::close(s.listen_fd);
      s.listen_fd = -1;
    }
    for(auto & pr: s.peers)
      if(pr.fd >= 0) {
        io().cancel_fd(pr.fd);
        ::close(pr.fd);
        pr.fd = -1;
        pr.open = false;
      }
    if(s.file_fd >= 0) {
      ::close(s.file_fd);
      s.file_fd = -1;
    }
  }

  value stream_op(std::string_view n, std::vector<value> & a, span where) {
    if(n == "stream_new") {
      if(checking()) rt_effect(); // Flow - Execution Resource: Creation does not fold into compile-time
      auto & ss = root->streams;
      ss.emplace_back();
      if(a.size() == 1 && a[0].is<int64_t>() && a[0].as<int64_t>() > 0) ss.back().cap = std::size_t(a[0].as<int64_t>());
      ss.back().born_ctx = ctx;
      ss.back().born_at = (uint64_t(where.source) << 32) | where.begin;
      // Element type - from parsing of the entire program: machine code reads a stream of its type
      if(auto it = stream_shapes.find({ctx, ss.back().born_at}); it != stream_shapes.end()) {
        ss.back().shape = it->second;
        ss.back().typed = true;
      }
      return stream_value(ss.size() - 1);
    }
    std::size_t sid = stream_id(a[0], where);
    stream_rec & s = root->streams[sid];
    if(checking()) { // parsing: type only; flow operation - runtime effect
      rt_effect();
      if(n == "stream_write") {
        value e = erase(a[1]);
        if(s.typed && !unknown(s.shape) && types.describe(s.shape) != types.describe(e)) fail(cat("поток значений ", types.describe(s.shape), ": запись ", types.describe(e)), where);
        s.shape = e;
        s.typed = true;
        return value();
      }
      if(n == "stream_read") {
        rt_check("closed_stream", where);
        return s.typed ? s.shape : value(rt_v{});
      }
      if(n == "stream_more") return erase(value(true));
      return value();
    }
    value none;
    uint8_t op = n == "stream_write" ? s_write : n == "stream_read" ? s_read : n == "stream_more" ? s_more : n == "stream_close" ? s_close : s_bad;
    if(op == s_bad) fail(cat("неизвестная операция потока ", n), where);
    return stream_do(op, sid, a.size() > 1 ? a[1] : none, where);
  }

  // Execution flow operation by number: without value-flow and without argument
  // vector - input for machine code. x is recorded (s_write).
  enum : uint8_t { s_write, s_read, s_more, s_close, s_bad };
  value stream_do(uint8_t op, std::size_t sid, value & x, span where) {
    stream_rec & s = root->streams[sid];
    fiber_rec * me = &running();
    if(op == s_write) {
      if(s.net) fail("в сеть — send(сеть, peer, байты)", where);
      if(s.file_fd >= 0) {
        if(!s.file_write) fail("файл открыт на чтение", where);
        if(!x.is<std::string>()) fail(cat("в файл — байты (str), получено ", types.describe(x)), where);
        s.peers[0].out.push_back(std::move(x.as<std::string>()));
        file_write_next(sid);
        return value();
      }
      if(!transferable(x)) fail(cat("в поток — только данные: ", types.describe(x)), where);
      while(s.buf.size() >= s.cap && !s.closed) {
        s.writers.push_back(me);
        park();
      }
      if(s.closed) fail_rt("closed_stream", "запись в закрытый поток", where);
      s.buf.push_back(std::move(x));
      if(!s.readers.empty()) {
        wake_direct(s.readers.front());
        s.readers.pop_front();
      }
      return value();
    }
    if(op == s_read || op == s_more) {
      while(s.buf.empty() && !s.closed) {
        if(s.file_fd >= 0 && !s.file_write && !s.io_busy) { // File: The next piece is the application
          s.io_busy = true;
          s.io_buf.resize(65536);
          ++root->io_pending;
          io().read(s.file_fd, s.io_buf.data(), s.io_buf.size(), s.offset, io_tag(op_fread, sid, 0));
        }
        s.readers.push_back(me);
        park();
      }
      if(op == s_more) return value(!s.buf.empty());
      if(s.buf.empty()) fail_rt("closed_stream", "чтение из закрытого пустого потока", where);
      value v = std::move(s.buf.front());
      s.buf.pop_front();
      if(!s.writers.empty()) {
        wake_direct(s.writers.front());
        s.writers.pop_front();
      }
      return v;
    }
    if(op == s_close) {
      close_host(sid);
      s.closed = true;
      for(fiber_rec * r: std::exchange(s.readers, {})) wake(r);
      for(fiber_rec * w: std::exchange(s.writers, {})) wake(w);
      return value();
    }
    fail("неизвестная операция потока", where);
  }

  // --- seats (lvalue) ---

  value * place(node_id n, scope * env, uint32_t frame) {
    if(db.is<n_ident>(n)) {
      if(value * c = env->find(db.as<n_ident>(n).name)) return c;
      fail(cat("неизвестная переменная ", str(db.as<n_ident>(n).name)), db[n].where);
    }
    if(db.is<n_index>(n)) { // cell of the element is only in a common array
      auto const & i = db.as<n_index>(n);
      value * arr = place(i.object, env, frame);
      flow idx = eval(i.index, env, frame);
      if(!arr || arr->is<rt_v>() || !idx.v.known()) return nullptr;
      return arr->as<array_v>().generic_cell(checked_index(*arr, idx.v, db[n].where));
    }
    if(db.is<n_member>(n)) { // field
      auto const & m = db.as<n_member>(n);
      value * obj = place(m.object, env, frame);
      if(obj && obj->is<rt_v>() && shape_of(*obj) && shape_of(*obj)->is<agg_v>()) *obj = *shape_of(*obj); // erased aggregate - sample fields
      if(!obj || !obj->is<agg_v>()) return nullptr;
      for(auto & [k, v]: obj->as<agg_v>().fields)
        if(k == m.name) return &v;
      fail(cat("у значения типа ", types.describe(*obj), " нет поля ", str(m.name)), db[n].where);
    }
    return nullptr;
  }

  // --- operator ---

  flow eval_node(node_id n, n_op const & o, scope * env, uint32_t frame) {
    auto op = str(o.op);
    span where = db[n].where;
    // Operator declared to be an unbuilt primitive code calls its entity (overload by
    // type; embedded is also a candidate)
    if(!user_ops.empty() || o.postfix)
      if(symbol f = operator_entity(o); user_ops.contains(f.value)) {
        call_site site{where, std::string(str(f)), n.value, frame};
        auto user = user_candidates(f);
        bool lazy = std::ranges::any_of(user, [&](node_id fn) {
          return std::ranges::any_of(db.as<n_fn>(fn).params, [&](node_id p) { return db.as<n_param>(p).mode == param_mode::expr; });
        });
        if(o.postfix || lazy) return call_overloaded(functions[f], o.args, env, frame, site);
        std::vector<value> vals;
        for(node_id a: o.args) {
          TZ_TRY(v, eval(a, env, frame));
          vals.push_back(std::move(v));
        }
        std::vector<std::string> ts;
        for(auto const & v: vals) ts.push_back(types.describe(v));
        if(std::ranges::any_of(vals, [](value const & v) { return v.is<split_v>(); }) || user_operator_applies(f, ts))
          return dispatch_split(user, vals, 0, o.args, env, frame, site);
        // Built-in operation: user overload of these types is not accepted
        if(o.args.size() == 1) return ok(lift(vals[0], [&](value const & a) { return unary(op, a, where); }));
        if(op == "&&" || op == "||") return ok(builtin(sym(op == "&&" ? "and" : "or"), vals, where));
        return ok(binary(op, vals[0], vals[1], where));
      }
    if(o.postfix) fail(cat("нет постфиксного оператора ", op), where);
    if(o.args.size() == 1) {
      TZ_TRY(x, eval(o.args[0], env, frame));
      return ok(lift(x, [&](value const & a) { return unary(op, a, where); }));
    }
    // redefinition of context (checked { … }): the nearest, whose parameters assume types
    if(!overrides.empty() && override_depth == 0 && o.args.size() == 2 && op != "&&" && op != "||" && !(op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=")) {
      symbol f = operator_entity(o);
      if(std::ranges::any_of(overrides, [&](override_rec const & r) { return r.entity == f; })) {
        std::vector<value> vals;
        for(node_id a: o.args) {
          TZ_TRY(v, eval(a, env, frame));
          vals.push_back(std::move(v));
        }
        for(auto it = overrides.rbegin(); it != overrides.rend(); ++it) {
          if(it->entity != f) continue;
          auto const & ps = db.as<n_fn>(it->fn).params;
          bool fits = true;
          for(std::size_t k = 0; k < ps.size() && fits; ++k) {
            auto const & p = db.as<n_param>(ps[k]);
            if(p.type && db.is<n_ident>(p.type) && str(db.as<n_ident>(p.type).name) != types.describe(vals[k])) fits = false;
          }
          if(!fits) continue;
          ++override_depth; // Redefinition Body - Normal Operations
          struct depth_hold {
            uint32_t & d;
            ~depth_hold() { --d; }
          } dh{override_depth};
          return dispatch_split({it->fn}, vals, 0, o.args, env, frame, {where, std::string(str(f)), n.value, frame});
        }
        return ok(binary(op, vals[0], vals[1], where));
      }
    }
    if(op == "&&" || op == "||") {
      TZ_TRY(a, eval(o.args[0], env, frame));
      auto av = known_bool(a, db[o.args[0]].where);
      if(av && (op == "&&" ? !*av : *av)) return ok(value(*av));
      TZ_TRY(b, eval(o.args[1], env, frame));
      auto bv = known_bool(b, db[o.args[1]].where);
      if(av && bv) return ok(value(*bv));
      return ok(erase(value(true)));
    }
    if(op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") {
      TZ_TRY(rhs, eval(o.args[1], env, frame));
      // x = e and no name x is an announcement with type output
      if(op == "=" && db.is<n_ident>(o.args[0]) && !env->find(db.as<n_ident>(o.args[0]).name)) {
        symbol name = db.as<n_ident>(o.args[0]).name;
        if(checking()) implicit_decls.insert(n.value);
        if(checking())
          emit({report_event::stage, path, std::string(str(name)), rhs.known() ? cat("кт ", show_v(rhs)) : cat("рт ", types.describe(rhs)), where, {}});
        env->declare(name, std::move(rhs));
        return ok();
      }
      written(o.args[0], env);
      if(db.is<n_ident>(o.args[0]))
        if(value * c = env->find(db.as<n_ident>(o.args[0]).name); c && c->is<ref_v>()) return ref_assign(c->as<ref_v>(), op, rhs, where);
      if(!checking() && db.is<n_index>(o.args[0])) { // entry into an element: set, not cell
        auto const & ix = db.as<n_index>(o.args[0]);
        value * arr = place(ix.object, env, frame);
        if(!arr) fail("присваивание не в переменную", db[o.args[0]].where);
        TZ_TRY(idx, eval(ix.index, env, frame));
        uint32_t k = checked_index(*arr, idx, db[o.args[0]].where);
        value cur = arr->as<array_v>().get(k);
        arr->as<array_v>().set(k, op == "=" ? rhs : binary(std::string_view(op).substr(0, 1), cur, rhs, where));
        return ok();
      }
      value * target = place(o.args[0], env, frame);
      if(!target) {
        if(checking() && db.is<n_index>(o.args[0])) { // entry into the run-time-array element: type reconciliation
          node_id saved_write = std::exchange(index_write, o.args[0]);
          flow cur_f = eval(o.args[0], env, frame);
          index_write = saved_write;
          if(!cur_f.normal()) return cur_f;
          value cur = std::move(cur_f.v);
          value res = op == "=" ? rhs : binary(std::string_view(op).substr(0, 1), cur, rhs, where);
          if(!unknown(cur) && !unknown(res) && types.describe(cur) != types.describe(res))
            fail(cat("элемент массива типа ", types.describe(cur), " нельзя заменить значением типа ", types.describe(res)),
                where);
          return ok();
        }
        fail("присваивание не в переменную", db[o.args[0]].where);
      }
      if(ct_cells.contains(target) && ct_depth == 0 && !level_probe)
        fail("ct-переменная меняется только кодом при компиляции (ct { … })", where);
      bool pinned = target->pinned;
      if(!pinned && target->hidden) not_hidden(*target, symbol{}, where); // x.f = ... with a hidden name.
      if(op == "=") *target = std::move(rhs);
      else *target = binary(std::string_view(op).substr(0, 1), *target, rhs, where);
      if(pinned) pin(*target, {}, where); // The ct field remains a state during compilation
      if(target->is<array_v>() && db.is<n_ident>(o.args[0])) target->as<array_v>().gen = ++array_gen; // New buffer - old links are invalid
      return ok();
    }
    TZ_TRY(a, eval(o.args[0], env, frame));
    TZ_TRY(b, eval(o.args[1], env, frame));
    return ok(binary(op, a, b, where));
  }

  value unary(std::string_view op, value const & x, span where) {
    if(unknown(x)) return x;
    if(!x.known()) return erase(unary(op, *shape_or_fail(x, where), where));
    if(op == "!") return value(!truthy(x, where));
    if(op == "-") {
      if(x.is<int64_t>()) return value(int64_t(0 - uint64_t(x.as<int64_t>())));
      if(x.is<double>()) return value(-x.as<double>());
    }
    fail(cat("оператор ", op, " не определён для ", types.describe(x)), where);
  }

  value const * shape_or_fail(value const & x, span where) {
    value const * s = shape_of(x);
    if(!s) fail("операция над значением неизвестного типа", where);
    return s;
  }

  static bool unknown(value const & x) { return x.is<rt_v>() && !x.as<rt_v>(); }

  // ct-поле: значение известно при компиляции и остаётся известным.
  void pin(value & v, symbol field, span where) {
    if(!v.known())
      fail(cat("ct-поле", field ? cat(" ", str(field)) : std::string(), " — состояние при компиляции: значение должно быть известно, получено рт ", types.describe(v)), where);
    v.pinned = true;
  }

  // Hidden name (hide / own) can not be mentioned:
// A split path is a mistake on that path.
  void not_hidden(value const & v, symbol name, span where) {
    uint32_t h = v.hidden;
    if(!h && v.is<split_v>())
      for(auto const & a: v.as<split_v>().alts)
        if(a.hidden) h = a.hidden;
    if(!h) return;
    diagnostic d(cat(name ? cat("имя ", str(name)) : std::string("значение"), v.hidden ? " скрыто" : " скрыто в одном из путей", " (hide/own)"), where);
    d.note("скрыто здесь", db[node_id(h - 1)].where);
    if(!path.empty()) d.note(cat("в продолжении ", path), where);
    for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
    throw d;
  }

  bool scalar(value const & v) const {
    value const * x = shape_of(v);
    return x && (x->is<bool>() || x->is<int64_t>() || x->is<double>());
  }

  void hide_cell(value * cell, node_id at) {
    book.written(cell, true); // Branches and cycles see concealment as a record
    cell->hidden = at.value + 1;
  }

  flow eval_node(node_id n, n_hide const & h, scope * env, uint32_t) {
    value * cell = env->find(h.name);
    if(!cell || cell->is<thunk_v>()) fail(cat("hide: нет переменной ", str(h.name)), db[n].where);
    not_hidden(*cell, h.name, db[n].where);
    hide_cell(cell, n);
    return ok();
  }

  // Compilation state within the value: ct-fields and hidden names (§5.6).
  std::string state_of(value const & x) const {
    std::string s;
    if(x.hidden) s += "H";
    if(x.pinned) s += "P" + show_v(x);
    value const * v = shape_of(x);
    if(v && v->is<agg_v>())
      for(auto const & [k, f]: v->as<agg_v>().fields) {
        auto t = state_of(f);
        if(!t.empty()) s += cat(k.value, "=", t, ";");
      }
    return s;
  }

  // Binary operation; with run-time-operand, the result type is calculated on the samples.
  value binary(std::string_view op, value const & a, value const & b, span where) {
    if(a.is<split_v>()) return lift(a, [&](value const & x) { return binary(op, x, b, where); });
    if(b.is<split_v>()) return lift(b, [&](value const & y) { return binary(op, a, y, where); });
    if(unknown(a) || unknown(b)) return value(rt_v{}); // type is still being removed (recursion)
    if(!a.known() || !b.known()) {
      // whole division into run-time - the divider can drop div_by_zero - "painting"
      if(checking() && (op == "/" || op == "%") && !b.known() && types.describe(b) == "i64") rt_check("div_by_zero", where);
      return erase(arith(op, *shape_or_fail(erase(a), where), *shape_or_fail(erase(b), where), where));
    }
    return arith(op, a, b, where);
  }

  value arith(std::string_view op, value const & a, value const & b, span where) {
    // Strictly: without implicit i64 <-> f64 (§5.8)
    if(a.is<int64_t>() && b.is<int64_t>()) return int_op(op, a.as<int64_t>(), b.as<int64_t>(), where);
    if(a.is<double>() && b.is<double>()) return float_op(op, a.as<double>(), b.as<double>(), where);
    if(a.is<std::string>() && b.is<std::string>()) {
      if(op == "+") return value(a.as<std::string>() + b.as<std::string>());
      if(op == "==") return value(a.as<std::string>() == b.as<std::string>());
      if(op == "!=") return value(a.as<std::string>() != b.as<std::string>());
    }
    if(a.is<type_v>() && b.is<type_v>()) { // Types - values: tag = number (§5.1)
      if(op == "==") return value(a.as<type_v>().type == b.as<type_v>().type);
      if(op == "!=") return value(a.as<type_v>().type != b.as<type_v>().type);
    }
    if(a.is<bool>() && b.is<bool>()) {
      if(op == "==") return value(a.as<bool>() == b.as<bool>());
      if(op == "!=") return value(a.as<bool>() != b.as<bool>());
    }
    fail(cat("оператор ", op, " не определён для ", types.describe(a), " и ", types.describe(b)), where);
  }

  value int_op(std::string_view op, int64_t x, int64_t y, span where) {
    auto w = [](uint64_t r) { return value(int64_t(r)); }; // Modular arithmetic (§5.8)
    if(op == "+") return w(uint64_t(x) + uint64_t(y));
    if(op == "-") return w(uint64_t(x) - uint64_t(y));
    if(op == "*") return w(uint64_t(x) * uint64_t(y));
    if(op == "/" || op == "%") {
      if(y == 0) fail_rt("div_by_zero", "деление на ноль", where); // certain exception, not UB
      return value(op == "/" ? x / y : x % y);
    }
    return compare(op, x, y, where);
  }

  value float_op(std::string_view op, double x, double y, span where) {
    if(op == "+") return value(x + y);
    if(op == "-") return value(x - y);
    if(op == "*") return value(x * y);
    if(op == "/") return value(x / y);
    return compare(op, x, y, where);
  }

  value compare(std::string_view op, auto x, decltype(x) y, span where) {
    if(op == "==") return value(x == y);
    if(op == "!=") return value(x != y);
    if(op == "<") return value(x < y);
    if(op == ">") return value(x > y);
    if(op == "<=") return value(x <= y);
    if(op == ">=") return value(x >= y);
    fail(cat("неизвестный оператор ", op), where);
  }

  void check_type(value const & v, symbol tname, span where) {
    auto want = str(tname);
    if(want == "auto") return;
    auto check1 = [&](value const & x) {
      if(types.describe(x) != want && types.name(types.of(x)) != want)
        fail(cat("ожидался тип ", want, ", получен ", types.describe(x)), where);
    };
    if(v.is<split_v>())
      for(auto const & a: v.as<split_v>().alts) check1(a);
    else check1(v);
  }

  // --- challenge ---

  flow eval_node(node_id n, n_call const & c, scope * env, uint32_t frame) {
    span where = db[n].where;
    if(db.is<n_member>(c.callee)) { // a.f(b) ≡ f(a, b) (§4.5)
      auto const & m = db.as<n_member>(c.callee);
      node_list args{m.object};
      args.insert(args.end(), c.args.begin(), c.args.end());
      return call_named(m.name, args, env, frame, where, n.value);
    }
    if(db.is<n_ident>(c.callee)) {
      symbol name = db.as<n_ident>(c.callee).name;
      if(value * cell = env->find(name)) {
        if(cell->is<thunk_v>()) {
          thunk_v t = thunk_arg(c.callee, env, where);
          if(db.is<n_lambda>(t.expr) && db.as<n_lambda>(t.expr).block) return call_block(t, c.args, env, frame, where);
        }
        TZ_TRY(fv, read(cell));
        return call_value(fv, c.args, env, frame, where, n.value);
      }
      if(name == sym_intercept && !functions.contains(name)) return intercept(c.args, env, frame, where);
      if(name == sym_append && !functions.contains(name)) return append(c.args, env, frame, n);
      if(name == sym_spawn && !functions.contains(name)) return spawn(c.args, env, frame, where);
      if(name == sym_process && !functions.contains(name)) return start_process(c.args, env, frame, where);
      if(name == sym_yield && !functions.contains(name) && c.args.empty()) {
        if(!checking()) yield();
        return ok();
      }
      if(name == sym_is_index && c.args.size() == 2 && !functions.contains(name))
        if(value const * arr = peek(c.args[1], env)) { // array by name - without a copy
          TZ_TRY(k, eval(c.args[0], env, frame));
          std::vector<value> a{std::move(k), value()};
          if(arr->is<array_v>() && a[0].is<int64_t>()) {
            int64_t i = a[0].as<int64_t>();
            return ok(value(i >= 0 && uint64_t(i) < arr->as<array_v>().size()));
          }
          a[1] = *arr;
          return ok(builtin_lifted(name, a, 0, where));
        }
      return call_named(name, c.args, env, frame, where, n.value);
    }
    TZ_TRY(fv, eval(c.callee, env, frame));
    return call_value(fv, c.args, env, frame, where, n.value);
  }

  flow call_value(value const & fv, node_list const & args, scope * env, uint32_t frame, span where, uint32_t node = 0) {
    if(!fv.is<fn_v>()) fail(cat("значение типа ", types.describe(fv), " нельзя вызвать"), where);
    fn_v const & f = fv.as<fn_v>();
    if(f.name) return call_named(f.name, args, env, frame, where, node);
    node_list const & params = db.is<n_lambda>(f.lambda) ? db.as<n_lambda>(f.lambda).params : db.as<n_fn>(f.lambda).params;
    node_id body = db.is<n_lambda>(f.lambda) ? db.as<n_lambda>(f.lambda).body : db.as<n_fn>(f.lambda).body;
    if(params.size() != args.size()) fail(cat("ожидалось ", params.size(), " аргументов, передано ", args.size()), where);
    scope * inner = new_scope(f.env);
    scope_hold hold{*this, inner};
    for(uint32_t i = 0; i < params.size(); ++i) {
      auto const & p = db.as<n_param>(params[i]);
      if(p.mode == param_mode::expr) {
        inner->declare(p.name, value(thunk_v{args[i], env, frame, ctx}));
        continue;
      }
      if(value * cell = i < args.size() ? place_or_null(args[i], env) : nullptr) {
        inner->alias(p.name, cell);
        continue;
      }
      TZ_TRY(v, eval(args[i], env, frame));
      inner->declare(p.name, std::move(v));
    }
    return run_frame(body, inner, db.is<n_fn>(f.lambda) ? db.as<n_fn>(f.lambda).name : symbol{}, false, frame);
  }

  // Execute body function in a new frame. The result of the verification mode is a
  // merger of the usual result and all possible returns over run-time branches.
  flow run_frame(node_id body, scope * inner, symbol name, bool outline, uint32_t caller = 0) {
    uint32_t me = ++frame_counter;
    frames.push_back({me, name, outline, false, caller});
    struct overrides_hold { // The redefinition of context lives to the end of his frame
      std::vector<override_rec> & v;
      std::size_t n;
      ~overrides_hold() { v.resize(n); }
    } ov{overrides, overrides.size()};
    flow r = eval(body, inner, me);
    bool effect = frames.back().rt_effect;
    frames.pop_back();
    if(!frames.empty()) frames.back().rt_effect |= effect;
    last_rt_effect = effect;

    if(r.kind == flow_kind::ret && r.target == me) r = ok(std::move(r.v));
    if(auto it = may_return.find(me); it != may_return.end()) {
      value merged = r.normal() ? r.v : it->second.front();
      for(auto const & x: it->second) merged = merge(merged, x);
      may_return.erase(it);
      if(r.normal()) r.v = merged;
      else if(r.kind == flow_kind::ret) record_return(r), r = ok(merged); // path
    }
    return r;
  }

  flow call_named(symbol name, node_list const & args, scope * env, uint32_t frame, span where, uint32_t node = 0) {
    if(auto it = functions.find(name); it != functions.end())
      return call_overloaded(it->second, args, env, frame, {where, std::string(str(name)), node, frame});
    std::vector<value> vals;
    for(node_id a: args) {
      TZ_TRY(v, eval(a, env, frame));
      vals.push_back(std::move(v));
    }
    return ok(builtin_lifted(name, vals, 0, where));
  }

  // Fragmented Built-in Argument: A Challenge to Each Alternative
  value builtin_lifted(symbol name, std::vector<value> & vals, uint32_t from, span where) {
    for(uint32_t i = from; i < vals.size(); ++i) {
      if(!vals[i].is<split_v>()) continue;
      value orig = vals[i], out;
      bool first = true;
      for(auto const & alt: orig.as<split_v>().alts) {
        vals[i] = alt;
        value r = builtin_lifted(name, vals, i + 1, where);
        out = first ? r : merge(out, r);
        first = false;
      }
      vals[i] = orig;
      return out;
    }
    return builtin(name, vals, where);
  }

  // overload: Candidates by arity and types; require at the beginning of the body -
  // candidate restriction (§5.4); choose the most specific.
  flow call_overloaded(std::vector<node_id> const & fns, node_list const & args, scope * env, uint32_t frame, call_site site) {
    std::vector<value> vals(args.size());
    for(uint32_t i = 0; i < args.size(); ++i) {
      bool expr_everywhere = true;
      for(node_id f: fns) {
        auto const & ps = db.as<n_fn>(f).params;
        if(i >= ps.size() || db.as<n_param>(ps[i]).mode != param_mode::expr) expr_everywhere = false;
      }
      if(expr_everywhere) vals[i] = value(thunk_v{args[i], env, frame, ctx});
      else if(value * cell = checking() ? nullptr : place_or_null(args[i], env); cell && cell->is<array_v>())
        vals[i] = value(array_v::borrow(cell->as<array_v>())); // place-link, not copy
      else {
        TZ_TRY(v, eval(args[i], env, frame));
        vals[i] = std::move(v);
      }
    }
    return dispatch_split(fns, vals, 0, args, env, frame, site);
  }

  // The split argument: Continuation-challenge is cloned by alternatives.
  flow dispatch_split(std::vector<node_id> const & fns, std::vector<value> & vals, uint32_t from, node_list const & args,
      scope * env, uint32_t frame, call_site const & site) {
    for(uint32_t i = from; i < vals.size(); ++i) {
      if(!vals[i].is<split_v>()) continue;
      value orig = vals[i];
      flow out;
      bool first = true;
      for(auto const & alt: orig.as<split_v>().alts) {
        vals[i] = alt;
        auto copy = vals;
        flow r;
        try {
          r = dispatch_split(fns, copy, i + 1, args, env, frame, site);
        } catch(diagnostic & d) { // Trace - the way: what is the argument in what state
          d.note(cat("в пути [", i < args.size() && db.is<n_ident>(args[i]) ? cat(str(db.as<n_ident>(args[i]).name), ": ") : std::string(),
                     alt.known() ? show_v(alt) : types.describe(alt), "]"),
              site.where);
          throw;
        }
        out = first ? r : join_branches(out, r);
        first = false;
      }
      vals[i] = orig;
      return out;
    }
    return select_and_invoke(fns, vals, args, env, frame, site);
  }

  flow select_and_invoke(std::vector<node_id> const & fns, std::vector<value> & vals, node_list const & args,
      scope * env, uint32_t frame, call_site const & site) {
    struct candidate {
      node_id fn;
      int score;
      bool maybe = false; // Whether it is suitable - decide the value (unpacking run-time values in parsing)
    };
    std::vector<candidate> good;
    std::vector<std::string> rejected;
    for(node_id f: fns) {
      auto const & fn = db.as<n_fn>(f);
      if(fn.params.size() != vals.size()) {
        rejected.push_back(cat(describe_fn(f), ": ожидает ", fn.params.size(), " аргументов"));
        continue;
      }
      int score = 0;
      bool maybe = false;
      std::string why;
      for(uint32_t i = 0; i < fn.params.size() && why.empty(); ++i) {
        auto const & p = db.as<n_param>(fn.params[i]);
        if(p.type && db.is<n_pat>(p.type)) { // sample: unpacking - candidate condition (as required)
          scope * probe = new_scope(globals);
          scope_hold hold{*this, probe};
          unpacked r = unpack_into(p.type, vals[i], probe, probe, 0, db[fn.params[i]].where);
          if(r == unpacked::no) why = cat("аргумент ", i + 1, " не распаковывается в ", printer(db).print(p.type));
          else score += 1 + int(pattern_checks(p.type));
          maybe = maybe || r == unpacked::maybe;
        } else if(p.type && db.is<n_ident>(p.type)) {
          auto want = str(db.as<n_ident>(p.type).name);
          if(types.describe(vals[i]) != want && types.name(types.of(vals[i])) != want)
            why = cat("параметр ", str(p.name), " ожидает ", want, ", передано ", types.describe(vals[i]));
          else ++score;
        }
      }
      if(why.empty()) {
        auto [passed, n_req, reason] = leading_requires(f, vals, args, env);
        if(!passed) why = reason;
        score += n_req;
      }
      if(why.empty()) good.push_back({f, score, maybe});
      else rejected.push_back(cat(describe_fn(f), ": ", why));
    }

    if(good.empty()) {
      diagnostic d(cat("нет подходящей перегрузки ", site.what), site.where);
      for(auto & r: rejected) d.note(cat("кандидат ", r), site.where);
      if(!path.empty()) d.note(cat("в продолжении ", path), site.where);
      for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
      throw d;
    }
    std::ranges::stable_sort(good, std::greater{}, &candidate::score);
    // runtime - overload in terms of:
// parsing candidates,
    // whose unpacking depends on the run-time-value, all are sorted out - by specificity,
    // to the first exactly suitable; the results merge like if branches.
    if(checking() && good[0].maybe) {
      std::vector<candidate> reach;
      for(auto const & c: good) {
        reach.push_back(c);
        if(!c.maybe) break;
      }
      if(reach.size() > 1) {
        flow out;
        for(std::size_t k = 0; k < reach.size(); ++k) {
          auto copy = vals;
          if(site.node) emit({report_event::resolved, {}, cat(reach[k].fn.value), {}, {}, {}, site.node});
          flow r = invoke(reach[k].fn, copy, args, env, site);
          out = k ? join_branches(out, r) : r;
        }
        return out;
      }
    }
    if(good.size() > 1 && good[0].score == good[1].score && !good[0].maybe && !good[1].maybe) {
      diagnostic d(cat("неоднозначный вызов ", site.what), site.where);
      for(auto & c: good)
        if(c.score == good[0].score) d.note(cat("кандидат ", describe_fn(c.fn)), db[c.fn].where);
      throw d;
    }
    // Which overload has chosen the challenge in this instance to move from
    // editor
    if(checking() && site.node) emit({report_event::resolved, {}, cat(good[0].fn.value), {}, {}, {}, site.node});
    return invoke(good[0].fn, vals, args, env, site);
  }

  // require at the beginning of the body: run-time - Candidate condition does not
  // cut off - it is checked in runtime (§5.4).
  std::tuple<bool, int, std::string> leading_requires(node_id f, std::vector<value> & vals, node_list const & args, scope * env) {
    auto const & fn = db.as<n_fn>(f);
    if(fn.expr_body || !db.is<n_block>(fn.body)) return {true, 0, ""};
    scope * probe = new_scope(globals);
    scope_hold hold{*this, probe};
    bind_values(fn.params, vals, args, env, probe);
    int n = 0;
    for(node_id s: db.as<n_block>(fn.body).items) {
      if(!db.is<n_require>(s)) break;
      ++n;
      flow c = eval(db.as<n_require>(s).cond, probe, 0);
      if(c.v.is<bool>() && !c.v.as<bool>()) return {false, n, "require не выполнено"};
      if(!c.v.is<bool>() && c.v.known()) return {false, n, "require не bool"};
    }
    return {true, n, ""};
  }

  flow invoke(node_id f, std::vector<value> & vals, node_list const & args, scope * env, call_site const & site) {
    auto const & fn = db.as<n_fn>(f);
    check_inside(f, site);
    declared_invalidations(f, args, env, site.node);
    // own-параметр: значение уходит в функцию, имя вызывающего исчезает (явное перемещение, §5.6)
    for(uint32_t k = 0; k < fn.params.size() && k < args.size(); ++k)
      if(db.as<n_param>(fn.params[k]).mode == param_mode::own && db.is<n_ident>(args[k]) && !scalar(vals[k])) // scalar moves around
        if(value * cell = env->find(db.as<n_ident>(args[k]).name); cell && !cell->is<thunk_v>()) hide_cell(cell, node_id(site.node));
    if(fn.outline && inline_required(f))
      fail(cat(site.what, " помечена outline, но использует семантику инлайна"), db[f].where);
    if(checking() && param_writes_checked.insert(f.value).second)
      for(uint32_t k = 0; k < fn.params.size(); ++k)
        if(db.as<n_param>(fn.params[k]).mode != param_mode::expr && writes_param(f, k))
          param_writes.push_back(cat(describe_fn(f), ": меняет ", str(db.as<n_param>(fn.params[k]).name)));

    // instance key : only runtime is part of the argument
    bool all_known = true;
    std::string key = cat(str(fn.name), "(");
    for(uint32_t i = 0; i < vals.size(); ++i) {
      if(i) key += ", ";
      if(vals[i].is<thunk_v>()) key += "expr";
      else if(!checking()) key += types.describe(vals[i]);
      else if(vals[i].known()) key += "кт";
      else {
        key += "рт " + types.describe(vals[i]);
        all_known = false;
      }
    }
    key += ")";
    if(!overrides.empty()) { // instance is specialized in context:
      std::vector<std::string> ctxs;
      for(auto const & o: overrides)
        if(std::ranges::find(ctxs, std::string(str(o.context))) == ctxs.end()) ctxs.push_back(std::string(str(o.context)));
      for(auto const & c: ctxs) key += cat("[", c, "]");
    }

    // Recursion in static parsing. According to known values, the usual execution:
    // “recursion stops the condition”; the activity key includes CT values. By mouth
    // values is a fixed point: the recursive call receives the current approximation
    // of the type of result, the body is disassembled again until the type ceases to
    // change.
//).
    std::string active_key = cat(key, "@", f.value); // overload: Another with the same argument is not recursion
    if(checking()) {
      active_key += "|";
      // The expr-argument is the code of the caller: the nested for(…) for(…) is not
      // recursion, the internal has another block; recursion transmits the same node further.
      for(auto & v: vals)
        active_key += v.is<thunk_v>() ? cat("expr", v.as<thunk_v>().expr.value, "|") : v.known() ? show_v(v) + "|"
                                                                                                 : "·|";
      if(active.contains(active_key)) {
        recursed.insert(active_key);
        auto it = approx.find(active_key);
        return ok(it != approx.end() ? it->second : value(rt_v{}));
      }
      if(frames.size() > 2000) fail("рекурсия не останавливается при статическом разборе", site.where);
    }

    // Reactive cache: instance with unchanged dependencies is not rediscovered
    std::string memo_key = active_key + "#" + path;
    bool memoize = checking() && cache && approx.empty() && memoizable(f, vals);
    if(memoize) {
      if(auto it = cache->entries.find(memo_key); it != cache->entries.end() && valid(it->second)) {
        ++cache->hits;
        for(auto const & ev: it->second.events) emit(ev);
        std::size_t replay_mark = journal.size();
        bool all_folded = !it->second.journal.empty();
        for(auto const & [node, v, seen, div, eff, folded]: it->second.journal) {
          journal.push_back({node, v, seen, div, eff, ctx, folded});
          all_folded = all_folded && folded;
        }
        note_layer(f, key, replay_mark, all_folded);
        observations_dirty = true;
        if(it->second.effect) {
          ++effects;
          if(!frames.empty()) frames.back().rt_effect = true;
        }
        last_rt_effect = it->second.effect;
        if(!deps.empty()) deps.back().insert(it->second.deps.begin(), it->second.deps.end());
        return ok(it->second.result);
      }
    }
    std::vector<report_event> recorded;
    if(memoize) {
      recording.push_back(&recorded);
      deps.emplace_back();
    }
    if(checking() && !deps.empty()) deps.back()[f.value] = db.node_revision(f);

    if(jit && !checking() && overrides.empty()) { // JIT: machine code instance, if any (in context, as long as the interpreter)
      if(auto r = try_jit(f, vals, args, env, key)) return *r;
    }

    stack.push_back(site);
    if(checking()) instance_keys.push_back(key);
    struct key_hold {
      interpreter & vm;
      bool on;
      ~key_hold() {
        if(on) vm.instance_keys.pop_back();
      }
    } kh{*this, checking()};
    uint64_t ctx_saved = std::exchange(ctx, inline_context(ctx, site.node)); // body in its context of embedding
    std::size_t journal_mark = journal.size();
    frame_floors.push_back({frame_counter, false});
    flow r;
    for(uint32_t pass = 0;; ++pass) {
      scope * inner = new_scope(globals);
      scope_hold hold{*this, inner};
      bind_values(fn.params, vals, args, env, inner, f);
      if(checking()) active.insert(active_key);
      try {
        std::vector<value> escaped; // "painting": exceptions to instance
        throw_sinks.push_back(&escaped);
        try {
          r = run_frame(fn.body, inner, fn.name, fn.outline, site.frame);
        } catch(...) {
          throw_sinks.pop_back();
          throw;
        }
        throw_sinks.pop_back();
        if(checking()) {
          for(auto const & v: escaped) emit({report_event::throws, key, exception_type(v), {}, {}, {}});
          if(r.kind == flow_kind::thr)
            for(auto const & a: r.v.is<split_v>() ? r.v.as<split_v>().alts : std::vector<value>{r.v})
              emit({report_event::throws, key, exception_type(a), {}, {}, {}});
        }
        for(auto const & v: escaped) record_throw(v);
      } catch(...) {
        stack.pop_back();
        ctx = ctx_saved;
        frame_floors.pop_back();
        if(memoize) {
          recording.pop_back();
          deps.pop_back();
        }
        if(checking()) {
          active.erase(active_key);
          approx.erase(active_key);
          recursed.erase(active_key);
        }
        throw;
      }
      if(checking()) active.erase(active_key);
      if(!checking() || !recursed.contains(active_key) || !r.normal()) break;
      value prev = approx.contains(active_key) ? approx[active_key] : value(rt_v{});
      value next = erase(merge(prev, r.v));
      if(types.describe(next) == types.describe(prev) && !unknown(next)) break;
      if(pass > 8) fail(cat("тип результата рекурсивной ", str(fn.name), " не сходится: ", types.describe(next)), site.where);
      approx[active_key] = next;
    }
    stack.pop_back();
    ctx = ctx_saved;
    if(checking()) {
      approx.erase(active_key);
      recursed.erase(active_key);
    }

    // curled up in compile-time: all arguments and result known, runtime no effects
    bool folded_now = checking() && all_known && r.normal() && r.v.known() && !last_rt_effect;
    if(checking()) note_layer(f, key, journal_mark, folded_now);
    if(folded_now) {
      emit({report_event::folded, std::string(str(fn.name)), {}, {}, {}, {}});
      // There is no curled call in runtime - only type observations
      for(std::size_t i = journal_mark; i < journal.size(); ++i) journal[i].folded = true;
      observations_dirty = true;
    } else {
      emit({report_event::instance, path.empty() ? key : cat(key, "   ", path), {}, {}, {}, {}});
      emit({report_event::live, {}, {}, {}, {}, {}, f.value}); // I need runtime.
    }
    bool outer_return = frame_floors.back().second;
    frame_floors.pop_back();
    if(memoize) {
      recording.pop_back();
      auto mine = std::move(deps.back());
      deps.pop_back();
      if(!deps.empty()) deps.back().insert(mine.begin(), mine.end());
      mine[mod.value] = db.node_revision(mod); // set of functions (overload)
      if(r.normal() && approx.empty() && !outer_return) {
        ++cache->recomputed;
        cache->recomputed_names.push_back(std::string(str(fn.name)));
        check_cache::entry e{r.v, std::move(mine), std::move(recorded), {}, last_rt_effect};
        for(std::size_t i = journal_mark; i < journal.size(); ++i)
          e.journal.emplace_back(journal[i].node, journal[i].v, journal[i].seen, journal[i].diverted, journal[i].effect, journal[i].folded);
        cache->entries[memo_key] = std::move(e);
      }
    }
    return r; // return, addressed to the caller (semantic inlining), goes further
  }

  std::optional<flow> try_jit(node_id f, std::vector<value> & vals, node_list const & args, scope * env,
      std::string const & key);

  bool valid(check_cache::entry const & e) const {
    for(auto const & [node, rev]: e.deps)
      if(db.node_revision(node_id(node)) != rev) return false;
    return true;
  }

  // Parameters: alias per caller cell, if the argument is a place; otherwise its own cell.
  void bind_values(node_list const & params, std::vector<value> & vals, node_list const & args, scope * env, scope * inner, node_id f = {}) {
    for(uint32_t i = 0; i < params.size(); ++i) {
      auto const & p = db.as<n_param>(params[i]);
      if(p.type && db.is<n_pat>(p.type)) { // parameter-sample: names - from the unpacking of the argument
        unpack_into(p.type, vals[i], inner, inner, 0, db[params[i]].where);
        continue;
      }
      // array element to variable parameter - reference to element, not copy
      if(f && i < args.size() && p.mode != param_mode::expr && p.mode != param_mode::own && db.is<n_index>(args[i]) && writes_param(f, i))
        if(auto r = element_ref(args[i], env, 0, args[i])) {
          inner->declare(p.name, std::move(*r));
          continue;
        }
      if(p.mode != param_mode::expr && p.mode != param_mode::own && !vals[i].is<split_v>()) {
        if(value * cell = i < args.size() ? place_or_null(args[i], env) : nullptr; cell && !cell->is<split_v>()) {
          inner->alias(p.name, cell);
          continue;
        }
      }
      inner->declare(p.name, vals[i]);
    }
  }

  value * place_or_null(node_id n, scope * env) {
    if(!db.is<n_ident>(n)) return nullptr;
    value * c = env->find(db.as<n_ident>(n).name);
    if(c) not_hidden(*c, db.as<n_ident>(n).name, db[n].where);
    return c && !c->is<thunk_v>() ? c : nullptr;
  }

  // Contextual condition [[inside(g)]] : entity exists only in the blocks of
  // construction g.
// 
// Check the place of call in the text.
  void check_inside(node_id f, call_site const & site) {
    for(node_id a: db.as<n_fn>(f).attrs) {
      if(!db.is<n_call>(a) || !db.is<n_ident>(db.as<n_call>(a).callee) || str(db.as<n_ident>(db.as<n_call>(a).callee).name) != "inside") continue;
      auto const & c = db.as<n_call>(a);
      if(c.args.size() != 1 || !db.is<n_ident>(c.args[0])) fail("[[inside(конструкция)]]", db[a].where);
      symbol g = db.as<n_ident>(c.args[0]).name;
      if(!site.node || !inside_block_of(node_id(site.node), g))
        fail(cat(site.what, " существует только внутри блоков ", str(g)), site.where);
    }
  }

  bool inside_block_of(node_id n, symbol g) const {
    for(node_id c = n, p = db[n].parent; p; c = p, p = db[p].parent) {
      if(db.is<n_fn>(p)) return false;
      if(!db.is<n_call>(p)) continue;
      auto const & call = db.as<n_call>(p);
      if(!db.is<n_ident>(call.callee) || db.as<n_ident>(call.callee).name != g) continue;
      bool block = db.is<n_block>(c) || (db.is<n_lambda>(c) && db.as<n_lambda>(c).block) || db.is<n_pack>(c) || (call.tail && call.args.back() == c);
      if(block && std::ranges::find(call.args, c) != call.args.end()) return true;
    }
    return false;
  }

  // The function uses inline semantics (§6.1): expr-parameter or address return.
  bool inline_required(node_id f) const {
    for(node_id p: db.as<n_fn>(f).params)
      if(db.as<n_param>(p).mode == param_mode::expr) return true;
    return has_addressed_return(db.as<n_fn>(f).body);
  }

  std::vector<node_id> user_candidates(symbol f) const {
    std::vector<node_id> out;
    if(auto it = functions.find(f); it != functions.end())
      for(node_id fn: it->second)
        if(!intrinsic_operator(fn)) out.push_back(fn);
    return out;
  }

  // Does the user overload operator accept such types of operands?
  bool user_operator_applies(symbol f, std::vector<std::string> const & ts) const {
    for(node_id fn: user_candidates(f)) {
      auto const & ps = db.as<n_fn>(fn).params;
      bool fits = ps.size() == ts.size();
      for(std::size_t k = 0; fits && k < ps.size(); ++k) {
        auto const & p = db.as<n_param>(ps[k]);
        if(p.type && db.is<n_ident>(p.type) && str(db.as<n_ident>(p.type).name) != ts[k]) fits = false;
      }
      if(fits) return true;
    }
    return false;
  }

  symbol entity_of(n_op const & o) const {
    return db.strings.intern(cat(o.args.size() == 2 ? "infix" : o.postfix ? "postfix"
                                                                          : "prefix",
        str(o.op)));
  }

  bool user_operator(node_id n) const { // node operator declared code (not primitive)
    if(!db.is<n_op>(n)) return false;
    auto const & o = db.as<n_op>(n);
    if(o.postfix) return true;
    if(user_ops.empty()) return false;
    return user_ops.contains(db.strings.intern(cat(o.args.size() == 2 ? "infix" : "prefix", str(o.op))).value);
  }

  bool has_addressed_return(node_id n) const {
    if(!n) return false;
    if(db.is<n_return>(n) && db.as<n_return>(n).target) return true;
    if(db.is<n_lambda>(n) || db.is<n_fn>(n)) return false;
    bool found = false;
    db.for_each_child(n, [&](node_id c) { found = found || has_addressed_return(c); });
    return found;
  }

  std::string describe_fn(node_id f) const {
    auto const & fn = db.as<n_fn>(f);
    std::string s = cat(str(fn.name), "(");
    for(uint32_t i = 0; i < fn.params.size(); ++i) {
      if(i) s += ", ";
      auto const & p = db.as<n_param>(fn.params[i]);
      if(p.type && db.is<n_pat>(p.type)) {
        s += printer(db).print(p.type);
        continue;
      }
      if(p.type && db.is<n_ident>(p.type)) s += cat(str(db.as<n_ident>(p.type).name), " ");
      s += str(p.name);
    }
    return s + ")";
  }

  // --- embedded functions (M0; later VM and stdlib services in language) ---

  void rt_effect() {
    ++effects;
    if(!frames.empty()) frames.back().rt_effect = true;
  }

  // Built-in, visible program (VM services): name and parameters - for addition in
  // the editor. The internal ones (stream *, net *, * checked, file *) wrap prelude -
  // they are not here. New visible built-in over here.
  static constexpr std::pair<std::string_view, std::string_view> visible_builtins[] = {
      {"print", "(x)"}, {"read_floats", "(stdin)"}, {"parse_int", "(s)"}, {"range", "(n)"},
      {"fill", "(n, x)"}, {"text", "(x)"}, {"typeof", "(x)"}, {"fields", "(T)"}, {"attrs", "(f)"},
      {"emit", "(quote)"}, {"static_level", "(f)"}, {"is_err", "(r)"}, {"zero", "(T)"},
      {"f64", "(x)"}, {"i64", "(x)"}, {"not", "(x)"}};

  value builtin(symbol name, std::vector<value> & a, span where) {
    auto n = str(name);
    if(n == "print") {
      rt_effect();
      if(checking()) return {};
      std::string line;
      for(uint32_t i = 0; i < a.size(); ++i) line += (i ? " " : "") + show_v(a[i]);
      if(captured) *captured += line + "\n";
      else put(stdout, cat(line, "\n"));
      return {};
    }
    // --- entity compiler as value (§7.4) ---
    if(n == "emit" && a.size() == 1) { // Advertisements from quote to current elaboration
      node_id q = code_node(a[0], where);
      if(!db.is<n_quote>(q)) fail("emit: ожидался quote { объявления }", where);
      for(node_id item: db.as<n_quote>(q).items) register_item(item);
      return value();
    }
    if(n == "attrs" && a.size() == 1) { // [[…]] attributes of a function or type - values
      std::vector<node_id> nodes;
      symbol owner;
      if(a[0].is<fn_v>() && a[0].as<fn_v>().name) {
        owner = a[0].as<fn_v>().name;
        if(auto it = functions.find(owner); it != functions.end())
          for(node_id f: it->second)
            for(node_id x: db.as<n_fn>(f).attrs) nodes.push_back(x);
      } else if(a[0].is<type_v>()) {
        owner = sym(types.name(a[0].as<type_v>().type));
        if(auto it = structs.find(owner); it != structs.end())
          for(node_id x: db.as<n_struct>(it->second).attrs) nodes.push_back(x);
      } else fail("attrs: ожидалась функция или тип", where);
      if(auto it = ext_attrs.find(owner); it != ext_attrs.end())
        for(node_id x: it->second) nodes.push_back(x);
      std::vector<value> vs;
      for(node_id x: nodes) {
        flow f = eval(x, globals, 0);
        if(!f.normal()) fail("атрибут не вычислился", db[x].where);
        vs.push_back(std::move(f.v));
      }
      return value(array_v::of(std::move(vs)));
    }
    if(n == "fields" && a.size() == 1 && a[0].is<type_v>()) { // aggregate field: {name, type}
      symbol owner = sym(types.name(a[0].as<type_v>().type));
      if(!structs.contains(owner)) fail(cat("fields: ", str(owner), " — не агрегат"), where);
      std::vector<value> vs;
      for(node_id f: fields_of_struct(owner)) {
        auto const & v = db.as<n_var>(f);
        auto tn = str(db.as<n_ident>(v.type).name);
        type_id t = types.builtin(tn) ? types.builtin(tn) : types.named(tn);
        agg_v d{types.named("field"), {}};
        d.fields.push_back({sym("name"), value(std::string(str(v.name)))});
        d.fields.push_back({sym("type"), value(type_v{t})});
        vs.push_back(value(std::move(d)));
      }
      return value(array_v::of(std::move(vs)));
    }
    if(n == "typeof" && a.size() == 1) { // Value Approach: Type is also known for run-time
      if(a[0].is<rt_v>() && !a[0].as<rt_v>()) fail("тип неизвестен", where);
      return value(type_v{types.of(a[0])});
    }
    if(n == "zero" && a.size() == 1 && a[0].is<type_v>()) {
      auto t = types.name(a[0].as<type_v>().type);
      if(t == "i64") return value(int64_t(0));
      if(t == "f64") return value(double(0));
      fail(cat("zero не определён для ", t), where);
    }
    if(n == "range" && a.size() == 1) {
      value r;
      if(!a[0].known() && types.describe(a[0]) == "i64") r = erase(value(array_v::of({value(int64_t(0))})));
      else if(a[0].is<int64_t>()) {
        array_v xs;
        for(int64_t i = 0; i < a[0].as<int64_t>(); ++i) xs.push(value(i));
        r = value(std::move(xs));
      }
      if(!r.is<unit_v>()) {
        if(checking() && fact_of(a[0], fact::size_of)) { // range(xs.size) - xs indices
          fact f = facts[a[0].proof - 1];
          f.kind = fact::range_of;
          r.proof = add_fact(f);
        }
        return r;
      }
    }
    // prelude (operator (a) + (b) => add(a, b))
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 11> prim2{
        {{"add", "+"}, {"sub", "-"}, {"mul", "*"}, {"div", "/"}, {"rem", "%"}, {"eq", "=="}, {"ne", "!="}, {"lt", "<"}, {"gt", ">"}, {"le", "<="}, {"ge", ">="}}};
    for(auto [name, op]: prim2)
      if(n == name && a.size() == 2) return binary(op, a[0], a[1], where);
    if((n == "neg" || n == "not") && a.size() == 1) return unary(n == "neg" ? "-" : "!", a[0], where);
    // overflow check arithmetic (context checked, §8.2)
    if((n == "add_checked" || n == "sub_checked" || n == "mul_checked") && a.size() == 2) {
      std::string_view op = n == "add_checked" ? "+" : n == "sub_checked" ? "-"
                                                                          : "*";
      if(a[0].is<int64_t>() && a[1].is<int64_t>()) {
        int64_t r;
        int64_t x = a[0].as<int64_t>(), y = a[1].as<int64_t>();
        bool bad = op == "+" ? __builtin_add_overflow(x, y, &r) : op == "-" ? __builtin_sub_overflow(x, y, &r)
                                                                            : __builtin_mul_overflow(x, y, &r);
        if(bad) fail_rt("overflow", cat("переполнение i64: ", x, " ", op, " ", y), where);
        return value(r);
      }
      if(checking() && types.describe(a[0]) == "i64" && types.describe(a[1]) == "i64") rt_check("overflow", where); // run-time - may throw
      return binary(op, a[0], a[1], where);
    }
    if((n == "and" || n == "or") && a.size() == 2) {
      auto x = known_bool(a[0], where), y = known_bool(a[1], where);
      if(x && y) return value(n == "and" ? *x && *y : *x || *y);
      return erase(value(true));
    }
    if(n.starts_with("stream_")) return stream_op(n, a, where);
    if(n.starts_with("net_") || n.starts_with("file_")) return io_op_call(n, a, where);
    if(n == "process_wait" || n == "process_error") return process_op(n, a, where);
    if(n == "text" && a.size() == 1) // text of value (bytes): number - decimal record
      return a[0].known() ? value(a[0].is<std::string>() ? a[0].as<std::string>() : show_v(a[0])) : erase(value(std::string("x")));
    if(n == "static_level" && a.size() == 1 && a[0].is<fn_v>() && a[0].as<fn_v>().name) return static_level(a[0].as<fn_v>().name, where);
    if(n == "is_index" && a.size() == 2) { // 0 <= k < size(xs): The test that makes k a safe index
      if(unknown(a[0]) || unknown(a[1])) return value(rt_v{}); // type
      if(types.describe(a[0]) != "i64") fail(cat("is_index: индекс должен быть i64, получено ", types.describe(a[0])), where);
      if(a[0].is<int64_t>() && a[1].is<array_v>()) {
        int64_t k = a[0].as<int64_t>();
        return value(k >= 0 && uint64_t(k) < a[1].as<array_v>().size());
      }
      if(!a[1].is<array_v>() && !(shape_of(a[1]) && shape_of(a[1])->is<array_v>()))
        fail(cat("is_index: ожидался массив, получено ", types.describe(a[1])), where);
      return erase(value(true));
    }
    if(n == "fill" && a.size() == 2 && types.describe(a[0]) == "i64") { // massif
      if(!a[0].known() || !a[1].known()) return erase(value(array_v::of({a[1].known() ? a[1] : *shape_of(a[1])})));
      array_v r;
      for(int64_t i = 0; i < a[0].as<int64_t>(); ++i) r.push(a[1]);
      return value(std::move(r));
    }
    if(n == "f64" && a.size() == 1 && types.describe(a[0]) == "i64")
      return a[0].known() ? value(double(a[0].as<int64_t>())) : erase(value(double(1)));
    if(n == "i64" && a.size() == 1 && types.describe(a[0]) == "f64")
      return a[0].known() ? value(int64_t(a[0].as<double>())) : erase(value(int64_t(1)));
    if(n == "read_floats") {
      rt_effect();
      std::string line;
      bool have = false;
      if(checking() && (!inputs || inputs->stdin_lines.empty()))
        return erase(value(array_v::of({value(double(1))})));
      if(inputs) { // Entry is set (statically known or submitted by session)
        if(!inputs->stdin_lines.empty()) {
          line = inputs->stdin_lines.front();
          inputs->stdin_lines.erase(inputs->stdin_lines.begin());
        }
        have = true;
      }
      array_v r;
      if(have || os::host_stdin().next(line)) { // stdin host - through the OS layer (M3: VM service)
        for(auto w: words_of(line)) {
          if(w.starts_with('+')) w.remove_prefix(1);
          double x;
          auto [end, ec] = std::from_chars(w.data(), w.data() + w.size(), x);
          if(ec != std::errc{} || end != w.data() + w.size()) break; // as before: before the first non-number
          r.push(value(x));
        }
      }
      return value(std::move(r));
    }
    if(n == "parse_int" && a.size() == 1 && types.describe(a[0]) == "str") {
      auto make = [&](bool good, int64_t v, std::string err) {
        agg_v r{types.named("result"), {}};
        r.fields.push_back({sym("ok"), value(good)});
        r.fields.push_back({sym("value"), value(v)});
        r.fields.push_back({sym("error"), value(std::move(err))});
        return value(std::move(r));
      };
      if(!a[0].known()) return erase(make(true, 0, ""));
      auto const & s = a[0].as<std::string>();
      int64_t v = 0;
      bool good = !s.empty();
      for(char ch: s) {
        if(ch < '0' || ch > '9') {
          good = false;
          break;
        }
        v = v * 10 + (ch - '0');
      }
      return make(good, good ? v : 0, good ? "" : cat("не число: ", in_quotes(s)));
    }
    if(n == "is_err" && a.size() == 1 && types.describe(a[0]) == "result") {
      value okf = member(a[0], sym("ok"), where);
      return okf.known() ? value(!okf.as<bool>()) : erase(value(true));
    }
    fail(cat("неизвестная функция ", n, "(", [&] {
            std::string s;
            for (uint32_t i = 0; i < a.size(); ++i) s += (i ? ", " : "") + types.describe(a[i]);
            return s; }(), ")"), where);
  }

#undef TZ_TRY

  // --- condition ---

  struct stage_rec {
    span where;
    std::string path, name, what;
  };

  struct split_rec {
    span where;
    std::string name;
    std::vector<std::string> alts;
  };

  ast_db & db;
  node_id mod;
  mode run_mode;
  std::deque<scope> scope_arena;
  std::vector<scope *> free_scopes;
  scope * globals = nullptr;
  std::map<symbol, std::vector<node_id>> functions;
  type_registry own_types;
  type_registry & types;
  std::vector<call_site> stack;
  std::vector<frame_rec> frames;
  uint32_t frame_counter = 0;
  bool last_rt_effect = false;
  std::map<uint32_t, std::vector<value>> may_return;
  std::map<symbol, node_id> structs; // named units of the module
  bool elaborated = false;
  std::map<symbol, std::vector<node_id>> ext_fields, ext_attrs; // Extensions Name { … }
  std::vector<std::vector<value> *> throw_sinks; // run-time branch shooters: try and instance
  std::map<std::string, std::set<std::string>> throws_of; // instance → Types of Exceptions (Report)
  std::vector<call_site> throw_stack; // last-throw
  std::map<std::string, uint32_t> instance_counts;
  std::map<std::string, uint32_t> folded;
  std::set<std::string> active;
  std::set<std::string> recursed;
  std::map<std::string, value> approx;

  struct excluded_rec {
    span where;
    std::string path, why;
  };
  std::vector<excluded_rec> excluded;
  std::vector<stage_rec> stages;
  std::vector<split_rec> splits;
  std::string path; // current continuation of splitting
  std::string * captured = nullptr;
  check_cache * cache = nullptr;
  struct jit_engine * jit = nullptr;
  struct a64_check * a64 = nullptr; // AArch64 in parallel with host (qemu)
  std::optional<uint64_t> fuel;
  std::vector<std::pair<uint32_t, bool>> frame_floors; // (The first frame of the call was a return to the outside)
  std::vector<std::vector<report_event> *> recording;
  std::vector<std::map<uint32_t, uint64_t>> deps;
  std::optional<known_inputs> inputs;
  std::vector<obs_entry> journal;
  mutable std::unordered_map<uint32_t, observation> observations;
  mutable std::map<std::pair<uint32_t, uint64_t>, observation> observations_ctx; // insertion
  uint64_t ctx = 0; // current context
  mutable bool observations_dirty = false;
  std::set<uint32_t> live_fns;
  std::set<uint32_t> param_writes_checked;
  std::vector<std::string> param_writes; // What functions change the parameters
  mutable std::map<uint64_t, bool> writes;
  mutable std::set<uint64_t> writes_open;
  mutable bool writes_guess_used = false;
  std::set<uint32_t> proven; // node Indexations Proven within Boundaries
  std::vector<std::tuple<span, std::string, uint32_t>> proven_list; // (place, text, node)
  std::set<uint32_t> unproven; // node indexation where the index has not been proven
  cell_book book;
  struct loop_state {
    bool may_break = false; // exit from the run-time cycle
  };
  std::vector<loop_state> loops;
  std::vector<fact> facts;
  std::map<fact, uint32_t> fact_ids;
  node_id index_write; // This is the entry of this element.
  symbol sym_intercept, sym_is_index, sym_append, sym_spawn, sym_yield, sym_process; // kernel forms analyzed prior to the calculation of arguments
  std::vector<std::string> instance_keys; // instance in the parsing chain
  std::map<std::string, std::map<std::string, std::pair<span, std::string>>> rt_checks; // instance → run-time - checks
  bool level_probe = false; // This is parsing for static_level.

  std::deque<fiber_rec> fibers; // root 0 is the main
  // at the root of VM (common to processes): scheduler, threads, expectation host
  interpreter * root = this;
  fiber_rec * running_fiber = nullptr;
  std::deque<fiber_rec *> ready;
  uint32_t handoff_run = 0; // direct
  std::vector<fiber_rec *> joiners;
  std::deque<stream_rec> streams;
  std::optional<os::uring> ring; // io_uring host (as required)
  uint32_t io_pending = 0; // nucleus
  std::deque<interpreter> procs;
  std::deque<proc_meta_rec> proc_meta;
  void const * host_bottom = nullptr;
  std::size_t host_size = 0;
  std::deque<interpreter> level_runs;
  std::vector<std::string> level_details;
  std::set<uint32_t> implicit_decls; // x = e, declaring x
  std::map<std::pair<uint32_t, uint64_t>, value> ct_memo; // (ct node, instance ) → value
  std::map<std::pair<uint64_t, uint64_t>, value> stream_shapes; // Place of creation of flow → type of elements (parsing program)
  std::map<uint32_t, std::vector<uint32_t>> resolved_calls; // call node → selected overload (by instance)
  std::vector<std::tuple<span, std::string, std::string>> ct_order; // policeman
  std::set<value const *> ct_cells; // ct-variable module
  std::vector<symbol> ct_names;
  uint32_t ct_depth = 0;
  std::vector<override_rec> overrides;
  uint32_t override_depth = 0;
  uint64_t array_gen = 0; // generation of array buffers (performance)
  std::set<uint32_t> user_ops; // operators declared code (not built-in primitive)
  std::unordered_map<uint64_t, symbol> op_entities;
  std::unordered_map<uint32_t, bool> assoc_ops;
  std::unordered_map<uint32_t, std::vector<symbol>> chain_names; // chain-name
  std::unordered_map<std::string, node_id> resolved_chains; // (chain, types of names) → tree
  std::map<std::pair<uint32_t, uint64_t>, node_id> chain_trees; // (chain, context) → tree (for JIT)
  std::unordered_map<std::string, std::optional<value>> applied; // (operator, types) → type of result
  std::unordered_map<uint32_t, node_id> apply_nodes; // l op r for a sample
  std::deque<interpreter> probes; // parsing chains without side effects
  uint64_t effects = 0;
};

} // namespace tz
