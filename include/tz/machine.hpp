// Process machine: execution without recursion, memory - data on indices (speak
// §9.5, criterion M5: process dump and recovery in another VM).
//
// 
// 
// 
// 
// 
// 
// . Therefore, the state of the process is only data: values are stored by value, areas
// of visibility are stored in the arena by numbers, lambda environments and expr
// parameters are area numbers; frames, task stack and value stack are also data. Dump
// is the recording of these arenas by the same writer as the journal base.
//
// Execution is AST, like an interpreter (without IR), but without C++ recursion:
// computation node is a problem on the task stack (node, phase, area, frame). Static
// decisions (chain tree without priorities, choice of overload, primary operations)
// are taken from the interpreter-assistant: it does not have a process state. What
// the machine doesn't know yet is what it calls a JIT.
#pragma once

#include "interp.hpp"
#include "store.hpp"

namespace tz {

constexpr uint32_t m_none = ~uint32_t{0};

struct mval;
struct m_field;
struct m_unit {};
struct m_type {
  type_id type;
};
struct m_arr {
  std::vector<mval> items;
  uint64_t gen = 0; // Buffer generation: growth and replacement - new (links to elements, §12.1)
};
struct m_agg {
  type_id type;
  std::vector<m_field> fields;
};
// Function as a value: overload by name or lambda by environment (area number).
struct m_fn {
  symbol name;
  node_id lambda;
  uint32_t env = m_none;
  std::vector<node_id> set; // Local set of overloads (block functions of the same name)
};
// expr-параметр: выражение вызывающего в его области и кадре (§6.1).
struct m_thunk {
  node_id expr;
  uint32_t env = m_none;
  uint32_t frame = 0;
  uint64_t ctx = 0; // context
};
// Parameter - alias of the caller's place (author's decision): area and slot. alias
// is a place by number: a variable (scope, slot) or an element of its array (index
// and buffer generation when taking: reference &e = v[i], §12.1).
struct m_alias {
  uint32_t scope = m_none;
  uint32_t slot = 0;
  uint32_t index = m_none;
  uint64_t gen = 0;
};
struct mval {
  std::variant<m_unit, bool, int64_t, double, std::string, m_type, m_arr, m_agg, m_fn, m_thunk, m_alias> v;

  template<typename T>
  bool is() const { return std::holds_alternative<T>(v); }
  template<typename T>
  T & as() { return std::get<T>(v); }
  template<typename T>
  T const & as() const { return std::get<T>(v); }
};
struct m_field {
  symbol name;
  mval v;
};

struct m_var {
  symbol name;
  mval v;
  uint32_t hidden = 0; // Node hide/own +1: name hidden (moved)
};
struct m_scope {
  uint32_t parent = m_none;
  std::vector<m_var> vars;
  bool captured = false; // Lambda environment - lives longer than its block
  bool live = true;
};

struct m_frame {
  uint32_t id;
  symbol name;
  bool outline;
  uint32_t caller;
  bool plain = false; // Context Redefinition Body: Inside - Normal Operations
};
// Redefinition of an operation by context (using operator ..., §8.2) - to the end of the frame of the function-context.
struct m_override {
  symbol entity; // infix+
  node_id fn;
  uint32_t frame;
};

// Task: Calculating node (or service step) with phase and data.
struct m_task {
  uint8_t kind; // m_eval, m_call_body, m_block_body…
  node_id node;
  uint32_t phase = 0;
  uint32_t scope = m_none;
  uint32_t frame = 0;
  uint32_t base = 0; // elevation
  uint32_t owned = m_none; // The area that the task releases at the exit
  uint32_t a = 0, b = 0; // phase-out
  node_id extra; // selected by overload, the chain tree...
  uint64_t ctx = 0; // Embedding context (chain of call places) - key ct-instance
  uint64_t thunk_ctx = 0; // Block/body context of the structure that the task will cause
};
// The result of ct code instance : (ct node, embedding context) → value (§7.3).
struct m_ct {
  node_id node;
  uint64_t ctx;
  mval v;
};

// bootstrapping signal: break/continue/return (with target frame)/throw.
struct m_signal {
  uint8_t kind = 0; // 0 no
  mval v;
  uint32_t target = 0;
};

// Error fiber - data (pop up to main while waiting for fiber).
struct m_trace {
  std::string what;
  span where;
};
struct m_error {
  bool present = false;
  std::string what;
  span where;
  std::string type;
  std::vector<m_trace> trace;
};

enum : uint8_t { f_ready, f_blocked, f_done };

// fiber - its stack of tasks and values, frames: switching - changing the number (§8.3).
struct m_fiber {
  std::vector<m_task> tasks;
  std::vector<mval> vals;
  std::vector<m_frame> frames;
  m_signal pending;
  std::vector<m_override> overrides;
  uint8_t state = f_ready;
  bool direct = false; // In the queue first: awakened by exchange (direct transmission)
  m_error error;
};

// Write/read between fiber (§8.3): A buffer waiting to be read and written.
struct m_stream {
  uint64_t cap = 16;
  std::vector<mval> buf;
  bool closed = false;
  std::vector<uint32_t> readers, writers;
};

// The state of the process is everything that Dump writes.
struct m_state {
  std::vector<m_scope> scopes;
  std::vector<uint32_t> free_scopes;
  std::vector<m_fiber> fibers;
  uint32_t frame_counter = 0;
  uint32_t globals = m_none;
  uint64_t steps = 0;
  std::vector<std::string> type_names; // Numbers of value types - as in the register at the time of dump
  bool done = false;
  int64_t exit_code = 0;
  // Input: given strings (tests, session) or stdin host when the queue is not set
  bool queued_input = false;
  std::vector<std::string> input;
  // fiber : current, queue of ready, waiting for the end of fiber ; streams
  uint32_t current = 0;
  std::vector<uint32_t> ready;
  std::vector<uint32_t> joiners;
  std::vector<m_stream> streams;
  bool deadlock = false; // Everyone is waiting for each other: the mistake will be the main one.
  uint32_t handoff_run = 0; // direct
  uint64_t array_gen = 0; // array-buffering
  std::vector<m_ct> ct_memo; // Canonical order ct: results of parsing of the entire program
  std::vector<symbol> ct_vars; // ct-variable module (change - only ct-code)
};

enum : uint8_t { m_eval, m_call_body, m_lambda_body, m_thunk_eval, m_intercept, m_elem_ref };
enum : uint8_t { sig_none, sig_break, sig_continue, sig_return, sig_throw };

struct process_machine {
  ast_db & db;
  node_id root;
  std::string name;
  interpreter helper; // Static Solutions and Primary Operations: No Process State
  m_state st;
  std::string * captured = nullptr; // output - in line (tests), otherwise stdout

  process_machine(ast_db & db, node_id root, std::string name)
      : db(db), root(root), name(name), helper(db, root, std::vector<std::string>{name}, mode::run) {
    helper.elaborate();
  }

  // --- memory ---

  uint32_t new_scope(uint32_t parent) {
    uint32_t id;
    if(!st.free_scopes.empty()) {
      id = st.free_scopes.back();
      st.free_scopes.pop_back();
      st.scopes[id] = m_scope{parent, {}, false, true};
    } else {
      id = uint32_t(st.scopes.size());
      st.scopes.push_back(m_scope{parent, {}, false, true});
    }
    return id;
  }
  void release_scope(uint32_t id) {
    if(id == m_none || st.scopes[id].captured) return;
    st.scopes[id].vars.clear();
    st.scopes[id].live = false;
    st.free_scopes.push_back(id);
  }
  void capture(uint32_t id) {
    for(; id != m_none && !st.scopes[id].captured; id = st.scopes[id].parent) st.scopes[id].captured = true;
  }

  struct place {
    uint32_t scope = m_none;
    uint32_t slot = 0;
  };
  place find(uint32_t scope, symbol name) const {
    for(uint32_t s = scope; s != m_none; s = st.scopes[s].parent) {
      auto const & vs = st.scopes[s].vars;
      for(uint32_t i = uint32_t(vs.size()); i-- > 0;)
        if(vs[i].name == name) return {s, i};
    }
    return {};
  }
  // Place cell - through alias to the present.
  mval & cell(place p) {
    mval * c = &st.scopes[p.scope].vars[p.slot].v;
    if(c->is<m_alias>()) c = &follow(c->as<m_alias>());
    return *c;
  }
  // Cell alias; reference element - if the support (buffer) has not changed, otherwise
  // a "dangerous-dangerous" exception to invalidated_alias, as with the interpreter.
  mval & follow(m_alias al) {
    mval * c = &st.scopes[al.scope].vars[al.slot].v;
    if(c->is<m_alias>()) c = &follow(c->as<m_alias>());
    if(al.index == m_none) return *c;
    if(!c->is<m_arr>() || c->as<m_arr>().gen != al.gen) {
      diagnostic d("ссылка на элемент использована после инвалидации опоры", here(), diag_kind::runtime);
      d.type = "invalidated_alias";
      throw d;
    }
    auto & xs = c->as<m_arr>().items;
    if(al.index >= xs.size()) out_of_bounds(al.index, xs.size(), here());
    return xs[al.index];
  }
  span here() { return fib().tasks.empty() ? span{} : db[fib().tasks.back().node].where; }
  mval deref(mval const & x) {
    if(x.is<m_alias>()) return follow(x.as<m_alias>());
    return x;
  }
  void declare(uint32_t scope, symbol name, mval v) { st.scopes[scope].vars.push_back({name, std::move(v), 0}); }

  // The place through alias is the variable itself (for “hidden”).
  m_var & var_of(place p) {
    m_var * v = &st.scopes[p.scope].vars[p.slot];
    while(v->v.is<m_alias>()) v = &st.scopes[v->v.as<m_alias>().scope].vars[v->v.as<m_alias>().slot];
    return *v;
  }
  void not_hidden(place p, symbol name, span where) {
    if(uint32_t h = var_of(p).hidden) {
      diagnostic d(cat("имя ", helper.str(name), " скрыто (hide/own)"), where);
      d.note("скрыто здесь", db[node_id(h - 1)].where);
      throw d;
    }
  }

  // --- The value of the interpreter (for primary operations and printing) ---

  value to_value(mval const & x) {
    return std::visit([&](auto const & a) -> value {
      using T = std::remove_cvref_t<decltype(a)>;
      if constexpr(std::is_same_v<T, m_unit>) return value(unit_v{});
      else if constexpr(std::is_same_v<T, m_type>) return value(type_v{a.type});
      else if constexpr(std::is_same_v<T, m_arr>) {
        std::vector<value> xs;
        xs.reserve(a.items.size());
        for(auto const & e: a.items) xs.push_back(to_value(e));
        return value(array_v::of(std::move(xs)));
      } else if constexpr(std::is_same_v<T, m_agg>) {
        agg_v g{a.type, {}};
        for(auto const & f: a.fields) g.fields.push_back({f.name, to_value(f.v)});
        return value(std::move(g));
      } else if constexpr(std::is_same_v<T, m_fn>) return value(fn_v{a.name, a.lambda, nullptr});
      else if constexpr(std::is_same_v<T, m_thunk> || std::is_same_v<T, m_alias>) return to_value(deref(x));
      else return value(a);
    },
        x.v);
  }

  mval from_value(value const & x, span where) {
    return std::visit([&](auto const & a) -> mval {
      using T = std::remove_cvref_t<decltype(a)>;
      if constexpr(std::is_same_v<T, unit_v>) return mval{m_unit{}};
      else if constexpr(std::is_same_v<T, bool> || std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, std::string>) return mval{a};
      else if constexpr(std::is_same_v<T, type_v>) return mval{m_type{a.type}};
      else if constexpr(std::is_same_v<T, array_v>) {
        m_arr r;
        for(uint32_t i = 0; i < a.size(); ++i) r.items.push_back(from_value(a.get(i), where));
        return mval{std::move(r)};
      } else if constexpr(std::is_same_v<T, agg_v>) {
        m_agg g{a.type, {}};
        for(auto const & [k, v]: a.fields) g.fields.push_back({k, from_value(v, where)});
        return mval{std::move(g)};
      } else if constexpr(std::is_same_v<T, fn_v>) return mval{m_fn{a.name, a.lambda, m_none}};
      else throw diagnostic("машина: значение этого вида пока не поддержано", where);
    },
        x.v);
  }

  // The value is transferred to the memory of the process: the whole is known (not run-time, not splitting).
  static bool portable(value const & x) {
    if(x.is<array_v>()) {
      for(uint32_t i = 0; i < x.as<array_v>().size(); ++i)
        if(!portable(x.as<array_v>().get(i))) return false;
      return true;
    }
    if(x.is<agg_v>()) return std::ranges::all_of(x.as<agg_v>().fields, [](auto const & f) { return portable(f.second); });
    return !x.is<thunk_v>() && !x.is<rt_v>() && !x.is<split_v>() && !x.is<ref_v>();
  }

  // Value type - no transfer (to select overload by parameter type).
  std::string describe(mval const & x) {
    return std::visit([&](auto const & a) -> std::string {
      using T = std::remove_cvref_t<decltype(a)>;
      if constexpr(std::is_same_v<T, bool>) return "bool";
      else if constexpr(std::is_same_v<T, int64_t>) return "i64";
      else if constexpr(std::is_same_v<T, double>) return "f64";
      else if constexpr(std::is_same_v<T, std::string>) return "str";
      else if constexpr(std::is_same_v<T, m_unit>) return "unit";
      else if constexpr(std::is_same_v<T, m_type>) return "type";
      else if constexpr(std::is_same_v<T, m_arr>) {
        if(a.items.empty()) return "array<?>";
        std::string first = describe(a.items[0]), all;
        bool same = true;
        for(auto const & e: a.items) {
          std::string d = describe(e);
          same = same && d == first;
          all += (all.empty() ? "" : ", ") + d;
        }
        return same ? cat("array<", first, ">") : cat("tuple<", all, ">"); // literate
      }
      else if constexpr(std::is_same_v<T, m_agg>) return std::string(helper.types.name(a.type));
      else if constexpr(std::is_same_v<T, m_fn>) return "fn";
      else return describe(deref(x));
    },
        x.v);
  }

  bool truthy(mval const & x, span where) {
    mval v = deref(x);
    if(!v.is<bool>()) throw diagnostic(cat("условие должно быть bool, получено ", describe(v)), where);
    return v.as<bool>();
  }

  // --- output ---

  void print(std::vector<mval> const & a) {
    std::string line;
    for(uint32_t i = 0; i < a.size(); ++i) line += (i ? " " : "") + helper.show_v(to_value(a[i]));
    line += '\n';
    if(captured) *captured += line;
    else {
      put(stdout, line);
      std::fflush(stdout);
    }
  }

  // --- task-stack ---

  m_fiber & fib() { return st.fibers[st.current]; }

  // --- fiber scheduler (like interpreter: cooperative, FIFO) ---

  void wake(uint32_t i) {
    if(st.fibers[i].state != f_blocked) return;
    st.fibers[i].state = f_ready;
    st.ready.push_back(i);
  }
  // Direct transmission, as in the interpreter: awakened by the exchange - the next,
  // in a row no more than interpreter::handoff_limit.
  void wake_direct(uint32_t i) {
    if(st.fibers[i].state != f_blocked) return;
    st.fibers[i].state = f_ready;
    if(st.handoff_run >= interpreter::handoff_limit) return st.ready.push_back(i);
    st.fibers[i].direct = true;
    st.ready.insert(st.ready.begin(), i);
  }
  void park() {
    fib().state = f_blocked;
    schedule();
  }
  // The next one is ready; no - everyone is waiting: mutual lock, error - in the main.
  void schedule() {
    while(!st.ready.empty() && st.fibers[st.ready.front()].state != f_ready) st.ready.erase(st.ready.begin());
    if(!st.ready.empty()) {
      st.current = st.ready.front();
      st.ready.erase(st.ready.begin());
      st.handoff_run = st.fibers[st.current].direct ? st.handoff_run + 1 : 0;
      st.fibers[st.current].direct = false;
      return;
    }
    if(fib().state == f_ready) return;
    st.current = 0;
    st.fibers[0].state = f_ready;
    st.deadlock = true;
  }
  void yield() {
    st.ready.push_back(st.current);
    schedule();
  }
  // fiber is over: main waits for the rest; fiber wakes the waiters.
  void fiber_finished() {
    if(st.current == 0) {
      bool alive = false;
      for(uint32_t i = 1; i < st.fibers.size(); ++i) alive = alive || st.fibers[i].state != f_done;
      if(alive) {
        st.joiners.push_back(0);
        return park();
      }
      for(auto const & f: st.fibers)
        if(f.error.present) throw restore_error(f.error);
      st.done = true;
      if(!fib().vals.empty() && deref(fib().vals.back()).is<int64_t>()) st.exit_code = deref(fib().vals.back()).as<int64_t>();
      return;
    }
    fib().state = f_done;
    for(uint32_t j: std::exchange(st.joiners, {})) wake(j);
    schedule();
  }
  static diagnostic restore_error(m_error const & e) {
    diagnostic d(e.what, e.where, diag_kind::runtime);
    d.type = e.type;
    for(auto const & t: e.trace) d.trace.push_back({t.what, t.where});
    return d;
  }
  // The uncaught error fiber (not main) is its outcome; the program lives to wait in the main.
  void fiber_error(diagnostic const & d) {
    auto & f = fib();
    f.error = {true, d.what(), d.where, d.type, {}};
    for(auto const & t: d.trace) f.error.trace.push_back({t.what, t.where});
    f.tasks.clear();
    f.vals.clear();
    f.state = f_done;
    for(uint32_t j: std::exchange(st.joiners, {})) wake(j);
    schedule();
  }

  void push_eval(node_id n, uint32_t scope, uint32_t frame) {
    auto & f = fib();
    f.tasks.push_back(m_task{m_eval, n, 0, scope, frame, uint32_t(f.vals.size())});
    if(f.tasks.size() > 1) f.tasks.back().ctx = f.tasks[f.tasks.size() - 2].ctx;
  }
  // The task is completed with v.
  void finish(mval v) {
    auto & f = fib();
    m_task t = f.tasks.back();
    f.tasks.pop_back();
    release_scope(t.owned);
    f.vals.resize(t.base);
    f.vals.push_back(std::move(v));
  }
  mval pop_val() {
    auto & f = fib();
    mval v = std::move(f.vals.back());
    f.vals.pop_back();
    return v;
  }
  // bootstrapping signal: Tasks are removed until a handler is found.
  void raise(m_signal s) {
    auto & f = fib();
    f.pending = std::move(s);
  }

  [[noreturn]] void out_of_bounds(int64_t k, std::size_t size, span where) {
    diagnostic d(cat("индекс ", k, " вне границ массива размера ", size), where, diag_kind::runtime);
    d.type = "out_of_bounds";
    throw d;
  }

  bool unsupported_hit = false; // "Not supported" is not a program error: try_compile does not silence it
  [[noreturn]] void unsupported(std::string_view what, node_id n) {
    unsupported_hit = true;
    throw diagnostic(cat("машина процессов: пока не поддержано — ", what), db[n].where);
  }

  // --- launch ---

  void start(std::vector<std::string> const & args, std::optional<std::vector<std::string>> input = std::nullopt) {
    st = {};
    if(input) {
      st.queued_input = true;
      st.input = *input;
    }
    st.fibers.emplace_back();
    st.globals = new_scope(m_none);
    // as an interpreter: args is the program name and arguments, stdin is the descriptor
    m_arr av;
    av.items.push_back(mval{name});
    for(auto const & a: args) av.items.push_back(mval{a});
    declare(st.globals, helper.sym("args"), mval{std::move(av)});
    declare(st.globals, helper.sym("stdin"), mval{m_agg{helper.types.named("stream"), {}}});
    // ct-variables of the module and ct-code instance - in canonical order (§7.3): the
    // results of parsing of the entire program assistant become the memory of the process
    if(!helper.ct_names.empty()) helper.canonical_ct();
    for(auto const & [k, v]: helper.ct_memo)
      if(portable(v)) st.ct_memo.push_back({node_id(k.first), k.second, from_value(v, {})});
    for(node_id item: db.as<n_module>(root).items)
      if(db.is<n_var>(item)) {
        auto const & v = db.as<n_var>(item);
        if(!v.ct) continue; // Outside of non-ct functions is parsing error; it doesn't get there
        value * x = helper.globals->find(v.name);
        if(!x) throw diagnostic(cat("ct ", helper.str(v.name), ": нет значения"), db[item].where);
        st.ct_vars.push_back(v.name);
        // the result, depending on the forks (branches give different), is not a value: read it ct- node from the memo
        if(portable(*x)) declare(st.globals, v.name, from_value(*x, db[item].where));
      }
    auto it = helper.functions.find(helper.sym("main"));
    if(it == helper.functions.end()) throw diagnostic("нет функции main", {});
    invoke(it->second, 0, 0, db[it->second.front()].where);
  }

  // Step: One phase of the upper task; false, the process is complete.
  bool step() {
    if(st.done) return false;
    auto & f = fib();
    try {
      if(st.deadlock && st.current == 0) { // Error of mutual blocking - in the main, at the waiting place
        st.deadlock = false;
        span where; // waiting place in the program, not in the wrapper prelude
        for(auto i = f.tasks.rbegin(); i != f.tasks.rend() && !where.end; ++i)
          if(db[i->node].where.source != db[db.prelude].where.source) where = db[i->node].where;
        diagnostic d("все файберы ждут друг друга (взаимная блокировка)", where, diag_kind::runtime);
        d.type = "deadlock";
        throw d;
      }
      if(f.pending.kind != sig_none) {
        unwind();
        return !st.done;
      }
      if(f.tasks.empty()) {
        fiber_finished();
        return !st.done;
      }
    } catch(diagnostic const & d) {
      if(st.current == 0) throw;
      fiber_error(d);
      return true;
    }
    ++st.steps;
    m_task & t = f.tasks.back();
    try {
      switch(t.kind) {
        case m_eval: eval_step(); break;
        case m_call_body: call_body_step(); break;
        case m_lambda_body: lambda_body_step(); break;
        case m_intercept: intercept_step(); break;
        case m_elem_ref: elem_ref_step(); break;
        default: throw diagnostic("машина: неизвестная задача", {});
      }
    } catch(diagnostic const & d) {
      if(d.kind == diag_kind::instantiation && !unsupported_hit && sfinae_cut()) return true;
      // VM Error: Eliminating the language if there is someone to catch it (§5.7)
      if(d.kind != diag_kind::runtime || std::ranges::none_of(fib().tasks, [](m_task const & x) { return x.kind == m_intercept && x.phase == 1; })) {
        if(st.current == 0) throw;
        fiber_error(d);
        return true;
      }
      m_agg e{helper.types.named(d.type.empty() ? "vm_error" : d.type), {}};
      e.fields.push_back({helper.sym("message"), mval{std::string(d.what())}});
      raise({sig_throw, mval{std::move(e)}, 0});
      pending_error = d;
    }
    return true;
  }

  std::optional<diagnostic> pending_error; // Original error VM flying exception (not process state)

  void run(uint64_t max_steps = ~uint64_t{0}) {
    while(max_steps-- > 0 && step()) {}
  }

  // --- раскрутка ------------------------------------------------------------------------------

  void unwind() {
    auto & f = fib();
    m_signal & s = f.pending;
    while(!f.tasks.empty()) {
      m_task & t = f.tasks.back();
      if(t.kind == m_eval && db.is<n_loop>(t.node) && (s.kind == sig_break || s.kind == sig_continue)) {
        f.vals.resize(t.base);
        if(s.kind == sig_break) {
          s.kind = sig_none;
          finish(mval{m_unit{}});
          return;
        }
        s.kind = sig_none;
        t.phase = 0; // Continue - the next iteration
        return;
      }
      if(t.kind == m_intercept && t.phase == 1 && s.kind == sig_throw) {
        if(catch_into(t)) return; // caught: next - catch block
      }
      if(t.kind == m_call_body && s.kind == sig_return && s.target == t.a) {
        mval v = std::move(s.v);
        s.kind = sig_none;
        end_frame(t);
        finish(std::move(v));
        return;
      }
      if(t.kind == m_call_body) end_frame(t);
      release_scope(t.owned);
      f.vals.resize(t.base);
      f.tasks.pop_back();
    }
    if(s.kind == sig_throw) {
      if(pending_error) throw *pending_error; // Error VM - like the interpreter, with its place
      throw diagnostic(cat("непойманное исключение ", exception_type(s.v), ": ", helper.show_v(to_value(s.v))), s.target ? db[node_id(s.target)].where : span{},
          diag_kind::runtime);
    }
    throw diagnostic(s.kind == sig_return ? "return вне функции" : "break/continue вне цикла", {});
  }

  // Error instantiation within try_compile: the block is excluded (§5.4) - the tasks on
  // it are removed, the execution goes on. runtime doesn't silence errors.
  bool sfinae_cut() {
    auto & f = fib();
    auto at = std::ranges::find_if(f.tasks.rbegin(), f.tasks.rend(), [&](m_task const & x) { return x.kind == m_eval && x.phase == 1 && db.is<n_try_compile>(x.node); });
    if(at == f.tasks.rend()) return false;
    std::size_t keep = f.tasks.size() - std::size_t(at - f.tasks.rbegin());
    while(f.tasks.size() > keep) {
      m_task & t = f.tasks.back();
      if(t.kind == m_call_body) end_frame(t);
      release_scope(t.owned);
      f.vals.resize(t.base);
      f.tasks.pop_back();
    }
    f.pending = {};
    f.vals.resize(f.tasks.back().base);
    finish(mval{m_unit{}});
    return true;
  }

  void end_frame(m_task const & t) {
    auto & fr = fib().frames;
    if(!fr.empty() && fr.back().id == t.a) fr.pop_back();
    auto & ov = fib().overrides; // The redefinition of context lives to the end of his frame
    while(!ov.empty() && ov.back().frame == t.a) ov.pop_back();
  }

  // --- staffing ---

  uint32_t return_target(symbol name, uint32_t frame, span where) {
    auto & frames = fib().frames;
    auto at = [&](uint32_t id) -> m_frame const * {
      for(auto it = frames.rbegin(); it != frames.rend(); ++it)
        if(it->id == id) return &*it;
      return nullptr;
    };
    m_frame const * cur = at(frame);
    if(!cur) throw diagnostic("return вне функции", where);
    bool to_caller = helper.str(name) == "caller";
    while(m_frame const * up = cur->caller ? at(cur->caller) : nullptr) {
      if(cur->outline) throw diagnostic(cat("return(", helper.str(name), ") через outline-функцию"), where);
      if(up->name && interpreter::operator_name(helper.str(up->name))) {
        cur = up;
        continue;
      }
      if(to_caller || up->name == name) return up->id;
      cur = up;
    }
    throw diagnostic(cat("return(", helper.str(name), "): нет такой внешней функции в цепочке вызовов"), where);
  }

  // Calling overloads fns with already calculated arguments (top of the value
  // stack: nargs pieces). The choice is by arrogance and types of parameters.
  node_id site_node; // Call Node - to mark "hidden here"
  uint32_t invoke_parent = m_none; // calling a local set of overloads: its environment (takes invoke)
  void invoke(std::vector<node_id> const & fns, uint32_t nargs, uint32_t caller_frame, span where) {
    auto & f = fib();
    std::vector<mval> args(f.vals.end() - nargs, f.vals.end());
    node_id chosen = override_for(fns, args); // Operation in context (checked) - its redefinition
    bool plain = bool(chosen);
    int best = -1;
    bool tie = false;
    uint32_t parent = std::exchange(invoke_parent, m_none); // local set: environment of the set
    uint32_t rejected = 0, by_value = 0; // All are cut off by meaning - an exception, like the interpreter.
    std::vector<std::pair<uint32_t, mval>> chosen_conv;
    for(node_id fnode: fns) {
      auto const & fn = db.as<n_fn>(fnode);
      if(plain) continue;
      if(fn.params.size() != nargs) {
        ++rejected;
        continue;
      }
      int score = 0;
      bool fits = true;
      std::vector<std::pair<uint32_t, mval>> conv; // candidate's converted arguments
      for(uint32_t i = 0; i < nargs && fits; ++i) {
        auto const & p = db.as<n_param>(fn.params[i]);
        if(p.type && db.is<n_pat>(p.type)) { // Applicant’s condition (as required)
          uint32_t probe = new_scope(parent == m_none ? st.globals : parent);
          bool ok = unpack_into(p.type, args[i], probe, where);
          release_scope(probe);
          if(!ok) {
            fits = false;
            ++by_value;
          } else score += 1 + int(helper.pattern_checks(p.type));
          continue;
        }
        if(p.type && db.is<n_ident>(p.type)) {
          if(args[i].is<m_thunk>()) continue;
          mval got;
          int r = type_fit(deref(args[i]), helper.str(db.as<n_ident>(p.type).name), p.strict, got);
          if(!r) {
            fits = false;
            by_value += numeric_pair(deref(args[i]), helper.str(db.as<n_ident>(p.type).name)) && !p.strict;
          } else {
            score += r;
            if(r == 1) conv.push_back({i, got});
          }
        }
      }
      if(!fits) {
        ++rejected;
        continue;
      }
      if(has_leading_require(fn)) { // require at the beginning of the body - Candidate restriction (§5.2)
        int n = requires_hold(fnode, args, where);
        if(n < 0) {
          ++rejected;
          ++by_value;
          continue;
        }
        score += n;
      }
      if(score > best) {
        best = score;
        chosen = fnode;
        chosen_conv = std::move(conv);
        tie = false;
      } else if(score == best) tie = true;
    }
    if(!chosen && by_value && by_value == rejected) {
      diagnostic d(cat("нет перегрузки ", helper.str(db.as<n_fn>(fns.front()).name), " для этих значений"), where, diag_kind::runtime);
      d.type = "unpack_failed";
      throw d;
    }
    if(!chosen) throw diagnostic(cat("нет подходящей перегрузки ", helper.str(db.as<n_fn>(fns.front()).name)), where);
    if(tie) throw diagnostic("неоднозначный вызов", where);
    for(auto & [i, v]: chosen_conv) args[i] = std::move(v); // Values - in the types of parameters selected
    auto const & fn = db.as<n_fn>(chosen);
    if(fn.outline && helper.inline_required(chosen))
      throw diagnostic(cat(helper.str(fn.name), " помечена outline, но использует семантику инлайна"), db[chosen].where);
    for(node_id at: fn.attrs) { // [[inside(g)]] - entity only in blocks g: at the place of call in the text (§7.4 )
      if(!db.is<n_call>(at) || !db.is<n_ident>(db.as<n_call>(at).callee) || helper.str(db.as<n_ident>(db.as<n_call>(at).callee).name) != "inside") continue;
      auto const & c = db.as<n_call>(at);
      if(c.args.size() != 1 || !db.is<n_ident>(c.args[0])) throw diagnostic("[[inside(конструкция)]]", db[at].where);
      symbol g = db.as<n_ident>(c.args[0]).name;
      if(!site_node || !helper.inside_block_of(site_node, g)) throw diagnostic(cat(helper.str(fn.name), " существует только внутри блоков ", helper.str(g)), where);
    }
    // function-operator from one primitive - VM operation
    if(helper.intrinsic_operator(chosen)) {
      std::string prim(helper.str(db.as<n_ident>(db.as<n_call>(fn.body).callee).name));
      f.vals.resize(f.vals.size() - nargs);
      mval r = primitive(prim, args, where);
      f.vals.push_back(std::move(r));
      return;
    }
    uint32_t id = ++st.frame_counter;
    f.frames.push_back({id, fn.name, fn.outline, caller_frame, plain});
    uint32_t sc = new_scope(parent == m_none ? st.globals : parent); // Local - on top of the surroundings of your block
    for(uint32_t i = 0; i < nargs; ++i) {
      auto const & p = db.as<n_param>(fn.params[i]);
      mval a = std::move(args[i]);
      if(a.is<m_alias>() && a.as<m_alias>().index != m_none && (p.mode == param_mode::own || !helper.writes_param(chosen, i))) a = deref(a);
      if(p.mode == param_mode::own && a.is<m_alias>()) { // movement: the meaning goes away, the name of the caller is hidden (scalar - copy)
        m_alias al = a.as<m_alias>();
        a = deref(a);
        if(!a.is<bool>() && !a.is<int64_t>() && !a.is<double>()) var_of({al.scope, al.slot}).hidden = site_node.value + 1;
      }
      if(p.type && db.is<n_pat>(p.type)) unpack_into(p.type, a, sc, db[fn.params[i]].where); // names
      else declare(sc, p.name, std::move(a));
    }
    f.vals.resize(f.vals.size() - nargs);
    uint64_t ctx = f.tasks.empty() ? 0 : f.tasks.back().ctx;
    f.tasks.push_back(m_task{m_call_body, chosen, 0, sc, id, uint32_t(f.vals.size()), sc, id});
    f.tasks.back().ctx = interpreter::inline_context(ctx, site_node ? site_node.value : 0); // body in its context of embedding
  }

  // Leads require the candidate on the values of the arguments: the number performed
  // (specificity), -1 - not fulfilled. Conditions are calculated by the transferred
  // values assistant - this is a calculation without a state of the process.
  int requires_hold(node_id f, std::vector<mval> const & args, span where) {
    auto const & fn = db.as<n_fn>(f);
    scope * probe = helper.new_scope(helper.globals);
    struct drop {
      interpreter & h;
      scope * s;
      ~drop() { h.drop_scope(s); }
    } hold{helper, probe};
    for(uint32_t i = 0; i < fn.params.size(); ++i) {
      if(args[i].is<m_thunk>()) unsupported("require с expr-параметром", f);
      probe->declare(db.as<n_param>(fn.params[i]).name, to_value(args[i]));
    }
    int n = 0;
    for(node_id s: db.as<n_block>(fn.body).items) {
      if(!db.is<n_require>(s)) break;
      ++n;
      auto c = helper.eval(db.as<n_require>(s).cond, probe, 0);
      if(!c.v.is<bool>()) throw diagnostic("require не bool", where);
      if(!c.v.as<bool>()) return -1;
    }
    return n;
  }

  bool has_leading_require(n_fn const & fn) const {
    if(!fn.body || !db.is<n_block>(fn.body)) return false;
    auto const & items = db.as<n_block>(fn.body).items;
    return !items.empty() && db.is<n_require>(items[0]);
  }

  void call_body_step() {
    auto & f = fib();
    m_task & t = f.tasks.back();
    node_id body = db.is<n_fn>(t.node) ? db.as<n_fn>(t.node).body : db.as<n_lambda>(t.node).body;
    if(t.phase == 0) {
      t.phase = 1;
      push_eval(body, t.scope, t.a);
      return;
    }
    mval r = deref(pop_val()); // a value, not a alias, on an area that is now freed up
    end_frame(t);
    finish(std::move(r));
  }

  void lambda_body_step() {
    m_task & t = fib().tasks.back();
    if(t.phase == 0) {
      t.phase = 1;
      push_eval(t.extra, t.scope, t.frame);
      return;
    }
    mval r = deref(pop_val());
    finish(std::move(r));
  }

  // --- Primary operations and embedded ---

  mval primitive(std::string_view op, std::vector<mval> const & a, span where) {
    static constexpr std::pair<std::string_view, std::string_view> bin[] = {{"add", "+"}, {"sub", "-"}, {"mul", "*"}, {"div", "/"},
        {"rem", "%"}, {"eq", "=="}, {"ne", "!="}, {"lt", "<"}, {"gt", ">"}, {"le", "<="}, {"ge", ">="}};
    for(auto const & [name, sym]: bin)
      if(op == name && a.size() == 2) return from_value(helper.binary(sym, to_value(a[0]), to_value(a[1]), where), where);
    if(op == "neg" && a.size() == 1) return from_value(helper.unary("-", to_value(a[0]), where), where);
    if(op == "not" && a.size() == 1) return mval{!truthy(a[0], where)};
    if((op == "and" || op == "or") && a.size() == 2) return mval{op == "and" ? truthy(a[0], where) && truthy(a[1], where) : truthy(a[0], where) || truthy(a[1], where)};
    return builtin(op, a, where);
  }

  mval builtin(std::string_view n, std::vector<mval> const & a, span where) {
    if(n == "print") {
      print(a);
      return mval{m_unit{}};
    }
    if(n == "read_floats") {
      std::string line;
      m_arr r;
      bool have = false;
      if(st.queued_input) {
        if(!st.input.empty()) {
          line = st.input.front();
          st.input.erase(st.input.begin());
          have = true;
        }
      } else have = os::host_stdin().next(line);
      if(have)
        for(auto w: words_of(line)) {
          if(w.starts_with('+')) w.remove_prefix(1);
          double x;
          auto [end, ec] = std::from_chars(w.data(), w.data() + w.size(), x);
          if(ec != std::errc{} || end != w.data() + w.size()) break;
          r.items.push_back(mval{x});
        }
      return mval{std::move(r)};
    }
    if(n == "is_index" && a.size() == 2) {
      mval i = deref(a[0]), xs = deref(a[1]);
      if(i.is<int64_t>() && xs.is<m_arr>()) return mval{i.as<int64_t>() >= 0 && uint64_t(i.as<int64_t>()) < xs.as<m_arr>().items.size()};
    }
    // The rest are pure embedded interpreters (range, fill, f64, i64, text...)
    std::vector<value> vs;
    for(auto const & x: a) vs.push_back(to_value(x));
    return from_value(helper.builtin(helper.sym(n), vs, where), where);
  }

  // Computation of nodes ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

  void eval_step() {
    m_task & t = fib().tasks.back();
    node_id n = t.node;
    std::visit([&](auto const & k) { eval_node(n, k); }, db[n].kind);
  }

  m_task & top() { return fib().tasks.back(); }

  void eval_node(node_id, n_int const & x) { finish(mval{x.value}); }
  void eval_node(node_id, n_float const & x) { finish(mval{x.value}); }
  void eval_node(node_id, n_str const & x) { finish(mval{x.value}); }

  void eval_node(node_id n, n_ident const & x) {
    m_task & t = top();
    place p = find(t.scope, x.name);
    if(p.scope == m_none) {
      std::string_view s = helper.str(x.name);
      if(s == "true" || s == "false") return finish(mval{s == "true"});
      if(auto ty = helper.types.builtin(s)) return finish(mval{m_type{ty}});
      if(helper.structs.contains(x.name)) return finish(mval{m_type{helper.types.named(s)}}); // entity struct - value
      if(auto it = helper.functions.find(x.name); it != helper.functions.end()) {
        for(node_id f: it->second)
          if(helper.inline_required(f))
            throw diagnostic(cat("функция ", s, " использует семантику инлайна (expr-параметр или return(…)) и не может быть значением; вызывайте её напрямую"), db[n].where);
        return finish(mval{m_fn{x.name, {}, m_none}});
      }
      throw diagnostic(cat("неизвестное имя ", s), db[n].where);
    }
    not_hidden(p, x.name, db[n].where);
    mval & c = cell(p);
    if(c.is<m_thunk>()) { // expr-parameter - the expression of the caller in its area and frame
      m_thunk th = c.as<m_thunk>();
      if(db.is<n_lambda>(th.expr) && db.as<n_lambda>(th.expr).block)
        throw diagnostic("блок конструкции вводит имена — его вызывают с аргументами: body(x)", db[th.expr].where);
      t.kind = m_lambda_body; // The result of the expression is the result of this node
      t.extra = th.expr;
      t.scope = th.env;
      t.frame = th.frame;
      t.ctx = th.ctx;
      t.phase = 0;
      return;
    }
    // alias is given: the call will see the location of the caller.
    finish(mval{m_alias{p.scope, p.slot}});
  }

  void eval_node(node_id n, n_block const & b) {
    m_task & t = top();
    if(t.phase == 0) {
      t.owned = new_scope(t.scope);
      t.scope = t.owned;
    } else if(!(b.has_value && t.phase == b.items.size())) {
      fib().vals.resize(t.base); // The meaning of the instruction is not
    }
    if(t.phase < b.items.size()) {
      node_id item = b.items[t.phase++];
      push_eval(item, t.scope, t.frame);
      return;
    }
    if(b.has_value && !b.items.empty()) {
      mval v = deref(pop_val());
      return finish(std::move(v));
    }
    (void)n;
    finish(mval{m_unit{}});
  }

  // --- unpacking (P12), as in the interpreter; value-element - literal ---

  // Levels of coincidence of type, as in the interpreter: 2 - type is equal, 1 - value
  // is imaginable (out - in the type of parameter), 0 - no.
  int type_fit(mval const & v, std::string_view want, bool strict, mval & out) {
    out = v;
    std::string have = describe(v);
    if(want == "auto" || have == want || have.substr(0, have.find('<')) == want) return 2;
    if(strict || !numeric_pair(v, want)) return 0;
    if(want == "f64") {
      int64_t i = v.as<int64_t>();
      double d = double(i);
      if(d < -9.2e18 || d > 9.2e18 || int64_t(d) != i) return 0;
      out = mval{d};
      return 1;
    }
    double d = v.as<double>();
    if(!(d >= -9.2e18 && d <= 9.2e18) || std::trunc(d) != d) return 0;
    out = mval{int64_t(d)};
    return 1;
  }
  static bool numeric_pair(mval const & v, std::string_view want) { return (want == "f64" && v.is<int64_t>()) || (want == "i64" && v.is<double>()); }

  // Reference to the interface from the unpackable value: .field, .size, [literal]; failed
  // (no field, border) - nullopt: not unpacked.
  std::optional<mval> access(node_id e, mval const & subject) {
    if(db.is<n_subject>(e)) return subject;
    if(db.is<n_member>(e)) {
      auto const & m = db.as<n_member>(e);
      auto o = access(m.object, subject);
      if(!o) return std::nullopt;
      mval x = deref(*o);
      if(x.is<m_arr>() && helper.str(m.name) == "size") return mval{int64_t(x.as<m_arr>().items.size())};
      if(x.is<m_agg>())
        for(auto const & f: x.as<m_agg>().fields)
          if(f.name == m.name) return f.v;
      return std::nullopt;
    }
    if(db.is<n_index>(e) && db.is<n_int>(db.as<n_index>(e).index)) {
      auto o = access(db.as<n_index>(e).object, subject);
      if(!o) return std::nullopt;
      mval x = deref(*o);
      int64_t k = db.as<n_int>(db.as<n_index>(e).index).value;
      if(!x.is<m_arr>() || k < 0 || uint64_t(k) >= x.as<m_arr>().items.size()) return std::nullopt;
      return x.as<m_arr>().items[std::size_t(k)];
    }
    unsupported("обращение образца (вызов, индекс-выражение)", e);
  }

  bool unpack_into(node_id pat, mval const & v0, uint32_t into, span where) {
    auto const & p = db.as<n_pat>(pat);
    std::string_view kind = helper.str(db.as<n_ident>(p.type).name);
    mval v = deref(v0);
    auto elem = [&](node_id item, mval const & e) -> bool {
      if(db.is<n_pat>(item)) return unpack_into(item, e, into, where);
      if(db.is<n_param>(item)) {
        auto const & b = db.as<n_param>(item);
        mval got = e;
        if(b.type && !type_fit(deref(e), helper.str(db.as<n_ident>(b.type).name), b.strict, got)) return false;
        if(helper.str(b.name) != "_") declare(into, b.name, got);
        return true;
      }
      mval lit;
      if(db.is<n_int>(item)) lit = mval{db.as<n_int>(item).value};
      else if(db.is<n_float>(item)) lit = mval{db.as<n_float>(item).value};
      else if(db.is<n_str>(item)) lit = mval{db.as<n_str>(item).value};
      else unsupported("значение-элемент образца — не литерал", item);
      mval eq = primitive("eq", {e, lit}, db[item].where);
      return eq.is<bool>() && eq.as<bool>();
    };
    node_list pos, fields;
    for(node_id it: p.items) (db.is<n_pat_field>(it) ? fields : pos).push_back(it);
    auto interface = [&] {
      for(node_id it: fields) {
        auto got = access(db.as<n_pat_field>(it).access, v);
        if(!got || !elem(db.as<n_pat_field>(it).sub, deref(*got))) return false;
      }
      return true;
    };
    if(kind == "array") {
      if(!v.is<m_arr>()) return false;
      auto const & a = v.as<m_arr>().items;
      std::size_t rest = pos.size();
      for(std::size_t k = 0; k < pos.size(); ++k)
        if(db.is<n_param>(pos[k]) && db.as<n_param>(pos[k]).pack) {
          if(rest != pos.size()) throw diagnostic("в образце — один ...остаток", where);
          rest = k;
        }
      bool open = rest < pos.size() || (pos.empty() && !fields.empty());
      std::size_t fixed = pos.size() - (rest < pos.size() ? 1 : 0);
      if(open ? a.size() < fixed : a.size() != fixed) return false;
      std::size_t tail = rest < pos.size() ? pos.size() - rest - 1 : 0;
      for(std::size_t k = 0; k < pos.size(); ++k) {
        if(k == rest) {
          m_arr part;
          for(std::size_t j = k; j + tail < a.size(); ++j) part.items.push_back(a[j]);
          if(helper.str(db.as<n_param>(pos[k]).name) != "_") declare(into, db.as<n_param>(pos[k]).name, mval{std::move(part)});
          continue;
        }
        std::size_t at = k < rest ? k : a.size() - (pos.size() - k);
        if(!elem(pos[k], a[at])) return false;
      }
      return interface();
    }
    if(helper.structs.contains(db.as<n_ident>(p.type).name)) {
      if(!v.is<m_agg>() || helper.types.name(v.as<m_agg>().type) != kind) return false;
      auto const & fs = v.as<m_agg>().fields;
      if(!pos.empty() && fs.size() != pos.size()) throw diagnostic(cat("образец ", kind, "{…}: полей ", fs.size(), ", элементов ", pos.size()), where);
      for(std::size_t k = 0; k < pos.size(); ++k) {
        if(db.is<n_param>(pos[k]) && db.as<n_param>(pos[k]).pack) throw diagnostic("...остаток — только в array{…}", db[pos[k]].where);
        if(!elem(pos[k], fs[k].v)) return false;
      }
      return interface();
    }
    if(!pos.empty()) throw diagnostic(cat("образец ", kind, "{…}: не struct и не array — только обращения .f: подобразец"), db[p.type].where);
    std::string have = describe(v);
    if(have != kind && have.substr(0, have.find('<')) != kind) return false;
    return interface();
  }

  // sample = value; - names in the current area, not unpacked - exception
  void eval_node(node_id n, n_unpack const & u) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(u.value, t.scope, t.frame);
    }
    mval v = deref(pop_val());
    bool ok = unpack_into(u.pattern, v, t.scope, db[n].where);
    if(u.test) return finish(mval{ok}); // predicate
    if(!ok) {
      diagnostic d(cat("распаковка не удалась: ", printer(db).print(u.pattern), " ← ", helper.show_v(to_value(v))), db[n].where, diag_kind::runtime);
      d.type = "unpack_failed";
      throw d;
    }
    finish(mval{m_unit{}});
  }

  void eval_node(node_id n, n_if const & i) {
    m_task & t = top();
    if(t.phase == 0) { // Unpacking names in condition - in its area visible only then
      t.phase = 1;
      t.a = helper.has_unpack(i.cond) ? new_scope(t.scope) : m_none;
      t.owned = t.a; // release the end of if or bootstrapping
      return push_eval(i.cond, t.a == m_none ? t.scope : t.a, t.frame);
    }
    if(t.phase == 1) {
      uint32_t then_scope = t.a == m_none ? t.scope : t.a;
      bool c = truthy(pop_val(), db[i.cond].where);
      m_task & tt = top();
      tt.phase = 2;
      node_id next = c ? i.then_ : i.else_;
      if(!next) return finish(mval{m_unit{}});
      return push_eval(next, c ? then_scope : tt.scope, tt.frame);
    }
    (void)n;
    mval v = deref(pop_val());
    finish(std::move(v));
  }

  void eval_node(node_id, n_loop const & l) {
    m_task & t = top();
    fib().vals.resize(t.base);
    t.phase = 1;
    push_eval(l.body, t.scope, t.frame);
  }

  void eval_node(node_id, n_break const &) { raise({sig_break, {}, 0}); }
  void eval_node(node_id, n_continue const &) { raise({sig_continue, {}, 0}); }

  void eval_node(node_id n, n_return const & r) {
    m_task & t = top();
    if(r.value && t.phase == 0) {
      t.phase = 1;
      return push_eval(r.value, t.scope, t.frame);
    }
    mval v = r.value ? deref(pop_val()) : mval{m_unit{}};
    uint32_t target = r.target ? return_target(db.as<n_ident>(r.target).name, t.frame, db[n].where) : t.frame;
    raise({sig_return, std::move(v), target});
  }

  void eval_node(node_id n, n_member const & m) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(m.object, t.scope, t.frame);
    }
    mval o = deref(pop_val());
    if(helper.str(m.name) == "size") {
      if(o.is<m_arr>()) return finish(mval{int64_t(o.as<m_arr>().items.size())});
      if(o.is<std::string>()) return finish(mval{int64_t(o.as<std::string>().size())});
    }
    if(o.is<m_agg>())
      for(auto const & f: o.as<m_agg>().fields)
        if(f.name == m.name) return finish(f.v);
    throw diagnostic(cat("у значения типа ", describe(o), " нет поля ", helper.str(m.name)), db[n].where);
  }

  // v.[f] is a field by descriptor (from fields(T)) or by line name (§7.4).
  void eval_node(node_id n, n_field const & fl) {
    m_task & t = top();
    if(t.phase < 2) {
      node_id next = t.phase == 0 ? fl.key : fl.object;
      ++t.phase;
      return push_eval(next, t.scope, t.frame);
    }
    mval o = deref(pop_val()), key = deref(pop_val());
    std::string name;
    if(key.is<std::string>()) name = key.as<std::string>();
    else if(key.is<m_agg>() && !key.as<m_agg>().fields.empty() && key.as<m_agg>().fields[0].v.is<std::string>()) name = key.as<m_agg>().fields[0].v.as<std::string>();
    else throw diagnostic(cat("v.[f]: дескриптор поля должен быть известен при компиляции, получено ", describe(key)), db[n].where);
    if(o.is<m_agg>())
      for(auto const & f: o.as<m_agg>().fields)
        if(helper.str(f.name) == name) return finish(f.v);
    throw diagnostic(cat("у значения типа ", describe(o), " нет поля ", name), db[n].where);
  }

  // Lambda value: The circuit holds the number of its area (it survives the block).
  void eval_node(node_id n, n_lambda const & l) {
    if(l.block) unsupported("блок конструкции как значение", n);
    uint32_t env = top().scope;
    capture(env);
    finish(mval{m_fn{symbol{}, n, env}});
  }

  void eval_node(node_id n, n_index const & ix) {
    m_task & t = top();
    if(t.phase < 2) {
      node_id next = t.phase == 0 ? ix.object : ix.index;
      ++t.phase;
      return push_eval(next, t.scope, t.frame);
    }
    mval i = deref(pop_val());
    mval o = deref(pop_val());
    if(!i.is<int64_t>()) throw diagnostic("индекс должен быть i64", db[n].where);
    if(!o.is<m_arr>()) throw diagnostic(cat("индексация значения типа ", describe(o)), db[n].where);
    auto const & xs = o.as<m_arr>().items;
    int64_t k = i.as<int64_t>();
    if(k < 0 || uint64_t(k) >= xs.size()) out_of_bounds(k, xs.size(), db[n].where);
    finish(xs[uint64_t(k)]);
  }

  void eval_node(node_id n, n_op const & o) {
    m_task & t = top();
    std::string_view op = helper.str(o.op);
    bool assign = op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=";
    if(assign) return eval_assign(n, o, op);
    bool logic = (op == "&&" || op == "||") && o.args.size() == 2;
    if(!logic && t.phase > 0) return resume_call(n);
    if((op == "&&" || op == "||") && o.args.size() == 2) {
      if(t.phase == 0) {
        t.phase = 1;
        return push_eval(o.args[0], t.scope, t.frame);
      }
      if(t.phase == 1) {
        bool a = truthy(pop_val(), db[o.args[0]].where);
        if(a == (op == "||")) return finish(mval{a});
        t.phase = 2;
        return push_eval(o.args[1], t.scope, t.frame);
      }
      return finish(mval{truthy(pop_val(), db[o.args[1]].where)});
    }
    // operator entity prelude (infix+, prefix-...): its overload
    std::string entity = cat(o.args.size() == 2 ? "infix" : o.postfix ? "postfix" : "prefix", op);
    auto it = helper.functions.find(helper.sym(entity));
    if(it == helper.functions.end()) throw diagnostic(cat("оператор ", op, " не определён"), db[n].where);
    return eval_call_args(n, it->second, o.args, true);
  }

  void eval_assign(node_id n, n_op const & o, std::string_view op) {
    m_task & t = top();
    node_id lhs = o.args[0];
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(o.args[1], t.scope, t.frame);
    }
    if(db.is<n_ident>(lhs)) {
      mval rhs = deref(pop_val());
      symbol name = db.as<n_ident>(lhs).name;
      place p = find(t.scope, name);
      if(p.scope == m_none) {
        if(op != "=") throw diagnostic(cat("неизвестное имя ", helper.str(name)), db[lhs].where);
        declare(t.scope, name, std::move(rhs)); // x = e is an announcement if there is no name.
        return finish(mval{m_unit{}});
      }
      if(p.scope == st.globals && std::ranges::contains(st.ct_vars, name) && !in_ct())
        throw diagnostic("ct-переменная меняется только кодом при компиляции (ct { … })", db[n].where);
      mval & c = cell(p);
      bool whole = !st.scopes[p.scope].vars[p.slot].v.is<m_alias>() || st.scopes[p.scope].vars[p.slot].v.as<m_alias>().index == m_none;
      c = op == "=" ? std::move(rhs) : primitive(prim_of(op), {c, rhs}, db[n].where);
      if(whole && c.is<m_arr>()) c.as<m_arr>().gen = ++st.array_gen; // New buffer - old links are invalid
      return finish(mval{m_unit{}});
    }
    if(db.is<n_index>(lhs) && db.is<n_ident>(db.as<n_index>(lhs).object)) { // xs[i] = e
      auto const & ix = db.as<n_index>(lhs);
      if(t.phase == 1) {
        t.phase = 2;
        return push_eval(ix.index, t.scope, t.frame);
      }
      mval i = deref(pop_val());
      mval rhs = deref(pop_val());
      place p = find(t.scope, db.as<n_ident>(ix.object).name);
      if(p.scope == m_none) throw diagnostic("присваивание не в переменную", db[lhs].where);
      mval & arr = cell(p);
      if(!arr.is<m_arr>() || !i.is<int64_t>()) throw diagnostic("запись в элемент не массива", db[lhs].where);
      auto & xs = arr.as<m_arr>().items;
      int64_t k = i.as<int64_t>();
      if(k < 0 || uint64_t(k) >= xs.size()) out_of_bounds(k, xs.size(), db[lhs].where);
      xs[uint64_t(k)] = op == "=" ? std::move(rhs) : primitive(prim_of(op), {xs[uint64_t(k)], rhs}, db[n].where);
      return finish(mval{m_unit{}});
    }
    // a.b.c = e is the aggregate field in the variable (chain of fields on behalf)
    if(db.is<n_member>(lhs)) {
      std::vector<symbol> path;
      node_id x = lhs;
      while(db.is<n_member>(x)) {
        path.push_back(db.as<n_member>(x).name);
        x = db.as<n_member>(x).object;
      }
      if(db.is<n_ident>(x)) {
        mval rhs = deref(pop_val());
        place p = find(t.scope, db.as<n_ident>(x).name);
        if(p.scope == m_none) throw diagnostic("присваивание не в переменную", db[lhs].where);
        not_hidden(p, db.as<n_ident>(x).name, db[lhs].where);
        mval * target = &cell(p);
        for(auto it = path.rbegin(); it != path.rend(); ++it) {
          if(!target->is<m_agg>()) throw diagnostic(cat("у значения типа ", describe(*target), " нет поля ", helper.str(*it)), db[lhs].where);
          mval * next = nullptr;
          for(auto & f: target->as<m_agg>().fields)
            if(f.name == *it) next = &f.v;
          if(!next) throw diagnostic(cat("у значения типа ", describe(*target), " нет поля ", helper.str(*it)), db[lhs].where);
          target = next;
        }
        *target = op == "=" ? std::move(rhs) : primitive(prim_of(op), {*target, rhs}, db[n].where);
        return finish(mval{m_unit{}});
      }
    }
    unsupported("присваивание в это место", lhs);
  }

  static std::string_view prim_of(std::string_view op) {
    return op == "+=" ? "add" : op == "-=" ? "sub" : op == "*=" ? "mul" : "div";
  }

  void eval_node(node_id n, n_chain const & c) {
    m_task & t = top();
    if(t.phase == 0) { // chain tree - according to the types of operands (solution parsing, "combination only")
      scope * probe = helper.new_scope(helper.globals);
      struct drop {
        interpreter & h;
        scope * s;
        ~drop() { h.drop_scope(s); }
      } hold{helper, probe};
      std::vector<symbol> names;
      for(node_id a: c.atoms) helper.free_names(a, names);
      for(symbol s: names) {
        place p = find(t.scope, s);
        if(p.scope == m_none) continue;
        mval v = cell(p);
        if(v.is<m_thunk>()) unsupported("цепочка с expr-параметром", n);
        probe->declare(s, to_value(v));
      }
      node_id tree = helper.resolve_chain(n, c, probe);
      if(!tree) throw diagnostic("цепочка: дерево не выбрано", db[n].where);
      t.phase = 1;
      return push_eval(tree, t.scope, t.frame);
    }
    mval v = deref(pop_val());
    finish(std::move(v));
  }

  // Challenge: arguments (expr-parameter - expression of the caller), then overload.
  void eval_node(node_id n, n_call const & c) {
    m_task & t = top();
    if(t.phase == 0) {
      if(db.is<n_member>(c.callee)) { // a.f(b) ≡ f(a, b) (§4.5)
        auto const & m = db.as<n_member>(c.callee);
        node_list args{m.object};
        args.insert(args.end(), c.args.begin(), c.args.end());
        auto it = helper.functions.find(m.name);
        if(it == helper.functions.end()) return builtin_call(n, m.name, args);
        return eval_call_args(n, it->second, args, false);
      }
      if(!db.is<n_ident>(c.callee)) unsupported("вызов значения-выражения", n);
      symbol name = db.as<n_ident>(c.callee).name;
      place p = find(t.scope, name);
      if(p.scope != m_none) {
        mval & cv = cell(p);
        if(cv.is<m_thunk>() && db.is<n_lambda>(cv.as<m_thunk>().expr) && db.as<n_lambda>(cv.as<m_thunk>().expr).block)
          return call_block(n, cv.as<m_thunk>(), c.args);
        if(cv.is<m_fn>() && !cv.as<m_fn>().set.empty()) { // localization
          std::vector<node_id> fns = cv.as<m_fn>().set;
          return eval_call_args(n, fns, c.args, false);
        }
        if(cv.is<m_fn>() && cv.as<m_fn>().lambda) return call_closure(n, cv.as<m_fn>(), c.args);
        if(cv.is<m_fn>()) {
          auto it = helper.functions.find(cv.as<m_fn>().name);
          if(it != helper.functions.end()) return eval_call_args(n, it->second, c.args, false);
        }
        unsupported("вызов значения", n);
      }
      auto it = helper.functions.find(name);
      if(it == helper.functions.end() && helper.str(name) == "intercept") return begin_intercept(n, c.args);
      if(it == helper.functions.end() && helper.str(name) == "fiber_start") return begin_spawn(n, c.args);
      if(it == helper.functions.end() && helper.str(name) == "yield" && c.args.empty()) {
        finish(mval{m_unit{}});
        yield();
        return;
      }
      if(it == helper.functions.end()) return builtin_call(n, name, c.args);
      return eval_call_args(n, it->second, c.args, false);
    }
    resume_call(n);
  }

  // Arguments for congestion fns: t.extra - overload by the name of the first, t.a - number of arguments.
  void eval_call_args(node_id n, std::vector<node_id> const & fns, node_list const & args, bool op) {
    m_task & t = top();
    (void)op;
    t.extra = fns.front();
    t.a = uint32_t(args.size());
    t.b = 0; // How many arguments are computed
    t.phase = 1;
    call_args_step(n);
  }

  node_list call_args(node_id n) const {
    if(db.is<n_op>(n)) return db.as<n_op>(n).args;
    auto const & c = db.as<n_call>(n);
    if(db.is<n_member>(c.callee)) {
      node_list args{db.as<n_member>(c.callee).object};
      args.insert(args.end(), c.args.begin(), c.args.end());
      return args;
    }
    return c.args;
  }

  // overload of the call n: local set (from the caller area) or module function; env is the
  // local set environment (m_none is the global area).
  std::vector<node_id> overloads_of(node_id n, node_id any, uint32_t scope, uint32_t * env = nullptr) {
    if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee))
      if(place p = find(scope, db.as<n_ident>(db.as<n_call>(n).callee).name); p.scope != m_none)
        if(mval & cv = cell(p); cv.is<m_fn>() && !cv.as<m_fn>().set.empty()) {
          if(env) *env = cv.as<m_fn>().env;
          return cv.as<m_fn>().set;
        }
    return helper.functions.at(db.as<n_fn>(any).name);
  }

  void call_args_step(node_id n) {
    m_task & t = top();
    node_list args = call_args(n);
    uint32_t local = m_none;
    std::vector<node_id> fns = overloads_of(n, t.extra, t.scope, &local);
    if(t.phase == 2) { // challenge
      mval r = pop_val();
      return finish(deref(r));
    }
    while(t.b < t.a) {
      uint32_t i = t.b++;
      bool expr_everywhere = true;
      for(node_id f: fns) {
        auto const & ps = db.as<n_fn>(f).params;
        if(i >= ps.size() || db.as<n_param>(ps[i]).mode != param_mode::expr) expr_everywhere = false;
      }
      if(expr_everywhere) {
        fib().vals.push_back(mval{m_thunk{args[i], t.scope, t.frame, t.ctx}});
        continue;
      }
      // array element to variable parameter - reference to element, not copy
      bool writes = false;
      for(node_id f: fns) {
        auto const & ps = db.as<n_fn>(f).params;
        writes = writes || (i < ps.size() && db.as<n_param>(ps[i]).mode != param_mode::expr && db.as<n_param>(ps[i]).mode != param_mode::own && helper.writes_param(f, i));
      }
      if(writes && elem_place(db, args[i])) return push_elem_ref(args[i], t.scope, t.frame);
      return push_eval(args[i], t.scope, t.frame); // the value (or alias of the place) on the stack
    }
    t.phase = 2;
    uint32_t frame = t.frame;
    site_node = n;
    invoke_parent = local;
    invoke(fns, t.a, frame, db[n].where);
  }

  // Built-in VM function: arguments are computed, then operation.
  void builtin_call(node_id n, symbol name, node_list const & args) {
    m_task & t = top();
    t.extra = node_id{};
    t.a = uint32_t(args.size());
    t.b = name.value;
    t.phase = 3;
    builtin_args_step(n);
  }
  void builtin_args_step(node_id n) {
    m_task & t = top();
    uint32_t have = uint32_t(fib().vals.size() - t.base);
    node_list args = call_args(n);
    if(have < t.a) return push_eval(args[have], t.scope, t.frame);
    symbol name(t.b);
    std::vector<mval> a;
    for(uint32_t i = 0; i < t.a; ++i) a.push_back(deref(fib().vals[t.base + i]));
    if(helper.str(name) == "append" && t.a == 2 && fib().vals[t.base].is<m_alias>()) { // push: the place of the caller grows
      m_alias al = fib().vals[t.base].as<m_alias>();
      mval & xs = follow(al);
      if(!xs.is<m_arr>()) throw diagnostic("append: не массив", db[n].where);
      xs.as<m_arr>().items.push_back(a[1]);
      xs.as<m_arr>().gen = ++st.array_gen; // growth: support disabled
      return finish(mval{m_unit{}});
    }
    if(helper.str(name).starts_with("stream_")) { // Can block: the task will remain and repeat
      mval out;
      if(!stream_op(helper.str(name), a, db[n].where, out)) return park();
      return finish(std::move(out));
    }
    if(helper.str(name).starts_with("net_") || helper.str(name).starts_with("file_") || helper.str(name) == "process")
      unsupported("сеть, файлы и процессы ОС (ресурсы хоста в дамп не входят)", n);
    finish(primitive(helper.str(name), a, db[n].where));
  }

  // Construction block: body(x) - parameters in the new area on top of the block environment,
  // the body - in the frame of the one who wrote the construction (semantic inlining).
  void call_block(node_id n, m_thunk th, node_list const & args) {
    m_task & t = top();
    auto const & l = db.as<n_lambda>(th.expr);
    if(l.params.size() != args.size()) throw diagnostic(cat("блок ждёт ", l.params.size(), " аргументов, передано ", args.size()), db[n].where);
    t.extra = th.expr;
    t.a = th.env;
    t.b = th.frame;
    t.thunk_ctx = th.ctx;
    t.phase = 4;
    block_args_step(n);
  }
  void block_args_step(node_id n) {
    m_task & t = top();
    auto const & l = db.as<n_lambda>(t.extra);
    auto const & args = db.as<n_call>(n).args;
    uint32_t have = uint32_t(fib().vals.size() - t.base);
    if(t.phase == 5) {
      mval r = pop_val();
      return finish(deref(r));
    }
    if(have < args.size()) return push_eval(args[have], t.scope, t.frame);
    uint32_t sc = new_scope(t.a);
    for(uint32_t i = 0; i < args.size(); ++i) declare(sc, db.as<n_param>(l.params[i]).name, deref(fib().vals[t.base + i]));
    fib().vals.resize(t.base);
    t.phase = 5;
    t.owned = sc;
    uint64_t ctx = t.thunk_ctx;
    fib().tasks.push_back(m_task{m_lambda_body, t.extra, 0, sc, t.b, uint32_t(fib().vals.size()), m_none, 0, 0, l.body, ctx});
  }

  // throw e is an expression: the exception goes the second way before the interception (§5.7).
  void eval_node(node_id n, n_throw const & th) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(th.value, t.scope, t.frame);
    }
    mval v = deref(pop_val());
    pending_error.reset();
    raise({sig_throw, std::move(v), n.value});
  }

  std::string exception_type(mval const & v) {
    mval x = deref(v);
    if(x.is<m_agg>()) return std::string(helper.types.name(x.as<m_agg>().type));
    return describe(x);
  }

  // intercept(тело, блоки): try { … } catch(E e) { … } prelude. The body is in the frame of its
  // author; the throw is the first block whose parameter takes the type. t.extra is the expression
  // of the body, t.a is the area, t.b is the frame; blocks are m_thunk in the value stack.
  m_thunk thunk_named(node_id arg, uint32_t scope) {
    if(!db.is<n_ident>(arg)) throw diagnostic("ожидался expr-параметр", db[arg].where);
    place p = find(scope, db.as<n_ident>(arg).name);
    if(p.scope == m_none || !cell(p).is<m_thunk>()) throw diagnostic("ожидался expr-параметр", db[arg].where);
    m_thunk th = cell(p).as<m_thunk>();
    while(db.is<n_ident>(th.expr)) { // A block passed on to another structure, too.
      place q = find(th.env, db.as<n_ident>(th.expr).name);
      if(q.scope == m_none || !cell(q).is<m_thunk>()) break;
      th = cell(q).as<m_thunk>();
    }
    return th;
  }

  void begin_intercept(node_id n, node_list const & args) {
    m_task & t = top();
    if(args.size() != 2) throw diagnostic("intercept(тело, блоки)", db[n].where);
    m_thunk body = thunk_named(args[0], t.scope), hs = thunk_named(args[1], t.scope);
    t.kind = m_intercept;
    t.phase = 0;
    fib().vals.resize(t.base);
    fib().vals.push_back(mval{hs});
    t.extra = body.expr;
    t.a = body.env;
    t.thunk_ctx = body.ctx;
    t.b = body.frame;
  }

  void intercept_step() {
    m_task & t = top();
    if(t.phase == 0) { // body
      t.phase = 1;
      auto & f = fib();
      uint64_t ctx = t.thunk_ctx;
      f.tasks.push_back(m_task{m_lambda_body, t.extra, 0, t.a, t.b, uint32_t(f.vals.size()), m_none, 0, 0, t.extra, ctx});
      return;
    }
    mval r = deref(pop_val()); // body or catch completed
    finish(std::move(r));
  }

  // The throw went to intercept: the appropriate catch block is a task, otherwise bootstrapping is further away.
  bool catch_into(m_task & t) {
    auto & f = fib();
    m_thunk hs = f.vals[t.base].as<m_thunk>();
    node_list handlers = db.is<n_pack>(hs.expr) ? db.as<n_pack>(hs.expr).items : node_list{hs.expr};
    std::string type = exception_type(f.pending.v);
    for(node_id h: handlers) {
      if(!db.is<n_lambda>(h) || db.as<n_lambda>(h).params.size() != 1) throw diagnostic("catch: блок с одним параметром — catch(E e) { … }", db[h].where);
      auto const & l = db.as<n_lambda>(h);
      auto const & p = db.as<n_param>(l.params[0]);
      if(p.type && db.is<n_ident>(p.type) && helper.str(db.as<n_ident>(p.type).name) != type) continue;
      uint32_t sc = new_scope(hs.env);
      if(helper.str(p.name) != "_") declare(sc, p.name, std::move(f.pending.v));
      f.pending = {};
      pending_error.reset();
      f.vals.resize(t.base + 1);
      t.phase = 2;
      t.owned = sc;
      f.tasks.push_back(m_task{m_lambda_body, h, 0, sc, hs.frame, uint32_t(f.vals.size()), m_none, 0, 0, l.body, hs.ctx});
      return true;
    }
    return false;
  }

  // spawn { … } prelude → fiber_start(тело): the new fiber at the end of the queue;
  // the body is closed (its area lives beyond the call).
  void begin_spawn(node_id n, node_list const & args) {
    if(args.size() != 1) throw diagnostic("spawn(тело)", db[n].where);
    m_thunk th = thunk_named(args[0], top().scope);
    capture(th.env);
    uint32_t id = uint32_t(st.fibers.size());
    st.fibers.emplace_back();
    st.fibers.back().tasks.push_back(m_task{m_lambda_body, th.expr, 0, th.env, th.frame, 0, m_none, 0, 0, th.expr, th.ctx});
    st.ready.push_back(id);
    finish(mval{m_unit{}});
  }

  mval stream_value(uint64_t id) { return mval{m_agg{helper.types.named("stream"), {{helper.sym("id"), mval{int64_t(id)}}}}}; }
  uint64_t stream_id(mval const & v, span where) {
    mval x = deref(v);
    if(x.is<m_agg>())
      for(auto const & f: x.as<m_agg>().fields)
        if(helper.str(f.name) == "id" && f.v.is<int64_t>() && uint64_t(f.v.as<int64_t>()) < st.streams.size()) return uint64_t(f.v.as<int64_t>());
    throw diagnostic(cat("ожидался поток, получено ", describe(x)), where);
  }
  static bool transferable(mval const & v) {
    if(v.is<m_thunk>() || v.is<m_alias>()) return false;
    if(v.is<m_fn>()) return !v.as<m_fn>().lambda; // entity - function - yes, closure - no
    if(v.is<m_arr>())
      for(auto const & e: v.as<m_arr>().items)
        if(!transferable(e)) return false;
    if(v.is<m_agg>())
      for(auto const & f: v.as<m_agg>().fields)
        if(!transferable(f.v)) return false;
    return true;
  }

  // False - fiber is blocked (the task will be repeated when awakened).
  bool stream_op(std::string_view n, std::vector<mval> & a, span where, mval & out) {
    if(n == "stream_new") {
      st.streams.emplace_back();
      if(a.size() == 1 && a[0].is<int64_t>() && a[0].as<int64_t>() > 0) st.streams.back().cap = uint64_t(a[0].as<int64_t>());
      out = stream_value(st.streams.size() - 1);
      return true;
    }
    uint64_t sid = stream_id(a[0], where);
    m_stream & s = st.streams[sid];
    uint32_t me = st.current;
    out = mval{m_unit{}};
    if(n == "stream_write") {
      if(!transferable(a[1])) throw diagnostic(cat("в поток — только данные: ", describe(a[1])), where);
      if(s.buf.size() >= s.cap && !s.closed) {
        s.writers.push_back(me);
        return false;
      }
      if(s.closed) {
        diagnostic d("запись в закрытый поток", where, diag_kind::runtime);
        d.type = "closed_stream";
        throw d;
      }
      s.buf.push_back(std::move(a[1]));
      if(!s.readers.empty()) {
        uint32_t r = s.readers.front();
        s.readers.erase(s.readers.begin());
        wake_direct(r);
      }
      return true;
    }
    if(n == "stream_read" || n == "stream_more") {
      if(s.buf.empty() && !s.closed) {
        s.readers.push_back(me);
        return false;
      }
      if(n == "stream_more") {
        out = mval{!s.buf.empty()};
        return true;
      }
      if(s.buf.empty()) {
        diagnostic d("чтение из закрытого пустого потока", where, diag_kind::runtime);
        d.type = "closed_stream";
        throw d;
      }
      out = std::move(s.buf.front());
      s.buf.erase(s.buf.begin());
      if(!s.writers.empty()) {
        uint32_t w = s.writers.front();
        s.writers.erase(s.writers.begin());
        wake_direct(w);
      }
      return true;
    }
    if(n == "stream_close") {
      s.closed = true;
      for(uint32_t r: std::exchange(s.readers, {})) wake(r);
      for(uint32_t w: std::exchange(s.writers, {})) wake(w);
      return true;
    }
    throw diagnostic(cat("неизвестная операция потока ", n), where);
  }

  // Hide x: Name no longer available (§5.6).
  void eval_node(node_id n, n_hide const & h) {
    place p = find(top().scope, h.name);
    if(p.scope == m_none || cell(p).is<m_thunk>()) throw diagnostic(cat("hide: нет переменной ", helper.str(h.name)), db[n].where);
    not_hidden(p, h.name, db[n].where);
    var_of(p).hidden = n.value + 1;
    finish(mval{m_unit{}});
  }

  // require in the body - check in runtime (§5.4): violated - program error.
  void eval_node(node_id n, n_require const & r) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(r.cond, t.scope, t.frame);
    }
    if(!truthy(pop_val(), db[r.cond].where)) throw diagnostic("нарушено требование require", db[n].where);
    finish(mval{m_unit{}});
  }

  // v[i] as location: reference to a variable array element (alias with buffer generation).
  void push_elem_ref(node_id n, uint32_t scope, uint32_t frame) {
    auto & f = fib();
    f.tasks.push_back(m_task{m_elem_ref, n, 0, scope, frame, uint32_t(f.vals.size())});
    if(f.tasks.size() > 1) f.tasks.back().ctx = f.tasks[f.tasks.size() - 2].ctx;
  }
  void elem_ref_step() {
    m_task & t = top();
    auto const & ix = db.as<n_index>(t.node);
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(ix.index, t.scope, t.frame);
    }
    mval i = deref(pop_val());
    symbol name = db.as<n_ident>(ix.object).name;
    place p = find(t.scope, name);
    if(p.scope == m_none) throw diagnostic(cat("неизвестное имя ", helper.str(name)), db[ix.object].where);
    not_hidden(p, name, db[t.node].where);
    m_alias al{p.scope, p.slot};
    if(st.scopes[p.scope].vars[p.slot].v.is<m_alias>()) al = st.scopes[p.scope].vars[p.slot].v.as<m_alias>();
    mval & arr = follow(al);
    if(!arr.is<m_arr>() || !i.is<int64_t>()) throw diagnostic("auto& — ссылка на переменную или элемент массива переменной", db[t.node].where);
    int64_t k = i.as<int64_t>();
    auto size = arr.as<m_arr>().items.size();
    if(k < 0 || uint64_t(k) >= size) out_of_bounds(k, size, db[t.node].where);
    if(al.index != m_none) throw diagnostic("auto& — ссылка на переменную или элемент массива переменной", db[t.node].where); // interpreter-like
    finish(mval{m_alias{al.scope, al.slot, uint32_t(k), arr.as<m_arr>().gen}});
  }
  static bool elem_place(ast_db const & db, node_id x) { return db.is<n_index>(x) && db.is<n_ident>(db.as<n_index>(x).object); }

  void eval_node(node_id n, n_var const & v) {
    m_task & t = top();
    if(v.ref) { // &a = b is the second name of the same cell; &e = v[i] is a reference to an element
      if(t.phase == 0) {
        t.phase = 1;
        if(elem_place(db, v.init)) return push_elem_ref(v.init, t.scope, t.frame);
        if(!db.is<n_ident>(v.init)) throw diagnostic("auto& — ссылка на переменную или элемент массива", db[v.init].where);
        return push_eval(v.init, t.scope, t.frame);
      }
      mval x = pop_val();
      if(!x.is<m_alias>()) throw diagnostic("auto& — ссылка на переменную или элемент массива", db[v.init].where);
      declare(t.scope, v.name, std::move(x));
      return finish(mval{m_unit{}});
    }
    if(v.init && t.phase == 0) {
      t.phase = 1;
      return push_eval(v.init, t.scope, t.frame);
    }
    mval x = v.init ? deref(pop_val()) : mval{m_unit{}};
    declare(t.scope, v.name, std::move(x));
    finish(mval{m_unit{}});
  }

  // The name {a, b} is an aggregate by field positions (missing - default values
  // calculated in the global domain); array {…} / [...] - array (different type -
  // tuple, author's decision).
  void eval_node(node_id n, n_construct const & c) {
    m_task & t = top();
    symbol tname = db.as<n_ident>(c.type).name;
    auto st_it = helper.structs.find(tname);
    node_list all;
    if(st_it != helper.structs.end()) all = helper.fields_of_struct(st_it->first);
    if(t.phase < c.items.size()) return push_eval(c.items[t.phase++], t.scope, t.frame);
    if(st_it != helper.structs.end()) {
      if(c.items.size() > all.size()) throw diagnostic(cat(helper.str(tname), ": полей ", all.size(), ", значений ", c.items.size()), db[n].where);
      uint32_t k = t.phase; // the next field without a value - the default value
      if(k < all.size()) {
        auto const & f = db.as<n_var>(all[k]);
        if(!f.init) throw diagnostic(cat(helper.str(tname), ": не задано поле ", helper.str(f.name)), db[n].where);
        ++t.phase;
        return push_eval(f.init, st.globals, t.frame);
      }
      m_agg g{helper.types.named(helper.str(tname)), {}};
      for(uint32_t i = 0; i < all.size(); ++i) g.fields.push_back({db.as<n_var>(all[i]).name, deref(fib().vals[t.base + i])});
      return finish(mval{std::move(g)});
    }
    if(helper.str(tname) != "array") throw diagnostic(cat("конструктор ", helper.str(tname), "{} пока не поддержан"), db[n].where);
    m_arr a;
    for(uint32_t i = 0; i < c.items.size(); ++i) a.items.push_back(deref(fib().vals[t.base + i]));
    finish(mval{std::move(a)});
  }

  // using operator ... - redefinition to the end of the frame of the function-context (checked).
  void eval_node(node_id n, n_fn const & f) {
    m_task & t = top();
    if(f.scoped) {
      fib().overrides.push_back({f.name, n, t.frame});
      return finish(mval{m_unit{}});
    }
    // Local function: the same names in this block are one set of overloads (like an interpreter)
    capture(t.scope);
    for(auto & v: st.scopes[t.scope].vars)
      if(v.name == f.name && v.v.is<m_fn>() && !v.v.as<m_fn>().set.empty() && v.v.as<m_fn>().env == t.scope) {
        v.v.as<m_fn>().set.push_back(n);
        return finish(mval{m_unit{}});
      }
    declare(t.scope, f.name, mval{m_fn{{}, {}, t.scope, {n}}});
    finish(mval{m_unit{}});
  }

  // Redefining the context for an operation: the nearest, whose parameter types take values.
  node_id override_for(std::vector<node_id> const & fns, std::vector<mval> const & args) {
    auto & f = fib();
    if(f.overrides.empty() || args.size() != 2 || !db.is<n_op>(site_node) || fns.empty()) return {};
    if(std::ranges::any_of(f.frames, [](m_frame const & fr) { return fr.plain; })) return {};
    symbol entity = db.as<n_fn>(fns.front()).name;
    for(auto it = f.overrides.rbegin(); it != f.overrides.rend(); ++it) {
      if(it->entity != entity) continue;
      auto const & ps = db.as<n_fn>(it->fn).params;
      bool fits = ps.size() == args.size();
      for(std::size_t k = 0; k < ps.size() && fits; ++k) {
        auto const & p = db.as<n_param>(ps[k]);
        if(p.type && db.is<n_ident>(p.type) && helper.str(db.as<n_ident>(p.type).name) != describe(deref(args[k]))) fits = false;
      }
      if(fits) return it->fn;
    }
    return {};
  }

  // ct e/ct { … } - once per instance (call location, embedding context): result of
  // canonical parsing or, if not, computation here (§7.3).
  void eval_node(node_id n, n_ct const & c) {
    m_task & t = top();
    if(t.phase == 0) {
      for(auto const & m: st.ct_memo)
        if(m.node == n && m.ctx == t.ctx) return finish(mval(m.v));
      t.phase = 1;
      return push_eval(c.body, t.scope, t.frame);
    }
    mval v = deref(pop_val());
    st.ct_memo.push_back({n, t.ctx, v});
    finish(std::move(v));
  }
  bool in_ct() {
    return std::ranges::any_of(fib().tasks, [&](m_task const & x) { return x.kind == m_eval && x.phase == 1 && db.is<n_ct>(x.node); });
  }

  // try_compile { … } - SFINAE block: the instantiation error in it removes sfinae_cut.
  void eval_node(node_id, n_try_compile const & c) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(c.body, t.scope, t.frame);
    }
    finish(deref(pop_val()));
  }

  void eval_node(node_id n, auto const &) { unsupported(kind_name(db[n]), n); }

  // Call phases with numbers: 1 - 2 - overload, 3 - built-in, 4 - 5 - construction block. Call
  // of closure: your frame (return - from the lambda), the area above the environment.
  void call_closure(node_id n, m_fn fv, node_list const & args) {
    m_task & t = top();
    auto const & l = db.as<n_lambda>(fv.lambda);
    if(l.params.size() != args.size()) throw diagnostic(cat("лямбда ждёт ", l.params.size(), " аргументов, передано ", args.size()), db[n].where);
    t.extra = fv.lambda;
    t.a = fv.env;
    t.phase = 6;
    closure_args_step(n);
  }
  void closure_args_step(node_id n) {
    m_task & t = top();
    auto & f = fib();
    auto const & args = db.as<n_call>(n).args;
    if(t.phase == 7) {
      mval r = deref(pop_val());
      return finish(std::move(r));
    }
    uint32_t have = uint32_t(f.vals.size() - t.base);
    if(have < args.size()) return push_eval(args[have], t.scope, t.frame);
    auto const & l = db.as<n_lambda>(t.extra);
    uint32_t sc = new_scope(t.a);
    for(uint32_t i = 0; i < args.size(); ++i) declare(sc, db.as<n_param>(l.params[i]).name, deref(f.vals[t.base + i]));
    f.vals.resize(t.base);
    uint32_t id = ++st.frame_counter;
    f.frames.push_back({id, symbol{}, false, t.frame});
    t.phase = 7;
    uint64_t ctx = t.ctx; // Closing - in the context of the calling
    f.tasks.push_back(m_task{m_call_body, t.extra, 0, sc, id, uint32_t(f.vals.size()), sc, id, 0, {}, ctx});
  }

  void resume_call(node_id n) {
    m_task & t = top();
    if(t.phase == 3) return builtin_args_step(n);
    if(t.phase >= 6) return closure_args_step(n);
    if(t.phase >= 4) return block_args_step(n);
    call_args_step(n);
  }

  // Process Dump: journal base (code, including node, generated during execution) and
  // state - arenas, task and value stacks, frames.
  std::string dump() {
    st.type_names = helper.types.names;
    writer w;
    for(char c: std::string_view("TZPROC01")) w.put(c);
    w.put(name);
    w.put(root.value);
    w.put(db.journal());
    w.put(st);
    return std::move(w.bytes);
  }
};

// Recovery in another VM: base - from journal dump, state - as it was.
struct restored_process {
  ast_db db;
  std::optional<process_machine> m;

  explicit restored_process(std::string_view bytes) {
    reader r(bytes);
    for(char c: std::string_view("TZPROC01"))
      if(r.get<char>() != c) throw diagnostic("не дамп процесса tz", {});
    std::string name = r.get<std::string>();
    node_id root(r.get<uint32_t>());
    db.replay(r.get<std::vector<journal_entry>>());
    m.emplace(db, root, name);
    m->st = r.get<m_state>();
    m->helper.types.names = m->st.type_names;
  }
};

} // namespace tz
