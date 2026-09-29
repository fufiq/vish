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
};
// expr-параметр: выражение вызывающего в его области и кадре (§6.1).
struct m_thunk {
  node_id expr;
  uint32_t env = m_none;
  uint32_t frame = 0;
};
// Parameter - alias of the caller's place (author's decision): area and slot.
struct m_alias {
  uint32_t scope = m_none;
  uint32_t slot = 0;
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
};

// bootstrapping signal: break/continue/return (with target frame)/throw.
struct m_signal {
  uint8_t kind = 0; // 0 no
  mval v;
  uint32_t target = 0;
};

struct m_fiber {
  std::vector<m_task> tasks;
  std::vector<mval> vals;
  std::vector<m_frame> frames;
  m_signal pending;
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
};

enum : uint8_t { m_eval, m_call_body, m_lambda_body, m_thunk_eval };
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
    while(c->is<m_alias>()) c = &st.scopes[c->as<m_alias>().scope].vars[c->as<m_alias>().slot].v;
    return *c;
  }
  mval deref(mval const & x) {
    if(x.is<m_alias>()) return cell({x.as<m_alias>().scope, x.as<m_alias>().slot});
    return x;
  }
  void declare(uint32_t scope, symbol name, mval v) { st.scopes[scope].vars.push_back({name, std::move(v)}); }

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
      else if constexpr(std::is_same_v<T, m_arr>) return cat("array<", a.items.empty() ? std::string("?") : describe(a.items[0]), ">");
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

  m_fiber & fib() { return st.fibers[0]; }

  void push_eval(node_id n, uint32_t scope, uint32_t frame) {
    auto & f = fib();
    f.tasks.push_back(m_task{m_eval, n, 0, scope, frame, uint32_t(f.vals.size())});
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

  [[noreturn]] void unsupported(std::string_view what, node_id n) {
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
    for(node_id item: db.as<n_module>(root).items)
      if(db.is<n_var>(item)) unsupported("переменные модуля", item);
    auto it = helper.functions.find(helper.sym("main"));
    if(it == helper.functions.end()) throw diagnostic("нет функции main", {});
    invoke(it->second, 0, 0, db[it->second.front()].where);
  }

  // Step: One phase of the upper task; false, the process is complete.
  bool step() {
    auto & f = fib();
    if(f.pending.kind != sig_none) {
      unwind();
      return !st.done;
    }
    if(f.tasks.empty()) {
      st.done = true;
      if(!f.vals.empty() && deref(f.vals.back()).is<int64_t>()) st.exit_code = deref(f.vals.back()).as<int64_t>();
      return false;
    }
    ++st.steps;
    m_task & t = f.tasks.back();
    switch(t.kind) {
      case m_eval: eval_step(); break;
      case m_call_body: call_body_step(); break;
      case m_lambda_body: lambda_body_step(); break;
      default: throw diagnostic("машина: неизвестная задача", {});
    }
    return true;
  }

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
    if(s.kind == sig_throw) throw diagnostic(cat("непойманное исключение: ", helper.show_v(to_value(s.v))), {});
    throw diagnostic(s.kind == sig_return ? "return вне функции" : "break/continue вне цикла", {});
  }

  void end_frame(m_task const & t) {
    auto & fr = fib().frames;
    if(!fr.empty() && fr.back().id == t.a) fr.pop_back();
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
  void invoke(std::vector<node_id> const & fns, uint32_t nargs, uint32_t caller_frame, span where) {
    auto & f = fib();
    std::vector<mval> args(f.vals.end() - nargs, f.vals.end());
    node_id chosen;
    int best = -1;
    bool tie = false;
    for(node_id fnode: fns) {
      auto const & fn = db.as<n_fn>(fnode);
      if(fn.params.size() != nargs) continue;
      int score = 0;
      bool fits = true;
      for(uint32_t i = 0; i < nargs && fits; ++i) {
        auto const & p = db.as<n_param>(fn.params[i]);
        if(p.type && db.is<n_ident>(p.type)) {
          if(args[i].is<m_thunk>()) continue;
          std::string want(helper.str(db.as<n_ident>(p.type).name)), have = describe(args[i]);
          if(have != want && have.substr(0, have.find('<')) != want) fits = false;
          else ++score;
        }
      }
      if(fits && has_leading_require(fn)) unsupported("перегрузка с require", fnode);
      if(!fits) continue;
      if(score > best) {
        best = score;
        chosen = fnode;
        tie = false;
      } else if(score == best) tie = true;
    }
    if(!chosen) throw diagnostic(cat("нет подходящей перегрузки ", helper.str(db.as<n_fn>(fns.front()).name)), where);
    if(tie) throw diagnostic("неоднозначный вызов", where);
    auto const & fn = db.as<n_fn>(chosen);
    // function-operator from one primitive - VM operation
    if(helper.intrinsic_operator(chosen)) {
      std::string prim(helper.str(db.as<n_ident>(db.as<n_call>(fn.body).callee).name));
      f.vals.resize(f.vals.size() - nargs);
      mval r = primitive(prim, args, where);
      f.vals.push_back(std::move(r));
      return;
    }
    uint32_t id = ++st.frame_counter;
    f.frames.push_back({id, fn.name, fn.outline, caller_frame});
    uint32_t sc = new_scope(st.globals);
    for(uint32_t i = 0; i < nargs; ++i) {
      auto const & p = db.as<n_param>(fn.params[i]);
      mval a = std::move(args[i]);
      if(p.mode == param_mode::own && a.is<m_alias>()) a = deref(a); // displacement
      declare(sc, p.name, std::move(a));
    }
    f.vals.resize(f.vals.size() - nargs);
    f.tasks.push_back(m_task{m_call_body, chosen, 0, sc, id, uint32_t(f.vals.size()), sc, id});
  }

  bool has_leading_require(n_fn const & fn) const {
    if(!fn.body || !db.is<n_block>(fn.body)) return false;
    auto const & items = db.as<n_block>(fn.body).items;
    return !items.empty() && db.is<n_require>(items[0]);
  }

  void call_body_step() {
    auto & f = fib();
    m_task & t = f.tasks.back();
    auto const & fn = db.as<n_fn>(t.node);
    if(t.phase == 0) {
      t.phase = 1;
      push_eval(fn.body, t.scope, t.a);
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
      if(helper.functions.contains(x.name)) return finish(mval{m_fn{x.name, {}, m_none}});
      throw diagnostic(cat("неизвестное имя ", helper.str(x.name)), db[n].where);
    }
    mval & c = cell(p);
    if(c.is<m_thunk>()) { // expr-parameter - the expression of the caller in its area and frame
      m_thunk th = c.as<m_thunk>();
      if(db.is<n_lambda>(th.expr) && db.as<n_lambda>(th.expr).block)
        throw diagnostic("блок конструкции вводит имена — его вызывают с аргументами: body(x)", db[th.expr].where);
      t.kind = m_lambda_body; // The result of the expression is the result of this node
      t.extra = th.expr;
      t.scope = th.env;
      t.frame = th.frame;
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

  void eval_node(node_id n, n_if const & i) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(i.cond, t.scope, t.frame);
    }
    if(t.phase == 1) {
      bool c = truthy(pop_val(), db[i.cond].where);
      t.phase = 2;
      node_id next = c ? i.then_ : i.else_;
      if(!next) return finish(mval{m_unit{}});
      return push_eval(next, t.scope, t.frame);
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
    if(k < 0 || uint64_t(k) >= xs.size()) throw diagnostic(cat("индекс ", k, " вне границ массива размера ", xs.size()), db[n].where);
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
      mval & c = cell(p);
      c = op == "=" ? std::move(rhs) : primitive(prim_of(op), {c, rhs}, db[n].where);
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
      if(k < 0 || uint64_t(k) >= xs.size()) throw diagnostic(cat("индекс ", k, " вне границ массива размера ", xs.size()), db[lhs].where);
      xs[uint64_t(k)] = op == "=" ? std::move(rhs) : primitive(prim_of(op), {xs[uint64_t(k)], rhs}, db[n].where);
      return finish(mval{m_unit{}});
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
        if(cv.is<m_fn>() && cv.as<m_fn>().lambda) unsupported("вызов лямбды", n);
        if(cv.is<m_fn>()) {
          auto it = helper.functions.find(cv.as<m_fn>().name);
          if(it != helper.functions.end()) return eval_call_args(n, it->second, c.args, false);
        }
        unsupported("вызов значения", n);
      }
      auto it = helper.functions.find(name);
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

  std::vector<node_id> const & overloads_of(node_id any) {
    return helper.functions.at(db.as<n_fn>(any).name);
  }

  void call_args_step(node_id n) {
    m_task & t = top();
    node_list args = call_args(n);
    auto const & fns = overloads_of(t.extra);
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
        fib().vals.push_back(mval{m_thunk{args[i], t.scope, t.frame}});
        continue;
      }
      return push_eval(args[i], t.scope, t.frame); // the value (or alias of the place) on the stack
    }
    t.phase = 2;
    uint32_t frame = t.frame;
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
      mval & xs = cell({al.scope, al.slot});
      if(!xs.is<m_arr>()) throw diagnostic("append: не массив", db[n].where);
      xs.as<m_arr>().items.push_back(a[1]);
      return finish(mval{m_unit{}});
    }
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
    fib().tasks.push_back(m_task{m_lambda_body, t.extra, 0, sc, t.b, uint32_t(fib().vals.size()), m_none, 0, 0, l.body});
  }

  void eval_node(node_id n, n_var const & v) {
    m_task & t = top();
    if(v.ref) unsupported("ссылка &e = …", n);
    if(v.init && t.phase == 0) {
      t.phase = 1;
      return push_eval(v.init, t.scope, t.frame);
    }
    mval x = v.init ? deref(pop_val()) : mval{m_unit{}};
    declare(t.scope, v.name, std::move(x));
    finish(mval{m_unit{}});
  }

  void eval_node(node_id n, n_construct const & c) {
    m_task & t = top();
    (void)c;
    (void)t;
    unsupported("конструирование агрегата", n);
  }

  void eval_node(node_id n, auto const &) { unsupported(kind_name(db[n]), n); }

  // Call phases with numbers: 1 - 2 - overload, 3 - built-in, 4 - 5 - construction block.
  void resume_call(node_id n) {
    m_task & t = top();
    if(t.phase == 3) return builtin_args_step(n);
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
