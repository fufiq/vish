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

#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <set>
#include <shared_mutex>

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
  std::set<value *> whole; // Completely recorded (replacement, growth) - cell version grows
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
  void written(value * c, bool whole) {
    auto & e = cells[c];
    if(whole) e.second = ++clock;
    for(auto * l: logs)
      if(e.first <= l->start) {
        if(whole) l->whole.insert(c);
        if(l->cells.insert(c).second && l->keep_old) l->old.emplace(c, *c);
      }
  }
};

// parsing instance compiler question and answer (instance deduplication).
struct jit_query {
  std::string name;
  std::vector<std::pair<char, int64_t>> args; // 'c' is a node, 'i' is a number
  std::string answer;
};

struct scope {
  scope * parent = nullptr;
  // The root of the body function: the environment of the call
// Free name
  // The body is searched there after lexical (interpreter::find).
  scope * caller = nullptr;
  node_id fn;
  std::vector<std::pair<symbol, value *>> vars;
  std::deque<value> storage;
  bool captured = false;
  cell_book * book = nullptr;
  // Resource alias (§12.2): Caller cell and its argument.
// Transfer from the parameter decides the use
  // and the caller (transfer by use across the call border)
  struct borrow {
    value * cell;
    node_id src;
    scope * env;
  };
  std::vector<borrow> borrows;

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
    // Types known in parsing: Values store type numbers - repeat only if the new parsing registry
    // starts the same way (correction elsewhere could register types in a different order)
    //
    uint32_t types_count = 0;
    uint64_t types_hash = 0;
  };
  std::map<std::string, entry> entries;
  uint32_t hits = 0, recomputed = 0;
  std::vector<std::string> recomputed_names;
};

// Primitives introduced by native “cpp” { … } (§3.3): Bodies assembled in VM - out/native.hpp
// generates `tz native` from library texts; key is name and parameter types.
struct interpreter;
struct native_entry {
  std::string_view key;
  value (*fn)(interpreter & vm, std::vector<value> & a, span where);
};
std::span<native_entry const> native_table();

std::string native_key(ast_db const & db, node_id fn) {
  auto const & f = db.as<n_fn>(fn);
  std::string k = cat(db.strings.text(f.name), "(");
  for(std::size_t i = 0; i < f.params.size(); ++i) {
    auto const & p = db.as<n_param>(f.params[i]);
    k += cat(i ? ", " : "", p.type && db.is<n_ident>(p.type) ? db.strings.text(db.as<n_ident>(p.type).name) : std::string_view("_"));
  }
  return k + ")";
}

// A function whose body is native.
node_id native_owner(ast_db const & db, node_id n) {
  node_id f = db[n].parent;
  if(!f || !db.is<n_fn>(f) || db.as<n_fn>(f).body != n) throw diagnostic("native — только тело функции: f(…) => native \"cpp\" { … }", db[n].where);
  return f;
}

native_entry const * native_of(ast_db const & db, node_id n) {
  if(db.strings.text(db.as<n_native>(n).lang) != "cpp") throw diagnostic(cat("native: язык ", db.strings.text(db.as<n_native>(n).lang), " не поддержан — только \"cpp\""), db[n].where);
  std::string key = native_key(db, native_owner(db, n));
  for(auto const & e: native_table())
    if(e.key == key) return &e;
  throw diagnostic(cat("native ", key, " не собран в VM — пересоберите: ./build.sh"), db[n].where);
}

// code generator entity in the context of the goal (§10.4): function-goal (amd64, arm64) and in its
// body - `using operator` or context function with these types of parameters; the last announcement
// - the first (program redefined prelude). The second is the function-goal itself (`model()` is the
// register model). code generator entity in the body of the function-context fn (goal or nested
// context of the goal).
std::pair<node_id, node_id> codegen_in(ast_db const & db, node_id fn, std::string_view entity, std::vector<std::string> const & types) {
  if(!fn || !db.is<n_fn>(fn) || !db.is<n_block>(db.as<n_fn>(fn).body)) return {};
  auto const & items = db.as<n_block>(db.as<n_fn>(fn).body).items;
  for(auto it = items.rbegin(); it != items.rend(); ++it) {
    node_id s = *it;
    if(!db.is<n_fn>(s) || db.strings.text(db.as<n_fn>(s).name) != entity) continue;
    auto const & ps = db.as<n_fn>(s).params;
    bool fits = ps.size() == types.size();
    for(std::size_t k = 0; fits && k < ps.size(); ++k) {
      node_id ty = db.as<n_param>(ps[k]).type;
      fits = ty && db.is<n_ident>(ty) && db.strings.text(db.as<n_ident>(ty).name) == types[k];
    }
    if(fits) return {s, fn};
  }
  return {};
}
std::pair<node_id, node_id> find_codegen(ast_db const & db, std::string_view target, std::string_view entity, std::vector<std::string> const & types) {
  for(uint32_t i = db.size(); i-- > 0;)
    if(db.is<n_fn>(node_id(i)) && db.strings.text(db.as<n_fn>(node_id(i)).name) == target)
      if(auto r = codegen_in(db, node_id(i), entity, types); r.first) return r;
  return {};
}

// --- VM bands (§9.5, step 12.3b) ---
//
// 
// 
// The program (monopoly) is the main stream of host on the production cores, without a VM scheduler;
// background processes are their own band: host stream on energy-efficient cores, their scheduler, their
// ring; the process receives a band at start-up and does not move (author's decision 2026-10-19: "in the
// interpreter now"). Between the bands are only flows (data) and the outcome of processes. The code (base)
// changes only the main one - with the background stopped: the background keeps it reading while it
// performs, and releases it in safe points and while waiting (*mine*).
//
//
std::shared_mutex code_lock;
std::atomic<uint32_t> code_writers{0};
std::recursive_mutex vm_lock; // Flows between processes, VM processes, incoming bands
std::recursive_mutex out_lock; // output
std::recursive_mutex service_lock; // status of VM services (step 12.4)
thread_local bool on_bg_lane = false;
thread_local uint32_t code_write_depth = 0; // Writer Inside Writer (compilation from compilation)
// Code Writer (process elaboration, JIT compilation): background bar standing while he writes
struct code_write {
  bool counted = lanes_on.load(std::memory_order_acquire) && !on_bg_lane;
  bool on = counted && code_write_depth++ == 0;
  code_write() {
    if(!on) return;
    code_writers.fetch_add(1);
    code_lock.lock();
  }
  ~code_write() {
    if(counted) --code_write_depth;
    if(!on) return;
    code_lock.unlock();
    code_writers.fetch_sub(1);
    code_writers.notify_all();
  }
  code_write(code_write const &) = delete;
};
// The background bar gives way to the code to the writer: let go of reading, wait, take again
void code_pause() {
  code_lock.unlock_shared();
  for(uint32_t w; (w = code_writers.load()) != 0;) code_writers.wait(w);
  code_lock.lock_shared();
}

struct interpreter {
  // residual program inputs: known arguments and strings of stdin (§13.2).
  struct known_inputs {
    std::vector<std::string> stdin_lines;
  };

  // Parent is a process (or sample) within the same VM: a common type registry and scheduler.
  // lane_of - Mainband VM: This root is the background band scheduler (step 12.3b)
  interpreter(ast_db & db, node_id module, std::vector<std::string> args, mode m = mode::run,
      std::optional<known_inputs> known = std::nullopt, check_cache * cache = nullptr, interpreter * parent = nullptr, interpreter * lane_of = nullptr)
      : db(db), mod(module), run_mode(m), cache(cache), inputs(std::move(known)), types(lane_of ? lane_of->types : parent ? parent->types : own_types),
        root(parent ? parent->root : this), vm_root(lane_of ? lane_of : parent ? parent->vm_root : this) {
    sym_intercept = sym("intercept");
    sym_alloc_in = sym("alloc_in");
    sym_append = sym("append");
    sym_pin_storage = sym("pin_storage");
    sym_set_ct_state = sym("set_ct_state");
    sym_spawn = sym("fiber_start");
    sym_pressure = sym("pressure_handler");
    sym_process = sym("process");
    sym_yield = sym("yield");
    sym_is_index = sym("is_index");
    globals = new_scope(nullptr);
    if(!parent)
      if(char const * q = std::getenv("TZ_SLICE")) slice_points = uint32_t(std::strtoul(q, nullptr, 10)); // Quantum - at safe points (tests)
    if(!parent && !lane_of) grants = granted_by_user();
    if(!args.empty()) prog_path = args[0];
    array_v a;
    for(auto & s: args) a.push(value(s));
    value args_v(std::move(a));
    if(checking() && !inputs) args_v = erase(value(array_v::of({value(std::string("x"))}))); // arguments - runtime
    globals->declare(sym("args"), std::move(args_v));
    globals->declare(sym("stdin"), value(agg_v{types.named("stream"), {}})); // descriptor, no value
    // prelude - first, then used modules (dependencies before), then your own; module error
    // (no module) - program error during elaboration
    try {
      register_modules(module);
    } catch(diagnostic const & d) {
      setup_error = d;
    }
  }

  void register_modules(node_id module) {
    order_modules(module);
    std::vector<node_id> all;
    if(db.prelude && db.prelude != module) all.push_back(db.prelude);
    all.insert(all.end(), module_order.begin(), module_order.end());
    for(node_id m: all) extension_any = extension_any || has_extension_attr(m);
    // generated by ct-code instance - after elaboration (rollback could remove it)
    for(node_id m: all) {
      registering = m;
      for(node_id item: db.as<n_module>(m).items)
        if((!db.is<n_extend>(item) || db.is_struct(item)) && visible_item(m, item) && !db.inst_emits.contains(item.value)) register_item(item);
    }
    for(node_id m: all) { // Expansion - after announcements
      registering = m;
      for(node_id item: db.as<n_module>(m).items)
        if(db.is<n_extend>(item) && !db.is_struct(item) && m != db.prelude && visible_item(m, item) && !db.inst_emits.contains(item.value)) register_item(item);
    }
    registering = node_id{};
    if(module_order.size() > 1)
      for(node_id m: module_order) check_type_names(m, m);
  }

  // Isolation of submodules and for type names (tail of step 10): the name struct in the code of the
  // module m - parameter type, variable and result, Name {…}, sample, base, extension Name { … } - can
  // be seen if the struct is declared in m, in prelude or in the module that m takes use directly.
  //  
// The value of the type by name (typeof(x) ==T) is when calculating the eval name.
  void check_type_names(node_id m, node_id x) {
    auto name_at = [&](node_id t) { return t && db.is<n_ident>(t) ? db.as<n_ident>(t).name : symbol{}; };
    auto use = [&](symbol name, node_id at) {
      if(!name) return;
      auto it = structs.find(name);
      if(it == structs.end()) return;
      if(node_id home = module_of(it->second); !sees_directly(m, home, name)) fail(not_visible("тип", name, home), db[at].where);
    };
    if(db.is<n_param>(x)) use(name_at(db.as<n_param>(x).type), x);
    else if(db.is<n_var>(x)) use(name_at(db.as<n_var>(x).type), x);
    else if(db.is<n_construct>(x)) use(name_at(db.as<n_construct>(x).type), x);
    else if(db.is<n_pat>(x)) use(name_at(db.as<n_pat>(x).type), x);
    else if(db.is<n_fn>(x)) use(name_at(db.as<n_fn>(x).result_type), x);
    else if(db.is<n_extend>(x)) {
      if(!db.as<n_extend>(x).of) use(db.as<n_extend>(x).name, x); // extension of another struct - his name
      use(db.as<n_extend>(x).base, x);
    }
    db.for_each_child(x, [&](node_id c) { check_type_names(m, c); });
  }
  // Module m sees the name announcement of the home module directly: its own, prelude, use home
  // (use home. {…} - named; operators - always). use is not transitive.
  bool sees_directly(node_id m, node_id home, symbol name) const {
    if(!m || !home || m == home || home == db.prelude) return true;
    for(node_id item: db.as<n_module>(m).items)
      if(db.is<n_use>(item) && db.as<n_use>(item).key == db.as<n_module>(home).name) {
        auto const & u = db.as<n_use>(item);
        if(u.names.empty() || std::ranges::contains(u.names, name) || operator_name(str(name))) return true;
      }
    return false;
  }
  std::string not_visible(std::string_view what, symbol name, node_id home) const {
    std::string_view k = home && db.as<n_module>(home).name ? str(db.as<n_module>(home).name) : std::string_view("?");
    if(home && vm_root->service_modules.contains(home.value))
      return cat(what, " ", str(name), " — состояние сервиса ", k, ": снаружи не видна («секреты остаются в сервисе»), только через его функции");
    return cat(what, " ", str(name), what == "переменная" ? " объявлена" : " объявлен", " в модуле ", k, ", а use его здесь нет: модули изолированы, use не транзитивен — use ", k, ";");
  }
  // A variable module with this name that the place does not see (for the "unknown name" error).
  node_id unseen_module_var(symbol name) {
    value * c = globals ? globals->find(name) : nullptr;
    auto it = c ? global_home.find(c) : global_home.end();
    return it == global_home.end() ? node_id{} : it->second;
  }

  // --- Modules (§9.1): use - semantic key inclusion ---

  // Modules in order of registration: used before, your last; selective use k. {a, b} - only
  // named (use k; somewhere else - all). Who sees what name is name_visible (use is not
  // transitive).
  void order_modules(node_id root) {
    std::set<uint32_t> done;
    auto visit = [&](auto && self, node_id m) -> void {
      if(!done.insert(m.value).second) return;
      for(node_id item: db.as<n_module>(m).items) {
        if(!db.is<n_use>(item)) continue;
        auto const & u = db.as<n_use>(item);
        node_id dep;
        for(uint32_t i = 0; i < db.size(); ++i)
          if(db.is<n_module>(node_id(i)) && db.as<n_module>(node_id(i)).name == u.key) dep = node_id(i);
        if(!dep) fail(cat("нет модуля ", str(u.key)), db[item].where);
        auto & only = module_names[dep.value];
        if(u.names.empty()) whole_modules.insert(dep.value);
        else only.insert(u.names.begin(), u.names.end());
        self(self, dep);
      }
      module_order.push_back(m);
    };
    visit(visit, root);
    // Qualified name modules (geo.vec.norm(…) without use) are registered in its entirety: the
    // name is named in full, isolation does not hide it
    std::vector<node_id> named;
    auto scan = [&](auto && self, node_id x) -> void {
      if(db.is<n_call>(x) && db.is_member(db.as<n_call>(x).callee))
        if(node_id q = module_by_path(db.member_of(db.as<n_call>(x).callee).object); q && !done.contains(q.value)) named.push_back(q);
      db.for_each_child(x, [&](node_id c) { self(self, c); });
    };
    for(node_id m: module_order) scan(scan, m);
    for(node_id q: named) {
      whole_modules.insert(q.value);
      std::vector<node_id> tail;
      std::swap(tail, module_order);
      visit(visit, q);
      std::swap(tail, module_order);
      module_order.insert(module_order.end() - 1, tail.begin(), tail.end()); // before his (last)
    }
  }
  // Insulation of sub-modules:
//  
// The name is visible in modulus m, if at least one of its announcements is in m, in prelude or in
  // A module that m takes use directly (use k. {…} - named); use is not transitive. The
  // overload visible name from the other modules involved is extensibility (§7.3).
  bool name_visible(node_id m, symbol name, std::vector<node_id> const & decls) {
    if(!m || module_order.size() <= 1) return true;
    auto [it, fresh] = name_seen.try_emplace((uint64_t(m.value) << 32) | name.value, true);
    if(!fresh) return it->second;
    it->second = std::ranges::any_of(decls, [&](node_id f) { return sees_directly(m, module_of(f), name); });
    return it->second;
  }
  std::unordered_map<uint64_t, bool> name_seen;
  // Qualified name module: geo.vec in geo.vec.norm(…) is a chain of names, the first is not
  // variable; no such module is empty.
  node_id qualified_module(node_id obj, scope * env) {
    node_id x = obj;
    while(db.is_member(x)) x = db.member_of(x).object;
    if(!db.is<n_ident>(x) || find(env, db.as<n_ident>(x).name)) return {};
    return module_by_path(obj);
  }
  // module on the chain of names (geo.vec); base modules - by name, recalculation as the base grows
  node_id module_by_path(node_id obj) {
    for(; modules_seen < db.size(); ++modules_seen) // new node databases - once
      if(node_id m(modules_seen); m != db.prelude && db.is<n_module>(m) && db.as<n_module>(m).name) modules_by_name[db.as<n_module>(m).name.value] = m;
    if(modules_by_name.empty()) return {};
    std::string key = module_path(db, obj);
    auto it = db.strings.index.find(key);
    if(it == db.strings.index.end()) return {};
    auto m = modules_by_name.find(it->second.value);
    return m == modules_by_name.end() ? node_id{} : m->second;
  }
  uint32_t modules_seen = 0;
  std::unordered_map<uint32_t, node_id> modules_by_name;
  bool visible_item(node_id m, node_id item) const {
    if(m == mod || m == db.prelude || whole_modules.contains(m.value)) return true;
    symbol name =  db.is<n_fn>(item) ? db.as<n_fn>(item).name
        : db.is<n_extend>(item)                                                        ? db.as<n_extend>(item).name
        : db.is<n_var>(item)                                                           ? db.as<n_var>(item).name
                                                                                       : symbol{};
    if(name && operator_name(str(name))) return true; // module operators - at any use (module - inclusion, author's decision 2026-10-02)
    auto it = module_names.find(m.value);
    return !name || (it != module_names.end() && it->second.contains(name));
  }
  // --- Extensibility control (§9.1)
  // -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// 
// 
// 
// 
// He didn't give me a mechanism.
  // (approved by user 2026-10-05): [[extension(g)]] at the module (all of its entity) or at the
  // announcement entity; g is a function of the language, executed at parsing for each extension
  // entity: overload, field (Name { … }), & f(…) require syntax, operator and using operator,
  // including its own (decides by g itself); solution -. Without [[extension]], the registration
  // program is not recorded - there are no costs.
  struct ext_reg {
    symbol entity;
    node_id item;
    node_id module;
  };
  // [[extension(g)]] among the attributes is the name of the check g
  symbol extension_check(node_list const & attrs) {
    for(node_id a: attrs) {
      if(!db.is<n_call>(a) || !db.is<n_ident>(db.as<n_call>(a).callee) || str(db.as<n_ident>(db.as<n_call>(a).callee).name) != "extension") continue;
      auto const & args = db.as<n_call>(a).args;
      if(args.size() != 1 || !db.is<n_ident>(args[0])) fail("[[extension(проверка)]]: проверка — имя функции языка", db[a].where);
      return db.as<n_ident>(args[0]).name;
    }
    return {};
  }
  node_list const * decl_attrs(node_id n) const {
    if(db.is<n_fn>(n)) return &db.as<n_fn>(n).attrs;
    if(db.is_struct(n)) return &db.as<n_extend>(n).attrs;
    return nullptr;
  }
  // The module or one of its ads has [[extension(…)]]
  bool has_extension_attr(node_id m) {
    if(extension_check(db.as<n_module>(m).attrs)) return true;
    for(node_id item: db.as<n_module>(m).items)
      if(auto * as = decl_attrs(item); as && extension_check(*as)) return true;
    return false;
  }
  // entity registration: the first is an announcement (host module), the following are extensions.
  void note_extension(symbol entity, node_id item) {
    if(!extension_any || !entity) return;
    node_id m = registering ? registering : module_of(item);
    if(!m) m = ct_source ? module_of(ct_source) : mod;
    auto & regs = entity_regs[entity.value];
    if(std::ranges::any_of(regs, [&](ext_reg const & r) { return r.item == item; })) return;
    regs.push_back({entity, item, m});
    ext_order.push_back({entity.value, uint32_t(regs.size() - 1)});
    if(extensions_checked) check_extension(entity, regs.size() - 1); // emitted
  }
  // Rules parsing in the blocks of the element - operator, syntax, design, using operator
  // (structure body rules, local) - also entity module: their addition - extension. *My:* as
  // overload (§9.1); the quotation is a code value (checked when emitted), the rule of the raw
  // {r} block is different for each design.
  void note_rules(node_id n, bool nested = false) {
    if(db.is<n_quote>(n)) return;
    if(nested && db.is<n_fn>(n)) {
      auto const & f = db.as<n_fn>(n);
      if(f.name && !raw_rule_form(db, f) && (f.scoped || rule_decl(n))) note_extension(f.name, n);
    }
    db.for_each_child(n, [&](node_id c) { note_rules(c, true); });
  }
  // entity checks: [[extension(g)]] for its ads and the first registration module; (g, the ad to
  // which the verification belongs)
  std::vector<std::pair<symbol, node_id>> extension_checks(std::vector<ext_reg> const & regs) {
    std::vector<std::pair<symbol, node_id>> out;
    for(auto const & r: regs)
      if(auto * as = decl_attrs(r.item))
        if(symbol g = extension_check(*as)) out.push_back({g, r.item});
    if(symbol g = extension_check(db.as<n_module>(regs.front().module).attrs)) out.push_back({g, regs.front().item});
    return out;
  }
  void check_extension(symbol entity, std::size_t k) {
    auto const & regs = entity_regs[entity.value];
    node_id ext = regs[k].item;
    for(auto const & [g, decl]: extension_checks(regs))
      if(decl != ext) run_extension_check(g, ext, decl, entity);
  }
  // All registrations - in order; during elaboration, up to ct modules (checking -
  // language code: they need registered functions). Only at the root: the sample and
  // parsing process see the same program.
  void check_extensions() {
    if(extensions_checked) return;
    extensions_checked = true;
    if(!extension_any || root != this) return;
    for(std::size_t i = 0; i < ext_order.size(); ++i) {
      auto [e, k] = ext_order[i];
      check_extension(entity_regs[e][k].entity, k);
    }
  }
  // g (extension, substance) - node code; false demand - parsing error on the
  // extension announcement (checking place - mark).
  void run_extension_check(symbol g, node_id ext, node_id decl, symbol entity) {
    span at = db[ext].where;
    if(!functions.contains(g)) fail(cat("[[extension(", str(g), ")]]: нет функции-проверки ", str(g)), db[decl].where);
    stack.push_back({at, cat("проверка расширения ", str(entity), " — ", str(g)), 0, 0});
    struct pop_site {
      std::vector<call_site> & s;
      ~pop_site() { s.pop_back(); }
    } pop{stack};
    try {
      flow f = call_hook(g, {}, {{sym("расширение"), code_value(ext)}, {sym("сущность"), code_value(decl)}}, at, 0, 0);
      if(f.kind == flow_kind::thr) uncaught(f);
    } catch(diagnostic const & d) {
      if(d.type != "require_failed") throw;
      diagnostic e(d.what(), at);
      e.hook = true;
      e.fixed = true;
      if(d.where.begin != at.begin || d.where.end != at.end) e.note(cat("условие ", str(g)), d.where);
      for(auto const & t: d.trace) e.trace.push_back(t);
      throw e;
    }
  }
  node_id module_of(node_id n) const {
    while(n && !db.is<n_module>(n)) n = db[n].parent;
    return n;
  }
  // Modules visible to the extension control module m: it itself, prelude, and its use
  // (transitively) are the context of determining its code.
  bool definition_sees(node_id m, node_id other) {
    auto [it, fresh] = definition_scope.try_emplace(m.value);
    if(fresh) {
      std::vector<node_id> todo{m};
      while(!todo.empty()) {
        node_id x = todo.back();
        todo.pop_back();
        if(!it->second.insert(x.value).second) continue;
        for(node_id item: db.as<n_module>(x).items)
          if(db.is<n_use>(item))
            if(node_id dep = find_module(db, db.as<n_use>(item).key)) todo.push_back(dep);
      }
      if(db.prelude) it->second.insert(db.prelude.value);
    }
    return it->second.contains(other.value);
  }

  // Hygiene - appearance, not renaming:
// 
// Variable
  // The module of foreign code is not visible: x = e in the body of the construction prelude (k = i
  // in for) announces its own, and does not write in ct k programs. *My:* visibility is like that of
  // extension control (your module, its use, prelude); it is checked by "declar or assign".
  bool foreign_global(value const * c, node_id at, scope * env, symbol name) {
    if(global_home.empty()) return false;
    // alien - only the name found in the domain of modules: parameter - alias of the caller cell
    // (text(runs) - n - alias of the run cell) - its name of the function
    for(scope * e = env; e && e != globals; e = e->parent)
      for(auto const & v: e->vars)
        if(v.first == name) return false;
    auto it = global_home.find(c);
    if(it == global_home.end()) return false;
    node_id m = module_of(at);
    if(m && m != it->second && vm_root->service_modules.contains(it->second.value)) return true; // Service status - only its code
    return m && !sees_directly(m, it->second, name); // use is non-transitive - and for variable modules (tail of step 10)
  }

  // entity operator node: infix+/prefix-/postfix++.
  symbol operator_entity(n_op const & o) {
    uint32_t k = o.args.size() == 2 ? 1 : o.postfix ? 2
                                                    : 0;
    if(o.op.value >= op_entities.size()) op_entities.resize(o.op.value + 1);
    symbol & e = op_entities[o.op.value][k]; // by symbol number: no hash on each operator
    if(!e) e = sym(cat(k == 1 ? "infix" : k == 2 ? "postfix"
                                                 : "prefix",
                  str(o.op)));
    return e;
  }

  // The prelude operator, whose body is built-in primitive over parameters in order
  // (operator (a) + (b) => add(a, b)): the VM operation is executed directly.
  bool intrinsic_operator(node_id f) const {
    auto const & fn = db.as<n_fn>(f);
    if(!fn.expr_body || !db.is<n_call>(fn.body) || !db.is<n_ident>(db.as<n_call>(fn.body).callee)) return false;
    static constexpr std::array prims{"index", "add", "sub", "mul", "div", "rem", "eq", "ne", "lt", "gt", "le", "ge", "and", "or", "neg", "not", "band", "bor", "bxor", "shl", "shr"};
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
      if(n.size() > role.size() && n.starts_with(role) && std::string_view("+-*/%<>=!&|^~?@$\\[").contains(n[role.size()])) return true;
    return false;
  }

  // Operators declared in blocks (operator in the body of a function or structure) are
  // local functions: the operator node looks for them in its area.
  void note_local_ops(node_id n, bool nested = false) {
    if(nested && db.is<n_fn>(n) && db.is<n_block>(db[n].parent) && !db.as<n_fn>(n).scoped && operator_name(str(db.as<n_fn>(n).name))) local_ops.insert(db.as<n_fn>(n).name.value);
    db.for_each_child(n, [&](node_id c) { note_local_ops(c, true); });
  }
  // The parsing rule (operator, syntax, construction) is not used.
  bool rule_decl(node_id n) const {
    if(!db.is<n_fn>(n) || db.as<n_fn>(n).scoped) return false;
    auto const & f = db.as<n_fn>(n);
    if(!f.form.empty() || operator_name(str(f.name))) return true;
    return std::ranges::any_of(f.params, [&](node_id p) { return db.as<n_param>(p).clause || db.as<n_param>(p).mode == param_mode::expr; });
  }
  // e - call block (block, block with parameters, tail instruction, pack clause) - as in parsing
  bool block_arg(node_id e) const {
    bool block = db.is<n_block>(e) || db.is<n_raw>(e) || (db.is<n_lambda>(e) && db.as<n_lambda>(e).block);
    node_id c = db[e].parent;
    if(c && db.is<n_construct>(c)) return block && db.is<n_call>(db[c].parent) && !db.as<n_call>(db[c].parent).words.empty();
    if(!c || !db.is<n_call>(c)) return false;
    auto const & k = db.as<n_call>(c);
    return block || (k.tail && !k.args.empty() && k.args.back() == e);
  }

  // Announcement in current elaboration: from a module or generated emit. Implicit parameters (O9, step
  // 13) ---
  //
  // 
// 
// 
// implicit [T]x - last parameters of the function: in call
  // They are not written, the argument is the name x at the call site (lexically, then in the caller -
  // the call environment, O4); written explicitly - the usual argument. The base (array) is implicit,
  // the index is explicit and narrow. My word, "last," a clear argument in their place. node-names are
  // outputs, at the call, once: so the augmented call is seen by both parsing and the machine, and JIT.
  std::unordered_set<uint32_t> implicit_fns; // function
  std::map<std::pair<uint32_t, uint32_t>, node_list> implicit_args; // (challenge, function) → arguments
  static uint32_t implicit_count(ast_db const & db, node_id f) {
    uint32_t k = 0;
    for(node_id p: db.as<n_fn>(f).params) k = db.as<n_param>(p).mode == param_mode::implicit ? k + 1 : 0;
    return k;
  }
  void note_implicit(node_id f) {
    auto const & ps = db.as<n_fn>(f).params;
    uint32_t k = implicit_count(db, f), all = 0;
    for(node_id p: ps) all += db.as<n_param>(p).mode == param_mode::implicit;
    if(all != k) fail("неявные параметры (implicit) — последними: дальше — только неявные", db[f].where);
    if(k) implicit_fns.insert(f.value);
  }
  // Call arguments with implicit ones: there is overload, which lacks the written ones exactly for
  // its implicit ones, and there is no one that takes them as is.
  node_list const & with_implicit(std::vector<node_id> const & fns, node_list const & args, uint32_t call) {
    if(implicit_fns.empty() || !call) return args;
    node_id pick;
    for(node_id f: fns) {
      std::size_t n = db.as<n_fn>(f).params.size();
      if(n == args.size() || rest_param(f)) return args;
      if(implicit_fns.contains(f.value) && n - implicit_count(db, f) == args.size() && !pick) pick = f;
    }
    if(!pick) return args;
    auto key = std::pair{call, pick.value};
    if(auto it = implicit_args.find(key); it != implicit_args.end()) return it->second;
    node_list full = args;
    auto const & ps = db.as<n_fn>(pick).params;
    for(std::size_t i = args.size(); i < ps.size(); ++i) {
      node_id x = db.derive(n_ident{db.as<n_param>(ps[i]).name}, db[node_id(call)].where, derived_nodes);
      db[x].parent = node_id(call); // place at the call: module, visibility
      full.push_back(x);
    }
    return implicit_args[key] = std::move(full);
  }

  void register_item(node_id item) {
    if(db.is<n_fn>(item)) note_implicit(item);
    if(record_of(db, item)) { // Syntax (if, loop, return...) - projection node
      note_extension(db.as<n_fn>(item).name, item); // rule parsing - entity module
      return;
    }
    if(registering && extension_any) note_rules(item);
    if(db.is<n_extend>(item) && db.as<n_extend>(item).of && !db.is_struct(item)) // T Name { … } : from T VM knows struct
      fail(cat("нет вида сущности ", str(db.as<n_extend>(item).of), ": ", str(db.as<n_extend>(item).name), " — инстанс struct"), db[item].where);
    if(extension_any) {
      if(db.is<n_fn>(item)) note_extension(db.as<n_fn>(item).name, item);
      else if(db.is<n_extend>(item)) {
        auto const & e = db.as<n_extend>(item);
        note_extension(e.target ? db.as<n_fn>(node_id(e.target)).name : e.name, item);
        if(!e.target && !db.is_struct(item)) // function in the name { … } - overload
          for(node_id i: e.items)
            if(db.is<n_fn>(i)) note_extension(db.as<n_fn>(i).name, i);
      }
    }
    if(db.is<n_fn>(item) && operator_name(str(db.as<n_fn>(item).name)) && !intrinsic_operator(item))
      user_ops.insert(db.as<n_fn>(item).name.value);
    note_local_ops(item);
    if(db.is<n_fn>(item)) functions[db.as<n_fn>(item).name].push_back(item);
    else if(db.is_struct(item)) {
      auto const & st = db.as<n_extend>(item);
      // Tag above machine representation: struct i64 { __int64 value; } (§5.8, O15) - name of the
      // presentation, not aggregate: value is the machine number itself
      // 
      if(st.items.size() == 1 && db.is<n_var>(st.items[0]) && db.is<n_ident>(db.as<n_var>(st.items[0]).type) && str(db.as<n_var>(st.items[0]).name) == "value" && !is_node_kind(str(st.name)))
        if(uint32_t m = type_registry::machine(str(db.as<n_ident>(db.as<n_var>(st.items[0]).type).name))) {
          if(types.bind_tag(m, str(st.name))) return;
          // The second type over the same view (struct meters { __int64 value; }) is its own type,
          // not the same i64:
//;
          // . *My:* value is an aggregate with a value field (the first tag has the machine number
          // itself)
          if(types.builtin(str(st.name)))
            fail(cat("тип ", str(st.name), ": имя занято типом VM"), db[item].where);
        }
      structs[st.name] = item;
      if(st.base) types.set_parent(types.named(str(st.name)), types.named(str(st.base)));
    }
    else if(db.is<n_extend>(item)) { // fields and attributes to entity, functions to overload
      auto const & e = db.as<n_extend>(item);
      if(e.target) { // overload of a specific function: under its current name
        symbol name = db.as<n_fn>(node_id(e.target)).name;
        for(node_id i: e.items) functions[name].push_back(i);
        return;
      }
      for(node_id a: e.attrs) ext_attrs[e.name].push_back(a);
      bool kind = program_kind(db, str(e.name)).has_value(); // ast.twice { eval(expr x) => ...; } - members of the species, not overload
      for(node_id i: e.items) {
        if(db.is<n_fn>(i)) (kind ? kind_members[e.name] : functions[db.as<n_fn>(i).name]).push_back(i);
        else ext_fields[e.name].push_back(i);
      }
    }
  }

  // Elaboration: modular ct { } are executed once, in module order, before main and before parsing
  // (instantation order, D14). They can generate ads (emit) - those are registered in this
  // elaboration. The process takes the elaboration of the VM root (step 12.3b): the code is the same -
  // the results are the same (ct values, ct instances, generated, state cells and services); the base
  // does not touch, so the process can be started from the background band (the code writes only the
  // main one - its elaboration has already passed)
  void adopt_elaboration(interpreter & from) {
    if(elaborated) return;
    elaborated = true;
    if(setup_error) throw *setup_error;
    for(auto const & [k, v]: from.ct_memo) ct_memo.emplace(k, v);
    for(symbol nm: from.ct_names)
      if(value * c = from.globals->find(nm)) {
        value * cell = globals->declare(nm, *c);
        ct_cells.insert(cell);
        if(auto it = from.global_home.find(c); it != from.global_home.end()) global_home[cell] = it->second;
        ct_names.push_back(nm);
      }
    for(node_id m: module_order)
      for(node_id item: db.as<n_module>(m).items) {
        bool state = db.is<n_var>(item) && !db.as<n_var>(item).attrs.empty();
        if(state) // [[state]] is a program cell
          if(value * c = from.globals->find(db.as<n_var>(item).name)) {
            globals->alias(db.as<n_var>(item).name, c);
            global_home[c] = m;
          }
        if(is_service(m)) note_service(m, item); // service cell at the root of VM
      }
    for(uint32_t x: from.inst_registered)
      if(inst_registered.insert(x).second) register_item(node_id(x));
    pass_ran = from.pass_ran;
  }

  void elaborate() {
    if(elaborated) return;
    elaborated = true;
    if(setup_error) throw *setup_error;
    // VM errors are one hierarchy: catch(vm_error e) catches any
    for(std::string_view e: {"out_of_bounds", "div_by_zero", "require_failed", "unpack_failed", "closed_stream", "deadlock", "invalidated_alias", "io_error", "overflow", "stack_overflow"})
      types.set_parent(types.named(e), types.named("vm_error"));
    check_extensions(); // [[extension(g)]] to ct modules: Extensions are already known
    // Mutations of the previous elaboration code are rolled back: the ct module is executed
    // again over the written
// A remote source doesn't change anything anymore.
    struct elab_hold {
      interpreter & vm;
      ast_db::elab_mark mark;
      bool done = false;
      ~elab_hold() {
        vm.elaborating = false;
        vm.ct_source = {};
        vm.db.end_elaboration(mark);
        if(done && !mark.nested) vm.db.elab_edits = vm.db.edits; // result is valid until edited
      }
    } eh{*this, detached ? db.nested_elaboration() : db.begin_elaboration()};
    elab_base = db.births;
    elab_nested = eh.mark.nested;
    elaborating = true;
    for(node_id m: module_order) { // ct modules - in order of modules (used earlier)
      for(node_id item: db.as<n_module>(m).items) {
        ct_source = item;
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
          value * cell = globals->declare(v.name, f.v);
          ct_cells.insert(cell);
          global_home[cell] = m;
          ct_names.push_back(v.name);
        }
        if(db.is<n_var>(item) && !db.as<n_var>(item).attrs.empty()) declare_state(item); // [[state]] x = e
        if(is_service(m)) note_service(m, item);
      }
    }
    transform_pass(); // Transformers - after ct modules: generated is also rewritten
    elaborating = false;
    ct_source = {};
    if(!eh.mark.nested && instance_ct()) instantiation_pass();
    eh.done = true;
    // generated by ct code instance - module elements (parsing of this or previous elaboration)
    for(node_id m: module_order)
      for(node_id item: db.as<n_module>(m).items)
        if(db.inst_emits.contains(item.value) && inst_registered.insert(item.value).second) register_item(item);
  }

  // --- Transformers: Rule (§7.4) ---
  //
  // Author's decision (2026-10-17).
// 
// 
  // . The function with the attribute [[transform]] and the parameter of the form node (ast.call
  // c; without the type - any node) is called when elaborating for each such node modules of the
  // program; returned the code - the node becomes it (change with the source - function: editing
  // or removing the transformer rolls back its changes, as in ct-code), another - the node as it
  // was. The rewritten node is again viewed by all transformers except those already applied to
  // it; then by its children. My:* attribute, order (in the order of modules, top-down), limit -
  // 100,000 applications.
  std::vector<node_id> transforms() {
    std::vector<node_id> out;
    for(node_id m: module_order)
      for(node_id f: db.as<n_module>(m).items)
        if(db.is<n_fn>(f))
          for(node_id a: db.as<n_fn>(f).attrs)
            if(db.is<n_ident>(a) && str(db.as<n_ident>(a).name) == "transform") out.push_back(f);
    return out;
  }
  bool transform_fits(node_id t, node_id c) {
    auto const & fn = db.as<n_fn>(t);
    if(fn.params.size() != 1) fail("[[transform]]: один параметр — узел (ast.call c)", db[t].where);
    node_id ty = db.as<n_param>(fn.params[0]).type;
    if(!ty) return true;
    if(!db.is<n_ident>(ty)) return false;
    std::string_view want = str(db.as<n_ident>(ty).name);
    std::string_view k = lang_kind_of(db[c]);
    return k == want || types.is_a(std::string(k), want);
  }
  void transform_pass() {
    auto ts = transforms();
    if(ts.empty()) return;
    uint32_t budget = 100000;
    std::set<std::pair<uint32_t, uint32_t>> applied;
    auto visit = [&](auto && self, node_id c) -> void {
      if(!c) return;
      if(db.is<n_fn>(c) && std::ranges::contains(ts, c)) return; // The transformer does not rewrite its code.
      for(bool again = true; again;) {
        again = false;
        for(node_id t: ts) {
          if(applied.contains({c.value, t.value}) || !transform_fits(t, c)) continue;
          applied.insert({c.value, t.value});
          if(budget-- == 0) fail("трансформаторы: больше 100000 применений — правило не сходится", db[t].where);
          auto const & fn = db.as<n_fn>(t);
          scope * inner = new_scope(globals);
          scope_hold hold{*this, inner};
          inner->declare(db.as<n_param>(fn.params[0]).name, code_value(c));
          ct_source = t;
          ++ct_depth;
          flow r = run_frame(fn.body, inner, fn.name, fn.outline);
          --ct_depth;
          if(r.kind == flow_kind::thr) uncaught(r);
          if(!r.v.is<agg_v>() || types.name(r.v.as<agg_v>().type) != "code") continue;
          node_id to = code_node(r.v, db[t].where);
          if(!to || to == c) continue;
          db.mutate(c, db[to].kind, t.value + 1);
          db.link_parents(c);
          again = true; // rewritten, again to all except those used
          break;
        }
      }
      std::vector<node_id> kids;
      db.for_each_child(c, [&](node_id x) { kids.push_back(x); });
      for(node_id x: kids) self(self, x);
    };
    for(node_id m: module_order)
      if(m != db.prelude) visit(visit, m);
  }

  // --- ct code instance changes the code (§7.4) ---
  // 
// 
// 
// 
  // . Elaboration - ct modules, then parsing programs from main in canonical order (§7.3): ct code
  // instance (ct { } in body functions, ct functions and caused from them) changes node and
  // generates ads. In runtime, code change is an error.
  //

  // Is there a ct code in the program functions (not prelude): otherwise parsing is not needed.
  bool instance_ct() {
    if(!functions.contains(sym("main"))) return false;
    for(uint32_t i = 0; i < db.size(); ++i) {
      if(!db.is<n_ct>(node_id(i))) continue;
      bool in_fn = false;
      node_id top;
      for(node_id x(i); x; x = db[x].parent) {
        in_fn = in_fn || db.is<n_fn>(x);
        top = x;
      }
      if(in_fn && top != db.prelude && std::ranges::contains(module_order, top)) return true;
    }
    return false;
  }

  // parsing programs in elaboration: as the canonical order ct (canonical_ct), but its ct code
  // instance changes the code. Running takes its ct results in places and the final ct variables;
  // parsing for JIT and check - the result (ct instance code no longer changes).
  void instantiation_pass() {
    interpreter & a = level_runs.emplace_back(db, mod, std::vector<std::string>{"ct"}, mode::check, std::nullopt, nullptr, this);
    a.fuel = 2000000;
    a.instantiating = true;
    a.pass_base = db.births;
    // parsing error, which did not change the code, is not an elaboration error: the execution
    // will find it itself, in its place (as canonical_ct without ct-variables); changed the code
    // parsing, ct-variables and hooks are part of the program (*mine*).
    try {
      a.run_main();
    } catch(diagnostic const & d) {
      if(!a.code_changes.empty() || a.code_failed || !ct_names.empty() || d.hook) throw;
      db.elab_pass_error = d;
      return;
    } catch(std::bad_alloc const &) {
      return;
    }
    for(auto const & [k, v]: a.ct_memo) db.elab_ct.insert(k);
    db.elab_changes = a.code_changes;
    if(checking()) return; // parsing (check) executes ct itself - the order inspector sees it
    take_canonical(a);
    pass_ran = true;
  }

  // --- Program status (§12.2) ---
  // 
// 
// [[state]] x = e —
  // The root is in the non-volatile level: the file holds the VM (the program has no paths), the
  // pages are in the same addresses. Author's decision 2026-10-01. *My:* file - next to the program
  // (.state), roots - by name, repair at opening (memory level, type numbers and field names),
  // level - 1 TB of addresses.

  address_space & program_state(span where) {
    interpreter & r = *root;
    if(r.state_level) return *r.state_level;
    std::string base_name = r.prog_path.substr(r.prog_path.find_last_of('/') + 1);
    std::string file = state_dir.empty() ? r.prog_path + ".state" : state_dir + "/" + base_name + ".state";
    r.state_level.emplace();
    std::string why = open_state(*r.state_level, file);
    if(!why.empty()) {
      r.state_level.reset();
      fail(cat("состояние программы: ", why, " (", file, ")"), where);
    }
    auto * h = reinterpret_cast<state_header *>(r.state_level->lo);
    if(h->nroots) { // level from the previous launch: numbers to this
      std::map<uint64_t, type_id> tmap;
      std::map<uint64_t, symbol> smap;
      for(uint64_t i = 0; i < h->ntypes; ++i) tmap[h->types[i].id] = types.named(h->types[i].name);
      for(uint64_t i = 0; i < h->nsyms; ++i) smap[h->syms[i].id] = sym(h->syms[i].name);
      for(uint64_t i = 0; i < h->nroots; ++i) fix_state(*reinterpret_cast<value *>(r.state_level->lo + h->roots[i].id), &*r.state_level, tmap, smap);
    }
    return *r.state_level;
  }

  void fix_state(value & x, address_space * lv, std::map<uint64_t, type_id> const & tmap, std::map<uint64_t, symbol> const & smap) {
    auto home = [&](words & w) { w.home = lv; };
    if(x.is<str_v>()) home(x.as<str_v>().w);
    else if(x.is<type_v>()) x.as<type_v>().type = tmap.at(x.as<type_v>().type.value);
    else if(x.is<array_v>()) {
      array_v & a = x.as<array_v>();
      home(a.dense);
      home(a.items.raw);
      for(auto & e: a.items) fix_state(e, lv, tmap, smap);
    } else if(x.is<agg_v>()) {
      agg_v & a = x.as<agg_v>();
      a.type = tmap.at(a.type.value);
      home(a.fields.raw);
      for(auto & [k, f]: a.fields) {
        k = smap.at(k.value);
        fix_state(f, lv, tmap, smap);
      }
    }
  }

  // --- VM services (§9.5, step 12.4) ---
  //
  //
//  
// [[service]] module k; - service: its variables are one state per VM (all
  // processes and bands see only cells, functions are called ordinary call (inline, caller types); in
  // bands, the call is under the service lock (*my*: the service is not waiting inside). Its struct on
  // the outside is descriptors: you can hold and transmit, you can create and read fields - only the
  // service itself; the names of its variables are not visible on the outside. The state of the
  // service is not part of the process (the dump does not carry it). Resources are provided by the
  // author (step 12.4):
// TZ_GRANT=fs,net (tz run
  // -grant fs,net); not given - "0 devices", as in the background process.
  std::set<std::string, std::less<>> grants;
  static std::set<std::string, std::less<>> granted_by_user() {
    std::set<std::string, std::less<>> out;
    if(char const * g = std::getenv("TZ_GRANT"))
      for(std::string_view t = g; !t.empty();) {
        std::size_t c = t.find(',');
        if(std::string_view k = t.substr(0, c); !k.empty()) out.emplace(k);
        t = c == std::string_view::npos ? std::string_view() : t.substr(c + 1);
      }
    return out;
  }
  bool granted(std::string_view kind) const { return !background && vm_root->grants.contains(kind); }
  std::set<uint32_t> service_modules;
  std::unordered_set<uint32_t> service_fns;
  std::map<std::string, uint32_t, std::less<>> service_types; // name struct → module service
  std::deque<value> service_cells; // VM Root: Service Status Cells
  std::map<uint32_t, value *> service_cell_of; // module element
  bool is_service(node_id m) const {
    for(node_id a: db.as<n_module>(m).attrs)
      if(db.is<n_ident>(a) && str(db.as<n_ident>(a).name) == "service") return true;
    return false;
  }
  void note_service(node_id m, node_id item) {
    interpreter & v = *vm_root;
    if(&v == this) {
      v.service_modules.insert(m.value);
      if(db.is<n_fn>(item)) v.service_fns.insert(item.value);
      if(db.is_struct(item)) v.service_types.emplace(std::string(str(db.as<n_extend>(item).name)), m.value);
    }
    bool var = db.is<n_var>(item) && !db.as<n_var>(item).ct && db.as<n_var>(item).attrs.empty();
    bool assign = db.is<n_op>(item) && str(db.as<n_op>(item).op) == "=" && db.is<n_ident>(db.as<n_op>(item).args[0]);
    if(!var && !assign) return;
    symbol name = var ? db.as<n_var>(item).name : db.as<n_ident>(db.as<n_op>(item).args[0]).name;
    node_id init = var ? db.as<n_var>(item).init : db.as<n_op>(item).args[1];
    value * cell = nullptr;
    if(auto it = v.service_cell_of.find(item.value); it != v.service_cell_of.end() && !checking()) cell = it->second; // process
    else {
      flow f = init ? eval(init, globals, 0) : ok();
      if(f.kind == flow_kind::thr) uncaught(f);
      if(checking()) cell = globals->declare(name, erase(f.v)); // parsing - run-time: State changes challenges
      else {
        cell = &v.service_cells.emplace_back(std::move(f.v));
        v.service_cell_of[item.value] = cell;
      }
    }
    if(!checking()) globals->alias(name, cell);
    global_home[cell] = m;
  }
  // Service Descriptor: Create or read fields - only the code of the service itself (and prelude VM)
  void service_guard(std::string_view type, node_id at, std::string_view what) {
    auto const & h = vm_root->service_types;
    if(h.empty()) return;
    auto it = h.find(type);
    if(it == h.end()) return;
    node_id m = module_of(at);
    if(m && m != node_id(it->second) && m != db.prelude)
      fail(cat("дескриптор ", type, " сервиса ", str(db.as<n_module>(node_id(it->second)).name), ": ", what, " — только в сервисе («секреты остаются в сервисе»)"), db[at].where);
  }

  void declare_state(node_id item) {
    auto const & v = db.as<n_var>(item);
    if(checking() || !vm_root->owns_state) { // parsing - run-time (value from past launches); assistants (compiler in language) - without a file
      flow f = eval(v.init, globals, 0);
      if(f.kind == flow_kind::thr) uncaught(f);
      global_home[globals->declare(v.name, checking() ? erase(f.v) : f.v)] = module_of(item);
      return;
    }
    address_space & lv = program_state(db[item].where);
    auto * h = reinterpret_cast<state_header *>(lv.lo);
    value * cell = nullptr;
    for(uint64_t i = 0; i < h->nroots && !cell; ++i)
      if(str(v.name) == h->roots[i].name) cell = reinterpret_cast<value *>(lv.lo + h->roots[i].id);
    if(!cell) { // first run: initializer, cell - in level pages
      if(h->nroots >= 256 || str(v.name).size() >= 64) fail("состояние программы: не больше 256 переменных, имя — до 63 байт", db[item].where);
      flow f = eval(v.init, globals, 0);
      if(f.kind == flow_kind::thr) uncaught(f);
      value x;
      try {
        x = adopt_into(std::move(f.v), &lv);
      } catch(diagnostic const & d) {
        fail(d.what(), db[item].where);
      }
      cell = new(lv.take_small(address_space::class_of(uint32_t((sizeof(value) + 7) / 8)))) value(std::move(x));
      auto & r = h->roots[h->nroots++];
      r.id = uint64_t(reinterpret_cast<std::byte *>(cell) - lv.lo);
      std::memcpy(r.name, str(v.name).data(), str(v.name).size());
      r.name[str(v.name).size()] = 0;
    }
    globals->alias(v.name, cell);
    global_home[cell] = module_of(item);
    vm_root->state_cells.insert(cell);
  }

  // Number → name tables for root field types and names; metadata per disk
  void save_program_state() {
    if(!state_level) return;
    auto * h = reinterpret_cast<state_header *>(state_level->lo);
    std::set<uint32_t> ts, ss;
    auto walk = [&](auto && self, value const & x) -> void {
      if(x.is<type_v>()) ts.insert(x.as<type_v>().type.value);
      else if(x.is<array_v>())
        for(auto const & e: x.as<array_v>().r().items) self(self, e);
      else if(x.is<agg_v>()) {
        ts.insert(x.as<agg_v>().type.value);
        for(auto const & [k, f]: x.as<agg_v>().fields) {
          ss.insert(k.value);
          self(self, f);
        }
      }
    };
    for(uint64_t i = 0; i < h->nroots; ++i) walk(walk, *reinterpret_cast<value const *>(state_level->lo + h->roots[i].id));
    auto put = [](state_header::named & n, uint32_t id, std::string_view name) {
      n.id = id;
      size_t len = std::min<size_t>(name.size(), 63);
      std::memcpy(n.name, name.data(), len);
      n.name[len] = 0;
    };
    h->ntypes = 0;
    for(uint32_t t: ts)
      if(h->ntypes < 1024) put(h->types[h->ntypes++], t, types.name(type_id(t)));
    h->nsyms = 0;
    for(uint32_t k: ss)
      if(h->nsyms < 4096) put(h->syms[h->nsyms++], k, str(symbol(k)));
    save_state(*state_level);
    close_state(*state_level);
    state_level.reset();
    state_cells.clear();
  }

  // Unit fields: declaration + extension (in module order).
  std::vector<node_id> fields_of_struct(symbol name) const {
    std::vector<node_id> out;
    if(auto it = structs.find(name); it != structs.end()) {
      if(symbol b = db.as<n_extend>(it->second).base) out = fields_of_struct(b); // basefields - first
      for(node_id f: db.as<n_extend>(it->second).items) out.push_back(f);
    }
    if(auto it = ext_fields.find(name); it != ext_fields.end())
      for(node_id f: it->second) out.push_back(f);
    return out;
  }

  // Launch main(). Returns the exit code.
  int run_main() {
    if(setup_error) throw *setup_error; // Modules: no module
    auto it = functions.find(sym("main"));
    if(it == functions.end()) throw diagnostic("нет функции main", {});
    owns_state = true; // Execution of the program: it opens its state
    elaborate();
    if(!checking()) check_before_run();
    it = functions.find(sym("main"));
    call_site site{db[it->second.front()].where, "main"};
    flow r;
    struct save_on_exit { // Program state on disk and in case of error
      interpreter & vm;
      ~save_on_exit() { vm.save_program_state(); }
    } saving{*this};
    try {
      r = call_overloaded(it->second, {}, globals, 0, site);
      if(r.kind == flow_kind::thr) uncaught(r);
      if(!checking()) join_all(); // VM awaits fiber program
      stop_lanes(false);
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
    bool throws = false; // run-time-check (boundary, requires...): can throw
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

  // node values for all instance : "type\literal", literal is empty - the value of runtime. Type is two parts: "the part that behaves
  // differently in compiltime and the part that behaves differently in runtime".
// - The editor shows both.
  std::map<uint32_t, std::vector<std::string>> node_values() const {
    std::map<uint32_t, std::vector<std::string>> out;
    for(auto const & e: journal) {
      if(!e.seen) continue;
      auto & vs = out[e.node];
      auto add = [&](value const & v) { // “type/t value” (runtime is empty)
        std::string t = cat(types.describe(v), "\t", literal(v).value_or(""));
        if(!std::ranges::contains(vs, t)) vs.push_back(t);
      };
      if(e.v.is<split_v>())
        for(auto const & a: e.v.as<split_v>().alts) add(a);
      else add(e.v);
    }
    return out;
  }

  // Show the meaning as literal, if possible.
  std::optional<std::string> literal(value const & v) const {
    if(!v.known()) return std::nullopt;
    if(v.is<int64_t>() || v.is<double>() || v.is<bool>()) return show_v(v);
    if(v.is<str_v>()) return cat(in_quotes(v.as<str_v>()));
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
    for(uint32_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
      auto const & p = db.as<n_param>(fn.params[i]);
      if(p.type && db.is<n_pat>(p.type)) { // parameter-sample: names - unpacking the sample value (like a call)
        unpack_into(p.type, args[i], inner, inner, 0, db[fn.params[i]].where);
        inner->declare(p.name, args[i]); // The value itself is a slot of a machine code parameter
        continue;
      }
      inner->declare(p.name, args[i]);
    }
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

  // parsing before execution:
// Hook error parsing [[g(…)]] (inside prelude)
  // Only he sees - it stops the start; other errors parsing execution finds itself in its place
  // (as before). ct-state - in canonical order, not along the execution branches; for JIT - also
  // the types of thread elements at the place of creation.
  void check_before_run() {
    bool streams = jit && calls(mod, sym("stream"));
    if(pass_ran && !streams) return; // parsing elaboration has already performed ct in canonical order
    // ct instance changed the code (elaboration was carried out by another interpreter): their results
    // are from the canonical parsing, the execution of the ct code instance does not perform again
    if(!ct_names.empty() || streams || !db.elab_ct.empty()) {
      canonical_ct();
      return;
    }
    if(!program_hooks()) return;
    interpreter a(db, mod, std::vector<std::string>{"ct"}, mode::check, std::nullopt, nullptr, this);
    a.fuel = 2000000;
    try {
      a.run_main();
    } catch(diagnostic const & d) {
      if(d.hook) throw;
    } catch(std::bad_alloc const &) { // parsing did not fit in addresses (copies of large arrays in journal observations) - execution without it
    }
  }

  // Hook parsing for a program function ([[g(…)]], g is a language function). Without them,
  // parsing doesn’t add anything before launch - the launch doesn’t pay for it (mys; the
  // prelude hooks don’t fail: the effects of the supports only change the ct state).
  bool program_hooks() const {
    for(uint32_t i = 0; i < db.size(); ++i) {
      node_id f(i);
      if(!db.is<n_fn>(f) || db.as<n_fn>(f).attrs.empty() || db.in_prelude(f)) continue;
      for(node_id a: db.as<n_fn>(f).attrs)
        if(db.is<n_call>(a) && db.is<n_ident>(db.as<n_call>(a).callee) && functions.contains(db.as<n_ident>(db.as<n_call>(a).callee).name)) return true;
    }
    return false;
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
      std::size_t first = vm_root->streams.size();
      try {
        a.run_main();
      } catch(diagnostic const & d) {
        if(!ct_names.empty() || d.hook) throw; // ct is part of the program; thread types are just a JIT hint
        stream_shapes.clear();
        return;
      }
      last = &a;
      // parsing threads (root common): type of elements at the place of creation; different - not type
      std::map<std::pair<uint64_t, uint64_t>, value> found;
      std::set<std::pair<uint64_t, uint64_t>> mixed;
      for(std::size_t k = first; k < vm_root->streams.size(); ++k) {
        auto const & s = vm_root->streams[k];
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
    take_canonical(*last);
  }
  // results ct parsing a: by call location and final ct variables
  void take_canonical(interpreter & a) {
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
    if(!db.elab_changes.empty()) { // The result of elaboration is at the base (it could be done by another interpreter).
      out += "изменения кода ct-кодом экземпляров (элаборация):\n";
      for(auto const & [w, inst, what]: db.elab_changes) out += cat("  ", loc(w), " в ", inst, ": ", what, "\n");
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
  enum class flow_kind : uint8_t { normal, ret, brk, cont, thr, tail }; // thr - second exit path (exception); tail - tail call yourself
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
    address_space * space = nullptr; // Context memory level with_alloc (§12.2)
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
// The meaning of the name src goes to a new place: the transfer or copy(T) is only for the
// resource, without wrapping in flow on the usual path.
#define TZ_TAKE(var, src, v0, env, frame)                         \
  value & var = v0;                                               \
  if(needs_take(src, var)) {                                      \
    flow var##_t = take(src, std::move(var), env, frame);         \
    if(!var##_t.normal()) return var##_t;                         \
    var = std::move(var##_t.v);                                   \
  }

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
    s->caller = nullptr;
    s->fn = {};
    s->book = checking() ? &book : nullptr;
    return s;
  }

  void drop_scope(scope * s) {
    if(s->captured) return;
    s->vars.clear();
    s->storage.clear();
    s->borrows.clear();
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
    while(db.is_index(target) || db.is_member(target))
      target = db.is_index(target) ? db.index_of(target).object : db.member_of(target).object;
    if(!db.is<n_ident>(target)) return;
    value * cell = find(env, db.as<n_ident>(target).name);
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
    uint64_t effects_before = effects, checks_before = checks;
    flow f = std::visit([&](auto const & k) { return eval_node(n, k, env, frame); }, db[n].kind);
    observe(n, f, effects != effects_before, checks != checks_before);
    return f;
  }

  // All changes to the report go through emit: so they can be written to the
  // instance cache and reproduced when hit.
  void emit(report_event ev) {
    // parsing Hooks - Library Ct Code: Only their run-time Checks and Exclusions
    if(hook_depth && ev.kind != report_event::rt_check && ev.kind != report_event::throws) return;
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
    bool throws = false; // run-time is still validated - it can throw
  };

  // A large known array of observation holds a sample of the type, not a copy: a copy for
  // each observation (cycles) exhausted the addresses (growing array - its own band), and to
  // know the values of this size neither the analysis nor the editor need (*my*)
  static constexpr uint32_t observed_items = 256;
  static value observable(value const & v) {
    if(v.is<array_v>() && v.as<array_v>().size() > observed_items) return erase(v);
    return v;
  }
  void observe(node_id n, flow const & f, bool effect, bool throws = false) {
    if(instantiating && !ct_depth) used_rt.insert(n.value);
    if(!f.normal()) journal.push_back({n.value, {}, false, true, effect, ctx, false, throws});
    else journal.push_back({n.value, observable(f.v), true, false, effect, ctx, false, throws});
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
      o.throws |= e.throws;
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
      ruled_hold rh{*this, t, ruled_env(t)};
      flow r;
      try {
        r = eval(t.expr, rh.s, t.frame);
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
    // merging rule - in language (join_rule prelude, M6 step 6): passage 1 - late type of empty
    // array elements, passage 2 - same compile-time / same type; facts - at VM
    std::string tx = types.describe(x);
    std::vector<std::string> tys; // Types of alternatives - once on both passes
    tys.reserve(alts.size());
    for(auto const & y: alts) tys.push_back(types.describe(y));
    std::string key;
    for(int64_t pass = 1; pass <= 2; ++pass)
      for(std::size_t k = 0; k < alts.size(); ++k) {
        auto & y = alts[k];
        std::string const & ty = tys[k];
        bool same_state = state_of(y) == state_of(x);
        bool same_value = pass == 2 && same_state && ty == tx && y.known() && x.known() && show_v(y) == show_v(x);
        key.clear();
        key += char('0' + pass);
        key += char('0' + same_state * 2 + same_value);
        key += ty;
        key += '|';
        key += tx;
        auto it = join_rules.find(key);
        if(it == join_rules.end()) {
          value r = lang_rule(join_fn, "join_rule", {value(pass), value(ty), value(tx), value(same_state), value(same_value)}, {});
          it = join_rules.emplace(key, r.as<int64_t>()).first;
        }
        switch(it->second) {
          case 1: return; // calculus
          case 2: y = erase(y); return; // same type, run-time
          case 3: y = erase(x); return; // array<?> ⊔ array<T>
          default: break;
        }
      }
    alts.push_back(x);
  }
  std::unordered_map<std::string, int64_t> join_rules; // Facts → merger (join_rule)
  node_id join_fn;

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
    if(v.is<rt_v>() && types.describe(v) == types.bool_name()) return std::nullopt;
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
    if(db.is<n_ident>(n) && std::ranges::contains(ct_names, db.as<n_ident>(n).name)) return true; // ct-variable reading - in place
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
    value * found = find(env, x.name);
    if(found && foreign_global(found, n, env, x.name)) found = nullptr; // Hygiene Visibility and Reading: An Alien Module Variable - as No Name
    if(value * cell = found) {
      // ct-variable outside the ct-code: the value at the time of execution in canonical
      // order instantiation (§7.3 para. 2), in place - as ct e.
// 
//.
      if(ct_cells.contains(cell) && ct_depth == 0 && !level_probe) {
        auto key = std::pair{n.value, ctx};
        if(auto it = ct_memo.find(key); it != ct_memo.end()) return ok(it->second);
        if(checking()) ct_memo[key] = *cell;
        return ok(*cell);
      }
      not_hidden(*cell, x.name, db[n].where);
      return read(cell, db[n].where);
    }
    if(str(x.name) == "true") return ok(value(true));
    if(str(x.name) == "false") return ok(value(false));
    if(auto st = structs.find(x.name); st != structs.end()) { // entity struct - value (previously registry: there and others)
      if(module_order.size() > 1)
        if(node_id home = module_of(st->second), m = module_of(n); m && !sees_directly(m, home, x.name)) fail(not_visible("тип", x.name, home), db[n].where);
      return ok(value(type_v{types.named(str(x.name))}));
    }
    if(auto t = types.builtin(str(x.name))) return ok(value(type_v{t}));
    // Type node code - tag (n_op, n_if...):
    if(is_node_kind(str(x.name))) return ok(value(type_v{types.named(str(x.name))}));
    if(auto it = functions.find(x.name); it != functions.end()) {
      for(node_id f: it->second)
        if(inline_required(f))
          fail(cat("функция ", str(x.name), " использует семантику инлайна (expr-параметр или return(…)) и не может быть значением; вызывайте её напрямую"),
              db[n].where);
      return ok(value(fn_v{x.name, {}, nullptr}));
    }
    if(node_id home = unseen_module_var(x.name)) fail(not_visible("переменная", x.name, home), db[n].where);
    fail(cat("неизвестное имя ", str(x.name)), db[n].where);
  }

  // --- blockages ---

  // --- resources: destructor, transfer, copy (§12.2) ---
  //
  // : type with destroy(T x) is a resource; destroy is called at the end of the variable region (not
  // transferred), at the overwritten value and at the discarded time. Destructors - "hooks" to track the
  // time of life;
  //
// A copy of the resource is selected by use: the name is no longer readable
  // - transfer (name hidden), read - copy(T) type
// No copy is an error; no
  // ".clone() is everywhere" (Rust critique) *My:* Destroy/copy names (drop in prelude - shutting down the network peer), life - until the end of
  // the block, resource number: a copy that passed the rule is a mistake in the second destroy, not a double release.
  std::unordered_map<uint32_t, uint8_t> res_live; // Resource number 1 live, 2 released
  // Divided object shared(x) - ref object
// 
// 
// My name is shared.
  // A link to the code), a VM storage slot, each copy has its own resource number. Count - cell-value
  // (tail of step 10): - parsing leads it like any cell (merging branches, cycles): known - folded
  // (shared_count - constant instance), depends on the path - run-time
  //
  struct shared_slot {
    value v;
    value count;
  };
  std::deque<shared_slot> shared_store;
  bool is_shared(value const & v) const { return v.is<agg_v>() && v.as<agg_v>().rid && types.name(v.as<agg_v>().type) == "shared"; } // No type registration: registry does not change in parsing
  shared_slot & slot_of(value const & r) { return shared_store[std::size_t(r.as<agg_v>().fields.begin()->second.as<int64_t>())]; }
  // run-time at parsing - last reference, parsing does not know
  bool count_known(shared_slot const & sl) const { return sl.count.is<int64_t>(); }
  void count_add(shared_slot & sl, int64_t d) {
    if(checking()) book.written(&sl.count, true);
    if(count_known(sl)) sl.count.as<int64_t>() += d;
  }
  value make_shared(value v) {
    shared_store.push_back({std::move(v), value(int64_t(1))});
    if(checking()) book.born(&shared_store.back().count);
    agg_v g{types.named("shared"), {{sym("slot"), value(int64_t(shared_store.size() - 1))}}};
    g.rid = ++res_counter;
    res_live[g.rid] = 1;
    return value(std::move(g));
  }
  uint32_t res_counter = 0;
  uint64_t drops_called = 0;
  // destroy(T x): one overload with structure parameter
  mutable std::size_t res_fns_seen = ~std::size_t{0};
  mutable bool res_fns = false;
  mutable std::size_t res_names_seen = ~std::size_t{0}; // function names for the last "destroy no"
  mutable std::vector<node_id> const * res_list = nullptr; // overload destroy (maps node does not move)
  [[gnu::always_inline]] bool res_any() const {
    if(!res_list && res_names_seen == functions.size()) return !shared_store.empty() || (root && vm_root->regions_alive);
    return res_any_slow();
  }
  [[gnu::noinline]] bool res_any_slow() const {
    if(!shared_store.empty() || (root && vm_root->regions_alive)) return true;
    if(!res_list) {
      if(res_names_seen == functions.size()) return false;
      auto it = functions.find(const_cast<interpreter *>(this)->sym("destroy"));
      if(it == functions.end()) {
        res_names_seen = functions.size();
        return false;
      }
      res_list = &it->second;
    }
    if(res_fns_seen != res_list->size()) {
      res_fns_seen = res_list->size();
      res_fns = std::ranges::any_of(*res_list, [&](node_id f) {
        auto const & fn = db.as<n_fn>(f);
        return fn.params.size() == 1 && db.as<n_param>(fn.params[0]).type && db.is<n_ident>(db.as<n_param>(fn.params[0]).type) && structs.contains(db.as<n_ident>(db.as<n_param>(fn.params[0]).type).name);
      });
    }
    return res_fns;
  }
  bool is_region(value const & v) const { return v.is<agg_v>() && v.as<agg_v>().rid && types.name(v.as<agg_v>().type) == "region"; }
  node_id member_fn_of(std::string_view fname, type_id t) {
    auto it = functions.find(sym(fname));
    if(it == functions.end()) return {};
    std::string_view tn = types.name(t);
    for(node_id f: it->second) {
      auto const & fn = db.as<n_fn>(f);
      if(fn.params.size() != 1) continue;
      node_id pt = db.as<n_param>(fn.params[0]).type;
      if(pt && db.is<n_ident>(pt) && str(db.as<n_ident>(pt).name) == tn) return f;
    }
    return {};
  }
  uint64_t resources_made = 0;
  uint64_t regions_made = 0; // parsing: instance creates region() - JIT leaves it to the interpreter
  void mark_resource(agg_v & g) {
    if(res_any() && member_fn_of("destroy", g.type)) {
      ++resources_made;
      g.rid = ++res_counter;
      res_live[g.rid] = 1;
    }
  }
  // Struct with resource fields (pair {file a; file b;}) is also a resource: its fields are released
  bool has_resource_fields(type_id t) {
    auto it = structs.find(sym(types.name(t)));
    if(it == structs.end()) return false;
    for(node_id f: fields_of_struct(it->first)) {
      node_id ft = db.as<n_var>(f).type;
      if(!ft || !db.is<n_ident>(ft)) continue;
      std::string_view fn = str(db.as<n_ident>(ft).name);
      if(fn == "shared" || (structs.contains(sym(fn)) && (member_fn_of("destroy", types.named(fn)) || has_resource_fields(types.named(fn))))) return true;
    }
    return false;
  }
  // Description type (as in parsing: array<file>)
  bool resource_type(std::string_view n) {
    if(n.starts_with("array<") && n.ends_with(">")) return resource_type(n.substr(6, n.size() - 7));
    if(n == "shared") return true;
    if(!structs.contains(sym(n))) return false;
    type_id t = types.named(n);
    return bool(member_fn_of("destroy", t)) || has_resource_fields(t);
  }
  bool has_resource(value const & v) const {
    if(v.is<agg_v>()) {
      if(v.as<agg_v>().rid) return true;
      for(auto const & [k, x]: v.as<agg_v>().fields)
        if(has_resource(x)) return true;
      return false;
    }
    if(v.is<array_v>()) {
      auto const & a = v.as<array_v>();
      if(a.size() == 0 || !(a.get(0).is<agg_v>() || a.get(0).is<array_v>())) return false; // scalar
      for(uint32_t i = 0; i < a.size(); ++i)
        if(has_resource(a.get(i))) return true;
    }
    return false;
  }
  // Free value resources: your drop, then fields and elements (in reverse order)
  void drop_value(value const & v, span where) {
    if(v.is<array_v>()) {
      auto const & a = v.as<array_v>();
      for(uint32_t i = a.size(); i-- > 0;) drop_value(a.get(i), where);
      return;
    }
    if(!v.is<agg_v>()) return;
    auto const & g = v.as<agg_v>();
    if(g.rid && is_region(v)) { // Memory level: the last holder is liberation if nothing lives in it
      auto it = res_live.find(g.rid);
      if(it == res_live.end()) fail("уровень памяти уже освобождён: копия прошла мимо правила", where);
      res_live.erase(it);
      address_space & a = vm_root->regions[std::size_t(g.fields.begin()->second.as<int64_t>())];
      if(a.holders > 0 && --a.holders == 0) {
        if(a.live > 0)
          fail(cat("уровень памяти освобождается, а в нём живут значения (", a.live, "): они пережили бы свою память — «Неявный регион требует отслеживания - всего что в него входит» "), where);
        a.destroy();
      }
      return;
    }
    if(g.rid && is_shared(v)) { // shared: dec, the latter releases the stored
      auto it = res_live.find(g.rid);
      if(it == res_live.end()) fail("разделяемый объект уже освобождён: копия прошла мимо правила", where);
      res_live.erase(it);
      shared_slot & sl = slot_of(v);
      bool had = count_known(sl) && sl.count.as<int64_t>() > 0;
      count_add(sl, -1);
      if(had && sl.count.as<int64_t>() == 0) {
        value held = std::move(sl.v);
        sl.v = value();
        drop_value(held, where);
      }
      return;
    }
    if(g.rid) {
      auto it = res_live.find(g.rid);
      if(it == res_live.end())
        fail(cat("ресурс ", types.name(g.type), " уже освобождён: его значение скопировано мимо правила переноса (нужен copy(", types.name(g.type), ") или перенос)"), where);
      if(it->second == 2) return; // It is now being released (destroyed).
      it->second = 2;
      ++drops_called;
      node_id f = member_fn_of("destroy", g.type);
      std::vector<value> vals{v};
      flow r = invoke(f, vals, {}, globals, {where, cat("destroy(", types.name(g.type), ")"), 0, 0});
      res_live.erase(g.rid);
      if(r.kind == flow_kind::thr) uncaught(r);
    }
    for(auto const * it = g.fields.end(); it != g.fields.begin();) drop_value((--it)->second, where);
  }
  // end of the field: eigenvalues (not alias, not transferred) - in reverse order
  void run_drops(scope * s, span where) {
    if(!res_any()) return;
    for(auto it = s->storage.rbegin(); it != s->storage.rend(); ++it) {
      if(!it->hidden && !it->is<thunk_v>() && has_resource(*it)) drop_value(*it, where);
      // What is declared later dies earlier: its memory is at the level immediately (the region
      // declared earlier sees itself empty); the area captured by the circuit does not have any
      if(!s->captured && !it->is<thunk_v>() && !checking()) *it = value();
    }
  }
  // Whether the name is read after node at (further in blocks before the function); repeating the cycle
  // or block of the structure in which the name is not declared is also read (the next iteration).
  bool used_after(node_id at, symbol name) const {
    auto mentions = [&](auto && self, node_id c) -> bool {
      if(!c) return false;
      if(db.is<n_ident>(c) && db.as<n_ident>(c).name == name) return true;
      bool r = false;
      db.for_each_child(c, [&](node_id x) { r = r || self(self, x); });
      return r;
    };
    auto declares = [&](auto && self, node_id c) -> bool {
      if(!c) return false;
      if(db.is<n_var>(c) && db.as<n_var>(c).name == name) return true;
      if(db.is<n_param>(c) && db.as<n_param>(c).name == name) return true;
      if(db.is<n_op>(c) && str(db.as<n_op>(c).op) == "=" && db.is<n_ident>(db.as<n_op>(c).args[0]) && db.as<n_ident>(db.as<n_op>(c).args[0]).name == name && implicit_decls.contains(c.value)) return true;
      bool r = false;
      db.for_each_child(c, [&](node_id x) { r = r || self(self, x); });
      return r;
    };
    for(node_id cur = at, p = db[at].parent; p; cur = p, p = db[p].parent) {
      if(db.is<n_fn>(p)) return false;
      if(db.is<n_block>(p)) {
        auto const & items = db.as<n_block>(p).items;
        bool after = false;
        for(node_id x: items) {
          if(after && mentions(mentions, x)) return true;
          if(x == cur) after = true;
        }
      }
      if((db.is<n_loop>(p) || db.is<n_lambda>(p)) && !declares(declares, p)) return true;
    }
    return false;
  }
  // the value of the name goes to a new place (announcement, assignment, return, literal
  // field): resource - transfer or copy(T)
  bool needs_take(node_id src, value const & v) const { return res_any() && db.is<n_ident>(src) && has_resource(v); }
  // Transfer from the name: no further read; name - parameter - alias - and the caller, along the
  // chain of calls (argument - not name, but field or element: part of someone else's value - copy)
  bool movable(node_id src, symbol name, value const * cell, scope * env) const {
    if(used_after(src, name)) return false;
    for(scope * s = env; s; s = s->parent)
      for(auto const & b: s->borrows)
        if(b.cell == cell) return db.is<n_ident>(b.src) && movable(b.src, db.as<n_ident>(b.src).name, cell, b.env);
    return true;
  }
  flow take(node_id src, value v, scope * env, uint32_t frame) {
    if(!needs_take(src, v)) return ok(std::move(v));
    symbol name = db.as<n_ident>(src).name;
    value * cell = find(env, name);
    if(!cell || cell->is<thunk_v>()) return ok(std::move(v));
    if(movable(src, name, cell, env)) {
      hide_cell(cell, src); // Transfer: the name is no longer readable (and the caller has alias)
      return ok(std::move(v));
    }
    if(is_region(v) && v.as<agg_v>().rid) { // level-copy - still the holder
      ++vm_root->regions[std::size_t(v.as<agg_v>().fields.begin()->second.as<int64_t>())].holders;
      v.as<agg_v>().rid = ++res_counter;
      res_live[v.as<agg_v>().rid] = 1;
      return ok(std::move(v));
    }
    if(is_shared(v)) { // copy of the shared - inc, your number
      count_add(slot_of(v), 1);
      v.as<agg_v>().rid = ++res_counter;
      res_live[v.as<agg_v>().rid] = 1;
      return ok(std::move(v));
    }
    node_id c = v.is<agg_v>() ? member_fn_of("copy", v.as<agg_v>().type) : node_id{};
    if(!c)
      fail(cat("значение ", types.describe(v), " — ресурс: ", str(name), " читается дальше, а копировать его нечем — объявите copy(", types.describe(v), " x) или не используйте ", str(name), " после"), db[src].where);
    std::vector<value> vals{v};
    return invoke(c, vals, {}, globals, {db[src].where, cat("copy(", types.describe(v), ")"), 0, frame});
  }

  flow eval_node(node_id n, n_block const & b, scope * env, uint32_t frame) {
    scope * inner = new_scope(env);
    scope_hold hold{*this, inner};
    flow r = run_block(b, 0, inner, frame);
    if(!res_any()) return r; // without resources - as it was (the resource could appear in the block itself)
    {
      if(r.normal() && b.has_value && !b.items.empty() && db.is<n_ident>(b.items.back())) { // the name of the block - transfer
        flow t = take(b.items.back(), std::move(r.v), inner, frame);
        if(!t.normal()) return t;
        r.v = std::move(t.v);
      }
      run_drops(inner, db[n].where);
    }
    return r;
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
        TZ_TRY(init0, eval(o.args[1], inner, frame));
        implicit_decls.insert(s.value);
        TZ_TAKE(init, o.args[1], init0, inner, frame);
        if(init.is<split_v>()) return split_continuation(b, i, s, init, inner, frame);
        emit({report_event::stage, path, std::string(str(db.as<n_ident>(o.args[0]).name)), init.known() ? cat("кт ", show_v(init)) : cat("рт ", types.describe(init)), db[s].where, {}});
        inner->declare(db.as<n_ident>(o.args[0]).name, std::move(init));
        continue;
      }
      if(checking() && db.is<n_var>(s) && !db.as<n_var>(s).ref) {
        auto const & v = db.as<n_var>(s);
        mark_executed(s); // instruction
        TZ_TRY(init0, eval(v.init, inner, frame));
        TZ_TAKE(init, v.init, init0, inner, frame);
        if(init.is<split_v>()) return split_continuation(b, i, s, init, inner, frame);
        declare_var(s, v, std::move(init), inner);
        continue;
      }
      flow f = eval(s, inner, frame);
      if(!f.normal()) return f;
      last = std::move(f.v);
      // Temporary discarded - the resource is released immediately
      if(res_any() && (i + 1 < b.items.size() || !b.has_value) && (db.is<n_call>(s) || db.is<n_construct>(s)) && has_resource(last)) drop_value(last, db[s].where);
      // reference &e = xs[k]: her life is the remainder of the block; what changed for her whole
      // (replacement, growth) is a fact for machine code (jit_ref_keeps):
//  
      if(checking() && db.is<n_var>(s) && db.as<n_var>(s).ref && i + 1 < b.items.size()) {
        write_log log{book.clock};
        flow r;
        {
          book.logs.push_back(&log);
          log_hold wh{*this, &log};
          r = run_block(b, i + 1, inner, frame);
        }
        ref_whole[s.value].insert(log.whole.begin(), log.whole.end());
        return r;
      }
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

  // Name: lexically (blocks, function, module), then functions, types and built-in names
  // of the module, then the environment of the caller and its caller (O4, user decision:
  // implicitly,
// - Yarik.
  value * find(scope * env, symbol name) {
    if(value * c = env->find(name)) return c;
    if(functions.contains(name) || structs.contains(name) || fixed_name(name)) return nullptr;
    for(scope * s = env; s; s = s->parent) {
      if(!s->caller) continue;
      value * c = find(s->caller, name);
      if(c && s->fn && db.as<n_fn>(s->fn).outline)
        fail(cat(describe_fn(s->fn), " помечена outline, но берёт имя ", str(name), " из окружения вызывающего"), db[s->fn].where);
      if(c && s->fn) caller_users.insert(s->fn.value);
      return c;
    }
    return nullptr;
  }

  // Register type name or true/false: the character number response is remembered (name
  // search miss on each function call; no string comparison). The register only grows: the
  // "no" is forgotten with the new type.
  bool fixed_name(symbol name) {
    if(fixed_types != types.names.size()) {
      fixed_names.assign(fixed_names.size(), 0);
      fixed_types = types.names.size();
    }
    if(name.value >= fixed_names.size()) fixed_names.resize(name.value + 1, 0);
    uint8_t & k = fixed_names[name.value];
    if(!k) k = types.builtin(str(name)) || str(name) == "true" || str(name) == "false" ? 2 : 1;
    return k == 2;
  }
  std::vector<uint8_t> fixed_names; // 0 don't know, 1 no, 2 yes
  size_t fixed_types = 0;

  // The value in the place of type tn (T x = e, tag T {e}): imaginable - is converted, otherwise error.
  value placed(value init, symbol tn, bool strict, span where) {
    value got;
    fit r = init.is<split_v>() ? fit::no : type_fit(init, str(tn), strict, got);
    if(r == fit::no && !strict && !init.is<split_v>() && numeric_pair(init, str(tn)))
      fail_rt("unpack_failed", cat("значение ", show_v(init), " не представимо в ", str(tn)), where); // type fits, meaning not
    if(r == fit::no) check_type(init, tn, where);
    else {
      if(r == fit::maybe) rt_check("unpack_failed", where);
      init = std::move(got);
    }
    return init;
  }

  bool implicit_declaration(node_id s, scope * env) const {
    if(!db.is<n_op>(s) || db.strings.text(db.as<n_op>(s).op) != "=" || !db.is<n_ident>(db.as<n_op>(s).args[0])) return false;
    return !env->find(db.as<n_ident>(db.as<n_op>(s).args[0]).name);
  }
  // The assignment that declares the variable (x = e without a name) is for projection.
  bool declares(node_id n) const { return implicit_decls.contains(n.value); }

  void declare_var(node_id s, n_var const & v, value init, scope * env) {
    if(v.type && db.is<n_ident>(v.type)) {
      // T x = e - the value is represented in T (reduced), explicit T x = e - type exactly T
      // (§5.8, the author's decision on parameters - the same in the announcement)
      init = placed(std::move(init), db.as<n_ident>(v.type).name, v.strict, db[v.init].where);
    }
    if(checking())
      emit({report_event::stage, path, std::string(str(v.name)),
          init.known() ? cat("кт ", show_v(init)) : cat("рт ", types.describe(init)), db[s].where, {}});
    env->declare(v.name, std::move(init));
  }

  flow eval_node(node_id n, n_var const & v, scope * env, uint32_t frame) {
    if(v.ref) return declare_ref(n, v, env, frame);
    TZ_TRY(init0, eval(v.init, env, frame));
    TZ_TAKE(init, v.init, init0, env, frame);
    declare_var(n, v, std::move(init), env);
    return ok();
  }

  // The entity body area is a function for calling its functions from the outside (entity .f(…)):
  // its local functions are declared, as the block itself would declare them, the neighbors see
  // each other. Encapsulation - entity, without a separate mechanism:
// 
//.
  scope * entity_scope(symbol owner) {
    auto [it, fresh] = entity_scopes.try_emplace(owner.value, nullptr);
    if(fresh) {
      std::vector<node_id> fs;
      if(auto f = functions.find(owner); f != functions.end()) fs = f->second;
      it->second = body_scope(fs, globals);
    }
    return it->second;
  }
  std::unordered_map<uint32_t, scope *> entity_scopes;
  // The domain of bodies of functions fs: their local functions - as the block itself would declare them
  scope * body_scope(std::vector<node_id> const & fs, scope * parent) {
    scope * s = new_scope(parent);
    for(node_id f: fs)
      if(db.is<n_block>(db.as<n_fn>(f).body))
        for(node_id item: db.as<n_block>(db.as<n_fn>(f).body).items)
          if(db.is<n_fn>(item) && !db.as<n_fn>(item).scoped) {
            symbol name = db.as<n_fn>(item).name;
            if(value * cell = s->find(name); cell && cell->is<fn_v>() && cell->as<fn_v>().env == s) cell->as<fn_v>().set.push_back(item);
            else s->declare(name, value(fn_v{{}, item, s, {item}}));
          }
    return s;
  }
  // Functions whose bodies contain entity-value members: entity by name (amd64) - all its
  // ads; nested function-context (cg.unless) - it itself.
  std::vector<node_id> entity_bodies(fn_v const & f) {
    if(f.name && f.set.empty() && !f.lambda) {
      if(auto it = functions.find(f.name); it != functions.end()) return it->second;
      return {};
    }
    if(!f.name && f.set.empty() && f.lambda && db.is<n_fn>(f.lambda) && db.is<n_block>(db.as<n_fn>(f.lambda).body) && f.env) return {f.lambda};
    return {};
  }
  // entity .context is a nested function-context value, with its area (its neighbors are
  // visible to each other):
//  
  std::optional<value> entity_context(fn_v const & f, symbol name) {
    std::vector<node_id> bodies = entity_bodies(f);
    if(bodies.empty()) return std::nullopt;
    scope * home = f.name ? entity_scope(f.name) : f.env;
    for(node_id b: bodies)
      for(node_id item: db.as<n_block>(db.as<n_fn>(b).body).items)
        if(db.is<n_fn>(item) && !db.as<n_fn>(item).scoped && db.as<n_fn>(item).name == name && db.is<n_block>(db.as<n_fn>(item).body)) {
          auto [it, fresh] = context_scopes.try_emplace(item.value, nullptr);
          if(fresh) it->second = body_scope({item}, home);
          return value(fn_v{{}, item, it->second, {}});
        }
    return std::nullopt;
  }
  std::unordered_map<uint32_t, scope *> context_scopes;

  flow eval_node(node_id n, n_fn const & f, scope * env, uint32_t) {
    if(f.scoped) { // using operator ... - to the end of the frame of the function-context, for all executable
      overrides.push_back({f.name, n, frames.empty() ? symbol{} : frames.back().name});
      return ok();
    }
    capture(env);
    // local function: the same names in this block - one set of overloads
    for(auto & [name, cell]: env->vars)
      if(name == f.name && cell->is<fn_v>() && !cell->as<fn_v>().set.empty() && cell->as<fn_v>().env == env) {
        cell->as<fn_v>().set.push_back(n);
        return ok();
      }
    env->declare(f.name, value(fn_v{{}, n, env, {n}}));
    rules_for_blocks(n, env);
    return ok();
  }

  // The rule in the body of the structure (operator, syntax, construction - §4.4) - for its
  // blocks: parsing took the rules of the body, the execution of the block sees the rules
  // announced at this point (the layer above the region of the caller - ruled_env). Lexically,
  // like parsing, it's not like using operator (§8.2). "" run_in_govno { мы можем здесь
  // поллучить всё для чего мы его вызываем.}.
  void rules_for_blocks(node_id n, scope * env) {
    node_id b = db[n].parent;
    if(!env->parent || !rule_decl(n) || !db.is<n_block>(b) || !db[b].parent || !db.is<n_fn>(db[b].parent) || db.as<n_fn>(db[b].parent).body != b) return;
    for(auto & [name, cell]: env->parent->vars) // design
      if(cell->is<thunk_v>() && block_arg(cell->as<thunk_v>().expr)) cell->as<thunk_v>().rules = env;
  }
  // The block with rules has a layer above the area of the caller with the rules of the
  // body (only they: the rest of the body is not visible to the block).
  scope * ruled_env(thunk_v const & t) { return t.rules ? rules_layer(t) : t.env; }
  [[gnu::cold, gnu::noinline]] scope * rules_layer(thunk_v const & t) { // The path of hot cycles does not affect him.
    scope * layer = new_scope(t.env);
    for(auto & [name, cell]: t.rules->vars)
      if(cell->is<fn_v>() && rule_decl(cell->as<fn_v>().lambda)) layer->alias(name, cell);
    return layer;
  }
  struct ruled_hold {
    interpreter & vm;
    thunk_v const & t;
    scope * s;
    ~ruled_hold() {
      if(s != t.env) vm.drop_scope(s);
    }
  };

  flow eval_node(node_id n, n_return const & r, scope * env, uint32_t frame) {
    value v;
    if(r.value) {
      TZ_TRY(x0, eval(r.value, env, frame));
      TZ_TAKE(x, r.value, x0, env, frame); // return x - transfer
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

  // require(c, "сообщение", "пометка", место, …): the condition is known when compiling and false
  // (at least in one way) is the parsing error with a trace: place in the program, markings at
  // places (code values), path, calls; otherwise, check in runtime.
// 
// Message and notes are calculated only in case of violation.
  flow eval_require(node_id n, node_id cond, scope * env, uint32_t frame) {
    TZ_TRY(c, eval(cond, env, frame));
    bool in_path = checking() && c.is<split_v>() && std::ranges::any_of(c.as<split_v>().alts, [](value const & a) { return a.is<bool>() && !a.as<bool>(); });
    auto b = in_path ? std::optional<bool>(false) : known_bool(c, db[cond].where);
    if(b && !*b) {
      auto const & args = db.as<n_call>(n).args;
      std::string msg = db.is_assume(n) ? "нарушено предположение assume" : "нарушено требование require";
      if(args.size() > 1) {
        TZ_TRY(m, eval(args[1], env, frame));
        msg = m.is<str_v>() ? std::string(m.as<str_v>().str()) : show_v(m);
      }
      for(auto const & d: level_details) msg += cat("; ", d); // static_level - with a list of unproven
      if(!checking() && !hook_depth) fail_rt("require_failed", msg, db[n].where); // Execution hook - Extension check (§9.1): parsing error
      span where = program_where(n);
      diagnostic d(msg, where, diag_kind::runtime);
      d.type = "require_failed";
      d.fixed = true; // known when compiling - error parsing, not the exception of the program
      if(in_path) d.note("в одном из путей", where);
      for(std::size_t k = 2; k + 1 < args.size(); k += 2) {
        TZ_TRY(text, eval(args[k], env, frame));
        TZ_TRY(at, eval(args[k + 1], env, frame));
        std::string t = text.is<str_v>() ? std::string(text.as<str_v>().str()) : show_v(text);
        for(auto const & x: at.is<split_v>() ? at.as<split_v>().alts : std::vector<value>{at})
          if(x.is<agg_v>() && types.name(x.as<agg_v>().type) == "code" && !x.as<agg_v>().fields.empty() && x.as<agg_v>().fields[0].second.is<int64_t>())
            if(node_id p(uint32_t(x.as<agg_v>().fields[0].second.as<int64_t>())); p) d.note(t, db[p].where);
      }
      if(!path.empty()) d.note(cat("в продолжении ", path), where);
      for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
      throw d;
    }
    // lead demand - overload selection condition: its failure runtime tells at the call
    // (unpack_failed), the body is not executed
    if(checking() && !b && !leading_require(n)) rt_check("require_failed", db[n].where);
    // assume( is_index(k, xs) ) - then k - proved index xs (§10.2 ):
//  
// Check always, the fact - parsing and machine code
    if(db.is_assume(n))
      if(auto proof = index_check(cond, env); proof && proof->positive) proof->cell->proof = proof->fact;
    return ok(); // run-time - Condition: runtime (§5.4)
  }

  bool leading_require(node_id n) const {
    for(uint32_t i = 0; i < db.size(); ++i) {
      if(!db.is<n_fn>(node_id(i)) || !db.is<n_block>(db.as<n_fn>(node_id(i)).body)) continue;
      for(node_id s: db.as<n_block>(db.as<n_fn>(node_id(i)).body).items) {
        if(!db.require_cond(s) || db.is_assume(s)) break; // assume is not a condition of overload
        if(s == n) return true;
      }
    }
    return false;
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

  // Match levels of type:
// 
  // explicit - only an equal type. Imagine: i64 → f64 lossless, f64 → i64 integer; out is a
  // value in parameter type. Maybe - run-time - meaning, will decide the execution.
  //
  enum class fit { no, exact, representable, maybe };
  fit type_fit(value const & v, std::string_view want, bool strict, value & out) {
    out = v;
    if(want != "shared" && want != "auto" && is_shared(v)) { // Shared - type of stored, value - itself shared (transparent)
      value got;
      fit r = type_fit(slot_of(v).v, want, strict, got);
      return r == fit::no ? fit::no : fit::exact;
    }
    std::string got_desc = types.describe(v);
    std::string_view got(types.name(types.of(v)));
    if(want == "auto" || got_desc == want || got == want) return fit::exact; // sameness
    // Type ratio in language (fit_rule prelude, M6 step 6), facts in VM
    value const * x = shape_of(v);
    bool want_node = node_kind_named(want) || want == lang_context();
    bool is_code = want_node && x && code_agg(*x);
    bool subtype = types.is_a(got, want);
    bool record = is_record(v), empty = record && empty_record(v), want_struct = record && structs.contains(sym(want));
    node_id conv = implicit_fn(got, want);
    std::string key;
    key += char('0' + strict + 2 * subtype + 4 * record + 8 * want_struct + 16 * empty + 32 * is_code + 64 * want_node);
    key += conv ? '+' : '-';
    key += got;
    key += '|';
    key += want;
    auto it = fit_rules.find(key);
    if(it == fit_rules.end()) {
      value r = lang_rule(fit_fn, "fit_rule", {value(std::string(got)), value(std::string(want)), value(strict), value(subtype), value(record), value(want_struct), value(empty), value(is_code), value(want_node), value(bool(conv))}, {});
      it = fit_rules.emplace(std::move(key), r.as<int64_t>()).first;
    }
    switch(it->second) {
      case 2: return fit::representable; // base
      case 3: return record_fit(v, want, out); // {x: …} where struct is expected
      case 4: return scalar_default(want, out) ? fit::representable : fit::no; // i64 n = {} — 0
      case 5: { // i64 - f64 without loss
        if(!v.known()) {
          out = erase(value(1.0));
          return fit::representable;
        }
        int64_t i = x->as<int64_t>();
        double d = double(i);
        if(d < -9.2e18 || d > 9.2e18 || int64_t(d) != i) return fit::no;
        out = value(d);
        return fit::representable;
      }
      case 6: { // f64 → i64 - the whole
        if(!v.known()) {
          out = erase(value(int64_t(1)));
          return fit::maybe;
        }
        double d = x->as<double>();
        if(!(d >= -9.2e18 && d <= 9.2e18) || std::trunc(d) != d) return fit::no;
        out = value(int64_t(d));
        return fit::representable;
      }
      case 7: { // N if c receives the node code n if, n context c and the heirs
        if(!v.known()) return strict ? fit::no : fit::maybe;
        node_id c = code_node(v, {});
        std::string k(lang_kind_of(db[c]));
        return k == want ? fit::exact : !strict && types.is_a(k, want) ? fit::representable : fit::no;
      }
      case 8: { // Transformation function: f64 implicit(meters m) =>
        if(!conv) return fit::no;
        std::vector<value> vals{v};
        flow r = invoke(conv, vals, {}, globals, {db[conv].where, "implicit", 0, 0});
        if(r.kind == flow_kind::thr) uncaught(r);
        if(!r.normal()) return fit::no;
        out = r.v;
        return v.known() ? fit::representable : fit::maybe;
      }
      default: return fit::no;
    }
  }
  // Implicit transformation declared by the program: `want implicit(got x) => …` -
  // parameter exactly type got, type of result - want.
//.
  // Whether his relation permits is decided by fit_rule (the fact of implicit).
  node_id implicit_fn(std::string_view got, std::string_view want) {
    auto it = functions.find(sym("implicit"));
    if(it == functions.end()) return {};
    for(node_id f: it->second) {
      auto const & fn = db.as<n_fn>(f);
      if(fn.params.size() != 1 || !fn.result_type || !db.is<n_ident>(fn.result_type) || str(db.as<n_ident>(fn.result_type).name) != want) continue;
      node_id t = db.as<n_param>(fn.params[0]).type;
      if(t && db.is<n_ident>(t) && str(db.as<n_ident>(t).name) == got) return f;
    }
    return {};
  }
  std::unordered_map<std::string, int64_t> fit_rules; // facts → relation (fit_rule)
  // The threshold of static parsing is analysis_limit prelude (M6 step 7), once to the name.
  enum limit_kind { loop_known, loop_types, recursion_widen, recursion_depth };
  int64_t limits[4] = {}; // 0 - not yet asked
  node_id limit_fn;
  int64_t analysis_limit(limit_kind k) {
    if(!limits[k]) {
      static constexpr std::string_view names[] = {"loop_known", "loop_types", "recursion_widen", "recursion_depth"};
      value r = lang_rule(limit_fn, "analysis_limit", {value(std::string(names[k]))}, {});
      if(!r.is<int64_t>() || r.as<int64_t>() <= 0) fail(cat("analysis_limit(", names[k], "): ожидалось положительное число"), {});
      limits[k] = r.as<int64_t>();
    }
    return limits[k];
  }
  node_id fit_fn;
  // A rule in language (prelude function) in an auxiliary interpreter: a pure function of
  // facts, the result causing remembers. Rules don’t apply themselves: there are no chains,
  // overloads, and type transformations in their code - otherwise, an explanation error.
  value lang_rule(node_id & fn, std::string_view name, std::vector<value> args, span where) {
    // bootstrap: rule code also needs rules (for array rejects overload for flow - array → stream
    // ratio); they are considered by the assistant of the next level, fit_rule and join_rule have
    // no cycles and overloads inside - depth is finite
    if(helper_depth >= 3) fail(cat("правило ", name, " понадобилось в коде правил на глубине ", helper_depth, ": в overload_pick, chain_resolve, fit_rule, join_rule — без цепочек и лишних перегрузок"), where);
    if(!fn)
      for(uint32_t i = db.size(); i-- > 0;)
        if(db.is<n_fn>(node_id(i)) && db.strings.text(db.as<n_fn>(node_id(i)).name) == name) {
          fn = node_id(i);
          break;
        }
    if(!fn) fail(cat("нет ", name, " в прелюдии"), where);
    return in_helper(fn, std::move(args));
  }
  // Numbers between which there is "imaginable": i64 . f64.
  bool numeric_pair(value const & v, std::string_view want) const {
    value const * x = shape_of(v);
    if(!x) return false;
    return (want == types.float_name() && x->is<int64_t>()) || (want == types.int_name() && x->is<double>());
  }

  // Anonymous aggregate {x: 1, y: 2} (§4.3): type - by structure, type name - recording fields.
  // 
// 
// One structure, one type.
  value record_value(field_list fs) {
    std::string name = "{";
    for(uint32_t k = 0; k < fs.size(); ++k) name += cat(k ? ", " : "", str(fs[k].first), ": ", types.describe(fs[k].second));
    return value(agg_v{types.named(name + "}"), std::move(fs)});
  }
  bool is_record(value const & v) const {
    value const * x = shape_of(v);
    return x && x->is<agg_v>() && types.anonymous(x->as<agg_v>().type);
  }
  // {} is a literal without fields: in a place of type T - the same as T {} (struct - default
  // fields). Numbers have zero, like zero(T) :
  //
// *Mine:* Only i64 and f64 (bool, str, y)
  // There are no {} arrays, an error like zero(T).
  bool empty_record(value const & v) const { return is_record(v) && shape_of(v)->as<agg_v>().fields.empty(); }
  bool scalar_default(std::string_view want, value & out) const {
    if(want == types.int_name()) out = value(int64_t(0));
    else if(want == types.float_name()) out = value(double(0));
    else return false;
    return true;
  }
  // Anonymous fields - from his record: name and type (array type - array, tuple).
  std::vector<std::pair<std::string, type_id>> record_fields(type_id t) {
    std::vector<std::pair<std::string, type_id>> out;
    std::string whole(types.name(t)); // copy: named() below grows the registry
    for(auto [name, tn]: type_registry::record_parts(whole)) {
      if(!types.anonymous(tn)) tn = tn.substr(0, tn.find('<')); // array<f64> — array
      out.push_back({std::string(name), types.builtin(tn) ? types.builtin(tn) : types.named(tn)});
    }
    return out;
  }
  // Anonymous literature is where a struct want is expected (parameter, announcement, field,
  // place, result): fields - by name, values - are represented in field types, missing - default
  // values struct, extra field - no.
// 
//.
  // My:* is the level of "imaginable", not "type equal" (`explicit point p` literal does not take).
  fit record_fit(value const & v, std::string_view want, value & out, std::string * why = nullptr) {
    auto no = [&](std::string w) {
      if(why) *why = std::move(w);
      return fit::no;
    };
    value const * x = shape_of(v);
    auto all = fields_of_struct(sym(want));
    auto const & have = x->as<agg_v>().fields;
    for(auto const & [k, _]: have)
      if(std::ranges::none_of(all, [&](node_id f) { return db.as<n_var>(f).name == k; })) return no(cat("у ", want, " нет поля ", str(k)));
    agg_v g{types.named(want), {}};
    fit r = fit::representable;
    for(node_id f: all) {
      auto const & d = db.as<n_var>(f);
      value const * got = nullptr;
      for(auto const & [k, fv]: have)
        if(k == d.name) got = &fv;
      value fv;
      if(got && d.type && db.is<n_ident>(d.type)) {
        fit q = type_fit(*got, str(db.as<n_ident>(d.type).name), false, fv);
        if(q == fit::no) return no(cat("поле ", str(d.name), ": ожидается ", str(db.as<n_ident>(d.type).name), ", получено ", types.describe(*got)));
        if(q == fit::maybe) r = fit::maybe;
      } else if(got) fv = *got;
      else if(d.init) {
        flow dv = eval(d.init, globals, 0);
        if(!dv.normal()) return no(cat("значение по умолчанию поля ", str(d.name), " не вычислилось"));
        fv = field_typed(d, std::move(dv.v), db[d.init].where);
      } else return no(cat("не задано поле ", str(d.name)));
      g.fields.push_back({d.name, std::move(fv)});
      if(d.ct) pin(g.fields.back().second, d.name, db[f].where);
    }
    out = v.known() ? value(std::move(g)) : erase(value(std::move(g)));
    return r;
  }
  // Value for a place whose type is known (field struct, variable with struct, result with
  // type): the anonymous literal becomes that struct; otherwise, as is.
  value to_type(value v, std::string_view want, span where) {
    if(value z; empty_record(v) && scalar_default(want, z)) return z; // x = {} у i64 — 0
    if(!is_record(v) || !structs.contains(sym(want))) return v;
    value got;
    std::string why;
    if(record_fit(v, want, got, &why) == fit::no) fail(cat(types.describe(v), " не становится ", want, ": ", why), where);
    return got;
  }
  // The type of place of the anonymous letter {…} on AST (§4.3): the result of the function (=> e,
  // return e, the value of the block-body) or T x = {…}; empty - there is no space in the text
  // (argument, etc.). It is used by JIT (VM cites the literature that was calculated for machine
  // code) and the compiler in the language (jit_literal_place), one rule on both sides.
  std::string literal_place(node_id e) const {
    node_id up = e ? db[e].parent : node_id{};
    auto named = [&](node_id t) { return t && db.is<n_ident>(t) ? std::string(db.strings.text(db.as<n_ident>(t).name)) : std::string(); };
    if(!up) return {};
    if(db.is<n_fn>(up) && db.as<n_fn>(up).body == e) return named(db.as<n_fn>(up).result_type);
    if(db.is<n_var>(up) && db.as<n_var>(up).init == e) return named(db.as<n_var>(up).type);
    if(db.is<n_return>(up) && db.as<n_return>(up).value == e && !db.as<n_return>(up).target) {
      node_id f = up;
      while(f && !db.is<n_fn>(f)) f = db[f].parent;
      return f ? named(db.as<n_fn>(f).result_type) : std::string();
    }
    if(db.is<n_block>(up) && db.as<n_block>(up).has_value && !db.as<n_block>(up).items.empty() && db.as<n_block>(up).items.back() == e)
      if(node_id f = db[up].parent; f && db.is<n_fn>(f) && db.as<n_fn>(f).body == up) return named(db.as<n_fn>(f).result_type);
    return {};
  }
  // struct unit (not tagged above: struct i64 { __int64 value; } - scalar)
  bool aggregate_struct(std::string_view name) {
    if(name.empty()) return false;
    auto it = structs.find(sym(name));
    if(it == structs.end()) return false;
    auto const & st = db.as<n_extend>(it->second);
    return !(st.items.size() == 1 && db.is<n_var>(st.items[0]) && db.is<n_ident>(db.as<n_var>(st.items[0]).type) && str(db.as<n_var>(st.items[0]).name) == "value" &&
        type_registry::machine(str(db.as<n_ident>(db.as<n_var>(st.items[0]).type).name)));
  }
  // The place holds the value of the type: struct is its name, otherwise it is empty (*my:* type of place is the type of its meaning).
  std::string place_type(value const & cur) const {
    value const * x = shape_of(cur);
    if(x && x->is<int64_t>()) return std::string(types.int_name());
    if(x && x->is<double>()) return std::string(types.float_name());
    if(!x || !x->is<agg_v>() || types.anonymous(x->as<agg_v>().type)) return {};
    return std::string(types.name(x->as<agg_v>().type));
  }
  // the value of the field d (from the literal or by default): the field with the type {…} becomes it
  value field_typed(n_var const & d, value v, span where) {
    return d.type && db.is<n_ident>(d.type) ? to_type(std::move(v), str(db.as<n_ident>(d.type).name), where) : v;
  }

  // Whether samples-arrays of candidates cover any length (elements - names, , residue:
  // without checks), in the same parameter - then there is no failure.
  bool covers_all_lengths(auto const & cands) const {
    int at = -1;
    std::set<std::size_t> exact;
    std::size_t from = ~std::size_t{0}; // length ≥ from
    for(auto const & c: cands) {
      auto const & fn = db.as<n_fn>(c.fn);
      if(db.is<n_block>(fn.body) && !db.as<n_block>(fn.body).items.empty() && db.require_cond(db.as<n_block>(fn.body).items[0]) && !db.is_assume(db.as<n_block>(fn.body).items[0])) return false;
      int mine = -1;
      for(std::size_t i = 0; i < fn.params.size(); ++i)
        if(node_id t = db.as<n_param>(fn.params[i]).type; t && db.is<n_pat>(t)) {
          if(mine >= 0) return false;
          mine = int(i);
        }
      if(mine < 0 || (at >= 0 && mine != at)) return false;
      at = mine;
      auto const & p = db.as<n_pat>(db.as<n_param>(fn.params[std::size_t(mine)]).type);
      if(!p.type || str(db.as<n_ident>(p.type).name) != "array") return false; // {x: a} is not an array
      bool rest = false;
      for(node_id x: p.items) {
        if(!db.is<n_param>(x) || db.as<n_param>(x).type) return false; // check-in
        rest = rest || db.as<n_param>(x).pack;
      }
      std::size_t n = p.items.size() - (rest ? 1 : 0);
      if(rest) from = std::min(from, n);
      else exact.insert(n);
    }
    for(std::size_t k = 0; k < from; ++k)
      if(!exact.contains(k)) return false;
    return from != ~std::size_t{0};
  }

  // Specificity of the sample among overloads:
// Type or value verification
  // element - +2; fixed element - +1; exact length (without residue) - +1: array {a, b}
  // specifically array {x, ...xs}.
  uint32_t pattern_checks(node_id pat) const {
    uint32_t n = 0;
    bool rest = false;
    for(node_id x: db.as<n_pat>(pat).items) {
      if(db.is<n_param>(x) && db.as<n_param>(x).pack) {
        rest = true;
        continue;
      }
      ++n;
      if(db.is<n_pat_field>(x)) x = db.as<n_pat_field>(x).sub; // circulation
      if(db.is<n_pat>(x)) n += 1 + pattern_checks(x);
      else if(!db.is<n_param>(x) || db.as<n_param>(x).type) n += 2;
    }
    return n + (rest ? 0 : 1);
  }

  // Value in sample: names in into; values-elements are calculated in env.
  unpacked unpack_into(node_id pat, value const & v, scope * into, scope * env, uint32_t frame, span where) {
    auto const & p = db.as<n_pat>(pat);
    std::string_view kind = p.type ? str(db.as<n_ident>(p.type).name) : std::string_view();
    value const * x = shape_of(v);
    if(unknown(v) || !x) return unpacked::maybe; // Type unknown even at parsing
    auto elem = [&](node_id item, value const & e) -> unpacked {
      if(db.is<n_pat>(item)) return unpack_into(item, e, into, env, frame, where);
      if(db.is<n_param>(item)) {
        auto const & b = db.as<n_param>(item);
        value got = e;
        fit r = fit::exact;
        if(b.type) {
          r = type_fit(e, str(db.as<n_ident>(b.type).name), b.strict, got);
          if(r == fit::no) return unpacked::no;
        }
        if(str(b.name) != "_") into->declare(b.name, got);
        if(checking()) observe(item, ok(got), false); // Name type for machine code (jit_type)
        return r == fit::maybe ? unpacked::maybe : unpacked::yes;
      }
      flow f = eval(item, env, frame); // Equality of the element
      value eq = binary("==", e, f.v, db[item].where);
      if(!eq.is<bool>()) return unpacked::maybe;
      return eq.as<bool>() ? unpacked::yes : unpacked::no;
    };
    // Position elements and interface references (.field: subsample)
    node_list pos, fields;
    for(node_id it: p.items) (db.is<n_pat_field>(it) ? fields : pos).push_back(it);
    auto interface = [&](unpacked r) {
      for(node_id it: fields) {
        if(r == unpacked::no) return r;
        auto const & f = db.as<n_pat_field>(it);
        // The application failed (border, exception) - was not unpacked:
//  
        subjects.push_back(v);
        flow got;
        try {
          got = eval(f.access, env, frame);
        } catch(diagnostic const & d) {
          subjects.pop_back();
          if(d.kind != diag_kind::runtime || d.fixed) throw;
          return unpacked::no;
        } catch(...) {
          subjects.pop_back();
          throw;
        }
        subjects.pop_back();
        if(got.kind == flow_kind::thr) return unpacked::no;
        r = both(r, elem(f.sub, got.v));
      }
      return r;
    };
    // {x: a, y: b} = v - addresses .x to any aggregate (anonymous or struct): structure, not type
    // name
// 
    if(!p.type) {
      if(!x->is<agg_v>()) return unpacked::no;
      for(node_id it: fields) { // no such field - sample does not fit (choice overload f({x: a}))
        auto const & a = db.as<n_op>(db.as<n_pat_field>(it).access);
        symbol name = sym(std::string(str(a.op).substr(1)));
        if(std::ranges::none_of(x->as<agg_v>().fields, [&](auto const & f) { return f.first == name; })) return unpacked::no;
      }
      return interface(unpacked::yes);
    }
    if(kind == "array") {
      if(!x->is<array_v>()) return unpacked::no;
      auto const & a = x->as<array_v>();
      std::size_t rest = pos.size();
      for(std::size_t k = 0; k < pos.size(); ++k)
        if(db.is<n_param>(pos[k]) && db.as<n_param>(pos[k]).pack) {
          if(rest != pos.size()) fail("в образце — один ...остаток", where);
          rest = k;
        }
      bool open = rest < pos.size() || (pos.empty() && !fields.empty()); // Interface only - any length
      std::size_t fixed = pos.size() - (rest < pos.size() ? 1 : 0);
      // length known: value (not erased array) or erased tuple - its length and element types - part
      // of the type
      bool tuple_shape = v.is<rt_v>() && is_tuple(a);
      bool sized = v.known() || !v.is<rt_v>() || tuple_shape;
      if(sized && (open ? a.size() < fixed : a.size() != fixed)) return unpacked::no;
      unpacked r = sized ? unpacked::yes : unpacked::maybe;
      value any = sized ? value() : element_type(v, where);
      std::size_t tail = rest < pos.size() ? pos.size() - rest - 1 : 0;
      for(std::size_t k = 0; k < pos.size(); ++k) {
        if(k == rest) { // ...the remainder is an array of remaining
          auto const & b = db.as<n_param>(pos[k]);
          value part = v;
          if(sized) {
            std::vector<value> xs;
            for(std::size_t j = k; j + tail < a.size(); ++j) xs.push_back(a.get(uint32_t(j)));
            part = value(array_v::of(std::move(xs)));
            if(tuple_shape) part = erase(part);
          }
          if(str(b.name) != "_") into->declare(b.name, part);
          if(checking()) observe(pos[k], ok(part), false);
          continue;
        }
        std::size_t at = k < rest ? k : a.size() - (pos.size() - k);
        r = both(r, elem(pos[k], sized ? a.get(uint32_t(at)) : any));
        if(r == unpacked::no) return r;
      }
      return interface(r);
    }
    // AST node - struct of the program: a sample of n_op {…} above the code - checking the type node
    // and its field; in parsing, the node run-time - fields on the samples of the structure node base
    // node (n_context - block, module, quote) - a sample of the base: the type node - its successor,
    // the field - the base (first; the author's decision 2026-10-02)
    bool node_base = kind == lang_context();
    if((node_kind_named(kind) || node_base) && x->is<agg_v>() && types.name(x->as<agg_v>().type) == "code") {
      std::vector<value> fs;
      unpacked r = unpacked::yes;
      if(v.known()) {
        node_id c = code_node(v, where);
        if(!c || (lang_kind_of(db[c]) != kind && !types.is_a(std::string(lang_kind_of(db[c])), kind))) return unpacked::no;
        fs = node_fields(c);
        fs.resize(std::min(fs.size(), node_samples(kind).size())); // sample fields - base fields
      } else {
        fs = node_samples(kind);
        r = unpacked::maybe;
      }
      if(!pos.empty() && pos.size() != fs.size()) fail(cat("образец ", kind, "{…}: полей ", fs.size(), ", элементов ", pos.size()), where);
      for(std::size_t k = 0; k < pos.size() && r != unpacked::no; ++k) {
        if(db.is<n_param>(pos[k]) && db.as<n_param>(pos[k]).pack) fail("...остаток — только в array{…}", db[pos[k]].where);
        r = both(r, elem(pos[k], fs[k]));
      }
      return interface(r);
    }
    if(structs.contains(db.as<n_ident>(p.type).name)) {
      if(!x->is<agg_v>() || !types.is_a(types.name(x->as<agg_v>().type), kind)) return unpacked::no; // heir
      auto const & fs = x->as<agg_v>().fields;
      std::size_t own = fields_of_struct(db.as<n_ident>(p.type).name).size(); // sample fields - base fields (first)
      if(!pos.empty() && own != pos.size()) fail(cat("образец ", kind, "{…}: полей ", own, ", элементов ", pos.size()), where);
      unpacked r = unpacked::yes;
      for(std::size_t k = 0; k < pos.size() && r != unpacked::no; ++k) {
        if(db.is<n_param>(pos[k]) && db.as<n_param>(pos[k]).pack) fail("...остаток — только в array{…}", db[pos[k]].where);
        r = both(r, elem(pos[k], fs[k].second));
      }
      return interface(r);
    }
    // It's not an aggregation.
    if(!pos.empty()) fail(cat("образец ", kind, "{…}: не struct и не array — только обращения .f: подобразец"), db[p.type].where);
    if(types.name(types.of(v)) != kind && types.describe(v) != kind) return unpacked::no;
    return interface(unpacked::yes);
  }

  // ...xs last parameter - the tail of arguments (pack clause expr...) - Not him?
  bool rest_param(node_id f) const {
    auto const & ps = db.as<n_fn>(f).params;
    return !ps.empty() && db.as<n_param>(ps.back()).pack && db.as<n_param>(ps.back()).mode == param_mode::let;
  }

  bool has_unpack(node_id n) const {
    if(db.is<n_unpack>(n)) return true;
    if(db.is<n_lambda>(n)) return false;
    bool found = false;
    db.for_each_child(n, [&](node_id c) { found = found || has_unpack(c); });
    return found;
  }

  // Tag "text {x} text" ст tag ("text", x, "text") Body native: function parameters are VM
  // values; effects are “unproven” (§3.3).
  flow eval_node(node_id n, n_native const &, scope * env, uint32_t) {
    native_entry const * e = native_of(db, n);
    std::vector<value> a;
    for(node_id pn: db.as<n_fn>(native_owner(db, n)).params) {
      value * c = env->find(db.as<n_param>(pn).name);
      a.push_back(c ? *c : value());
    }
    value r = e->fn(*this, a, db[n].where);
    if(checking() && !r.known()) // “Not proven” at the level of staticity, not the type of exception
      for(auto const & k: instance_keys) emit({report_event::rt_check, k, "native", {}, db[n].where, {}});
    return ok(std::move(r));
  }

  flow eval_node(node_id n, n_spread const &, scope *, uint32_t) { fail("...раскрытие — только аргументом вызова", db[n].where); }

  flow eval_node(node_id n, n_subject const &, scope *, uint32_t) {
    if(subjects.empty()) fail("обращение без значения — только в образце", db[n].where);
    return ok(subjects.back());
  }
  flow eval_node(node_id n, n_pat_field const &, scope *, uint32_t) { fail("элемент образца — только в распаковке", db[n].where); }

  // Instructions: names - in the current area; not unpacked - exception. Predicate (test):
  // success - bool (in parsing unknown - run-time), names - with success.
  flow eval_node(node_id n, n_unpack const & u, scope * env, uint32_t frame) {
    TZ_TRY(v, eval(u.value, env, frame));
    unpacked r = unpack_into(u.pattern, v, env, env, frame, db[n].where);
    if(u.test) return ok(r == unpacked::maybe ? erase(value(true)) : value(r == unpacked::yes));
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
    if(has_unpack(i.cond)) { // Unpacking names in condition - only then
      then_env = new_scope(env);
      then_hold.emplace(*this, then_env);
    }
    TZ_TRY(c, eval(i.cond, then_env, frame));
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
    value * k = find(env, db.as<n_ident>(c.args[0]).name);
    value * a = find(env, db.as<n_ident>(c.args[1]).name);
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
        safe_point(true); // rib-back
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
      loop_whole[n.value].insert(log.whole.begin(), log.whole.end());
      if(!loops.back().may_break) {
        if(f.kind == flow_kind::brk) return ok();
        if(f.kind == flow_kind::ret || f.kind == flow_kind::thr) return f;
        if(guard > analysis_limit(loop_known)) fail("цикл не сошёлся при статическом разборе", db[n].where);
        continue;
      }
      if(f.kind == flow_kind::ret) record_return(f);
      if(f.kind == flow_kind::thr) record_throw(f.v);
      return abstract_loop(l.body, env, frame, std::move(log.cells), n);
    }
  }

  flow abstract_loop(node_id body, scope * env, uint32_t frame, std::set<value *> cells, node_id n) {
    for(int64_t pass = 0, passes = analysis_limit(loop_types); pass < passes; ++pass) {
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
      loop_whole[n.value].insert(log.whole.begin(), log.whole.end());
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
      // anew - only unfinished "does not change" (completed - the result of past requests: the machine
      // asks on each call, without this - bypassing the body every time)
      std::erase_if(writes, [&](auto const & e) { return !e.second && !writes_final.contains(e.first); });
      compute();
      if(!writes_guess_used || trues() == before) {
        for(auto const & [k, w]: writes) writes_final.insert(k);
        return writes[key];
      }
    }
  }
  mutable std::unordered_set<uint64_t> writes_final; // writes_param results that are independent of guesswork

  // Does the code change the place with that name: assigning (including to an element/field)
  // or transferring to a parameter that changes the function being called? Shadow, stop.
  bool writes_name(node_id n, symbol name) const {
    if(!n || db.is<n_fn>(n)) return false;
    if(symbol h = db.hide_name(n)) return h == name; // Hide Self - Changing the location of the caller
    if(db.is<n_var>(n) && db.as<n_var>(n).ref) { // auto&e = name[i] - the entry in e changes the name
      node_id t = db.as<n_var>(n).init;
      while(db.is_index(t)) t = db.index_of(t).object;
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
        while(db.is_index(t) || db.is_member(t))
          t = db.is_index(t) ? db.index_of(t).object : db.member_of(t).object;
        if(db.is<n_ident>(t) && db.as<n_ident>(t).name == name) return true;
      }
    }
    // The call (and the operator declared to be: a++ with a mut parameter) changes the place-argument
    auto via = [&](symbol callee, node_list const & args) {
      if(auto it = functions.find(callee); it != functions.end())
        for(uint32_t k = 0; k < args.size(); ++k) {
          node_id a = args[k];
          while(db.is_index(a) || db.is_member(a))
            a = db.is_index(a) ? db.index_of(a).object : db.member_of(a).object;
          if(!db.is<n_ident>(a) || db.as<n_ident>(a).name != name) continue;
          for(node_id fn: it->second)
            if(writes_param(fn, k)) return true;
        }
      return false;
    };
    if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee) && via(db.as<n_ident>(db.as<n_call>(n).callee).name, db.as<n_call>(n).args)) return true;
    if(db.is<n_call>(n) && db.is_member(db.as<n_call>(n).callee)) { // a.f(b) f(a, b) (§4.5): a is the first argument
      auto const & m = db.member_of(db.as<n_call>(n).callee);
      node_list args{m.object};
      args.insert(args.end(), db.as<n_call>(n).args.begin(), db.as<n_call>(n).args.end());
      if(via(m.name, args)) return true;
    }
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
    // external ct node instance - source of code changes (§7.4) and canonical order key
    auto saved_top = ct_top;
    node_id saved_source = ct_source;
    if(!ct_depth) {
      ct_top = key;
      if(!elaborating) ct_source = n;
    }
    ++ct_depth;
    flow f;
    try {
      f = eval(c.body, env, frame);
    } catch(...) {
      --ct_depth;
      ct_top = saved_top;
      ct_source = saved_source;
      throw;
    }
    --ct_depth;
    ct_top = saved_top;
    ct_source = saved_source;
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
    ++checks; // can drop on execution: convolution JIT in constant would lose the exception
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

  // intercept(тело, блоки) is the exception kernel (§8.1), on it the library try { … } catch(E e)
  // { … }: body and blocks are expr-arguments, blocks are pack pack catch. Catch the first block,
  // whose parameter takes the type of value thrown (auto - any). Break/continue is no exception:
  // they are caught by loop, catch(auto e) does not see them. alloc_in(r, тело) is the core of
  // with_alloc: values created in the body are in the memory level r
  //
  // 
// 
// 
  flow alloc_in(node_list const & args, scope * env, uint32_t frame, span where) {
    if(args.size() != 2) fail("alloc_in(уровень, тело)", where);
    TZ_TRY(r, eval(args[0], env, frame));
    value const * sh = shape_of(r);
    if(!sh || !sh->is<agg_v>() || types.name(sh->as<agg_v>().type) != "region") fail(cat("with_alloc: ожидался уровень памяти region, получено ", types.describe(r)), where);
    if(checking() || !r.known()) return eval(args[1], env, frame);
    int64_t id = r.as<agg_v>().fields[0].second.as<int64_t>();
    address_space * saved = current_space;
    current_space = &vm_root->regions[std::size_t(id)];
    struct back {
      address_space * s;
      ~back() { current_space = s; }
    } restore{saved};
    return eval(args[1], env, frame);
  }

  flow intercept(node_list const & args, scope * env, uint32_t frame, span where) {
    if(args.size() != 2) fail("intercept(тело, блоки)", where);
    thunk_v hs = thunk_arg(args[1], env, where);
    node_list handlers = db.handlers_of(hs.expr);
    std::vector<value> thrown; // throws on the run-time-branches of the body (check mode)
    throw_sinks.push_back(&thrown);
    flow r;
    try {
      r = eval(args[0], env, frame);
    } catch(diagnostic const & d) {
      if(d.kind != diag_kind::runtime || d.fixed) {
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
      for(auto & v: thrown) (want.empty() || types.is_a(exception_type(v), want) ? hit : rest).push_back(std::move(v)); // and heirs
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
    value * cell = db.is<n_ident>(arg) ? find(env, db.as<n_ident>(arg).name) : nullptr;
    if(!cell || !cell->is<thunk_v>()) fail("ожидался expr-параметр", where);
    thunk_v t = cell->as<thunk_v>();
    while(db.is<n_ident>(t.expr))
      if(value * c = find(t.env, db.as<n_ident>(t.expr).name); c && c->is<thunk_v>()) t = c->as<thunk_v>();
      else break;
    return t;
  }

  // Calling a construction block with arguments: body(xs[k]) in for. The block is
  // the part of the caller (semantics inline): return and break from it go further.
  flow call_block(thunk_v const & t, node_list const & args, scope * env, uint32_t frame, span where) {
    auto const & l = db.as<n_lambda>(t.expr);
    if(l.params.size() != args.size()) fail(cat("блок ждёт ", l.params.size(), " аргументов, передано ", args.size()), where);
    ruled_hold rh{*this, t, ruled_env(t)};
    scope * inner = new_scope(rh.s);
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

  flow eval_node(node_id, n_extend const &, scope *, uint32_t) { return ok(); } // Expansion - when loading the module

  // --- code as a value (§7.4) ---

  bool editing_program = false; // Application of the editing function: code_replace ... change the program
  uint32_t program_edits = 0;
  // Apply the editing function (without parameters): its code_replace, code_rename ... change
  // the program through the base (journal, editor - as a human edit).
  uint32_t apply_edit(node_id f) {
    elaborate();
    editing_program = true;
    struct off {
      bool & flag;
      ~off() { flag = false; }
    } guard{editing_program};
    auto const & fn = db.as<n_fn>(f);
    scope * inner = new_scope(globals);
    scope_hold hold{*this, inner};
    flow r = run_frame(fn.body, inner, fn.name, fn.outline);
    if(r.kind == flow_kind::thr) uncaught(r);
    return program_edits;
  }

  // parsing is a code fact-finding program (code_types...): when parsing is itself, when
  // executed it is its parsing, once.
  std::list<interpreter> analysis_vm; // List: Type not yet complete (as choice_vms)
  interpreter & analysis() {
    if(checking()) return *this; // parsing (and elaboration in performance) - observations
    if(on_bg_lane) fail("разбор программы из фонового процесса — нельзя: он меняет базу (полосы, шаг 12.3b)", {});
    if(vm_root != this) return vm_root->analysis();
    if(analysis_vm.empty()) {
      analysis_vm.emplace_back(db, mod, std::vector<std::string>{prog_path}, mode::check);
      try {
        analysis_vm.back().run_main();
      } catch(diagnostic const &) {
      }
    }
    return analysis_vm.back();
  }

  // --- Code communication: name announcement, overload call, use ---
  static bool relation_builtin(std::string_view n) {
    for(std::string_view x: {"code_declaration", "code_targets", "code_users", "code_callers", "code_calls", "code_deps", "code_instances", "code_types", "code_values"})
      if(x == n) return true;
    return false;
  }
  // Bypassing the subtree (without attached function announcements - they have their own).
  void walk_code(node_id c, auto && f) {
    if(!c) return;
    f(c);
    db.for_each_child(c, [&](node_id x) {
      if(!db.is<n_fn>(x) || x == c) walk_code(x, f);
    });
  }
  bool within_code(node_id x, node_id top) const {
    for(; x; x = db[x].parent)
      if(x == top) return true;
    return false;
  }
  // Announce name by area: blocks up (announcement to place), function or lambda
  // parameters, then module (function, module variable) and prelude.
  node_id declaration_of(node_id c) {
    if(db.is<n_fn>(c) || db.is<n_var>(c) || db.is<n_param>(c)) return c;
    if(!db.is<n_ident>(c)) return {};
    symbol s = db.as<n_ident>(c).name;
    for(node_id at = c, p = db[c].parent; p; at = p, p = db[p].parent) {
      if(db.is<n_block>(p))
        for(node_id x: db.as<n_block>(p).items) {
          if(x == at || within_code(at, x)) { // `a = …` without the previous announcement - it's itself
            if(db.is<n_op>(x) && str(db.as<n_op>(x).op) == "=" && db.as<n_op>(x).args.size() == 2 && db.as<n_op>(x).args[0] == c) return c;
            break;
          }
          if(db.is<n_var>(x) && db.as<n_var>(x).name == s) return x;
          if(db.is<n_op>(x) && str(db.as<n_op>(x).op) == "=" && db.as<n_op>(x).args.size() == 2 && db.is<n_ident>(db.as<n_op>(x).args[0]) && db.as<n_ident>(db.as<n_op>(x).args[0]).name == s) return db.as<n_op>(x).args[0];
        }
      if(db.is<n_fn>(p))
        for(node_id x: db.as<n_fn>(p).params)
          if(db.as<n_param>(x).name == s) return x;
      if(db.is<n_lambda>(p))
        for(node_id x: db.as<n_lambda>(p).params)
          if(db.is<n_param>(x) && db.as<n_param>(x).name == s) return x;
    }
    for(node_id m: {mod, db.prelude}) {
      if(!m || !db.is<n_module>(m)) continue;
      for(node_id x: db.as<n_module>(m).items) {
        if(db.is<n_fn>(x) && db.as<n_fn>(x).name == s) return x;
        if(db.is<n_var>(x) && db.as<n_var>(x).name == s) return x;
      }
    }
    return {};
  }
  // overload call: selected by parsing (by instance), otherwise all with that name.
  std::vector<node_id> targets_of(node_id call) {
    std::vector<node_id> out;
    if(!db.is<n_call>(call)) return out;
    if(auto it = vm_root->resolved_calls.find(call.value); it != vm_root->resolved_calls.end())
      for(uint32_t f: it->second) out.push_back(node_id(f));
    if(!out.empty()) return out;
    node_id callee = db.as<n_call>(call).callee;
    symbol s = db.is<n_ident>(callee) ? db.as<n_ident>(callee).name : db.is_member(callee) ? db.member_of(callee).name : symbol{};
    if(auto it = functions.find(s); s && it != functions.end()) out = it->second;
    return out;
  }
  // Ad uses: program names (with modules used) that lead to it; functions and calls that select
  // it.
  std::vector<node_id> users_of(node_id d) {
    std::vector<node_id> out;
    if(!d) return out;
    bool fn = db.is<n_fn>(d);
    symbol s = fn ? db.as<n_fn>(d).name : db.is<n_var>(d) ? db.as<n_var>(d).name : db.is<n_param>(d) ? db.as<n_param>(d).name : db.is<n_ident>(d) ? db.as<n_ident>(d).name : symbol{};
    if(!s) return out;
    auto look = [&](auto && self, node_id x) -> void {
      if(db.is<n_ident>(x) && db.as<n_ident>(x).name == s && x != d) {
        node_id up = db[x].parent;
        bool call = up && db.is<n_call>(up) && db.as<n_call>(up).callee == x;
        bool hit = fn ? (call ? std::ranges::contains(targets_of(up), d) : declaration_of(x) == d) : declaration_of(x) == d;
        if(hit) out.push_back(x);
      }
      db.for_each_child(x, [&](node_id y) { self(self, y); });
    };
    look(look, mod);
    return out;
  }

  value code_value(node_id n) {
    agg_v c{types.named("code"), {}};
    c.fields.push_back({sym("node"), value(int64_t(n.value))});
    return value(std::move(c));
  }

  node_id code_of(node_id e, scope * env) {
    while(db.is<n_ident>(e))
      if(value * c = env->find(db.as<n_ident>(e).name); c && c->is<thunk_v>()) {
        e = c->as<thunk_v>().expr;
        env = c->as<thunk_v>().env;
      } else break;
    if(db.is<n_ident>(e) && !env->find(db.as<n_ident>(e).name)) { // function name - its declaration (first overload)
      if(auto it = functions.find(db.as<n_ident>(e).name); it != functions.end() && !it->second.empty()) return it->second.front();
      if(auto it = structs.find(db.as<n_ident>(e).name); it != structs.end()) return it->second; // Struct's name is his announcement
    }
    if(db.is<n_raw>(e) && db.as<n_raw>(e).rule) return raw_result(e); // The block disassembled by the code - its result
    return e;
  }

  // node fields in order (as in its struct program): node - code (empty - code without node,
  // given - lie), list node and names - array, name and text - string.
  value field_value(auto const & f) {
    using field_t = std::remove_cvref_t<decltype(f)>;
    if constexpr(std::is_same_v<field_t, node_id>) return code_value(f);
    else if constexpr(std::is_same_v<field_t, node_list>) {
      std::vector<value> ys;
      for(node_id y: f) ys.push_back(code_value(y));
      return value(array_v::of(std::move(ys)));
    } else if constexpr(std::is_same_v<field_t, std::vector<symbol>>) {
      std::vector<value> ys;
      for(symbol y: f) ys.push_back(value(std::string(y ? str(y) : ""))); // an empty name, like a field name
      return value(array_v::of(std::move(ys)));
    } else if constexpr(std::is_same_v<field_t, std::vector<uint32_t>>) { // layout
      std::vector<value> ys;
      for(uint32_t y: f) ys.push_back(value(int64_t(y)));
      return value(array_v::of(std::move(ys)));
    } else if constexpr(std::is_same_v<field_t, symbol>) return value(std::string(f ? str(f) : ""));
    else if constexpr(std::is_same_v<field_t, bool>) return value(f);
    else if constexpr(std::is_floating_point_v<field_t>) return value(double(f));
    else if constexpr(std::is_same_v<field_t, std::string>) return value(f);
    else return value(int64_t(f));
  }
  std::vector<value> node_fields(node_id c) {
    std::vector<value> xs;
    if(db.is<n_node>(c)) { // Type of program: fields - node in order of announcement
      for(node_id x: db.as<n_node>(c).items) xs.push_back(field_value(x));
      return xs;
    }
    std::visit([&](auto const & k) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(k)>>()) xs.push_back(field_value(k.[:m:]));
    },
        db[c].kind);
    return xs;
  }
  // Sample field for parsing (node - run-time): type from the structure node
  value field_sample(auto const & f) {
    using field_t = std::remove_cvref_t<decltype(f)>;
    value code = code_value(node_id(1));
    if constexpr(std::is_same_v<field_t, node_id>) return erase(code);
    else if constexpr(std::is_same_v<field_t, node_list>) return erase(value(array_v::of({code})));
    else if constexpr(std::is_same_v<field_t, std::vector<symbol>>) return erase(value(array_v::of({value(std::string("x"))})));
    else if constexpr(std::is_same_v<field_t, std::vector<uint32_t>>) return erase(value(array_v::of({value(int64_t(1))})));
    else if constexpr(std::is_same_v<field_t, symbol> || std::is_same_v<field_t, std::string>) return erase(value(std::string("x")));
    else if constexpr(std::is_same_v<field_t, bool>) return erase(value(true));
    else if constexpr(std::is_floating_point_v<field_t>) return erase(value(1.0));
    else return erase(value(int64_t(1)));
  }
  std::vector<value> node_samples(std::string_view kind) {
    std::vector<value> xs;
    if(auto pk = program_kind(db, kind)) {
      for(std::size_t k = 0; k < pk->size(); ++k) xs.push_back(field_sample(node_id{}));
      return xs;
    }
    template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) {
      if(lang_kind_name(std::meta::identifier_of(t)) == kind) {
        [[maybe_unused]] typename[:t:] x{};
        template for(constexpr auto m: fields_of<typename[:t:]>()) xs.push_back(field_sample(x.[:m:]));
      }
    }
    return xs;
  }

  // --- node - living values-code (§7.4 ) --------------------------------------------- Author's
  // decision 2026-10-02: n_X {…} creates a real base node and gives the code, e. field reads the
  // field node , e. field = v changes the node during elaboration (journal with source, rollback).
  //
//;
  // 
//.

  bool code_agg(value const & v) { // code type number - once (hot path: fields and assignment of aggregates)
    if(!v.is<agg_v>()) return false;
    if(!code_type) code_type = types.named("code");
    return v.as<agg_v>().type == code_type;
  }
  type_id code_type;

  // node field by name: value (node known) or pattern (run-time: field type - in the first
  // species with such field; different types in different species - unknown)
  std::optional<value> node_field(node_id c, symbol name) {
    std::optional<value> out;
    if(db.is<n_node>(c)) { // Type of program: field - by declaring struct ast. X
      auto const & u = db.as<n_node>(c);
      if(auto pk = program_kind(db, u.kind))
        for(std::size_t k = 0; k < pk->size() && k < u.items.size(); ++k)
          if((*pk)[k] == name) out = field_value(u.items[k]);
      return out;
    }
    std::visit([&](auto const & k) {
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(k)>>())
        if(lang_field(std::meta::identifier_of(m)) == str(name)) out = field_value(k.[:m:]);
    },
        db[c].kind);
    return out;
  }
  std::optional<value> node_field_sample(symbol name) {
    std::optional<value> out;
    bool mixed = false;
    template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) {
      [[maybe_unused]] typename[:t:] x{};
      template for(constexpr auto m: fields_of<typename[:t:]>())
        if(lang_field(std::meta::identifier_of(m)) == str(name)) {
          value v = field_sample(x.[:m:]);
          if(out && types.describe(*out) != types.describe(v)) mixed = true;
          if(!out) out = std::move(v);
        }
    }
    if(mixed) return value(rt_v{});
    if(!out) return field_sample(node_id{}); // Program type field - node (struct ast.X { code x; })
    return out;
  }

  // Derivatives node subtree holds this interpreter: they will not be retaken by another
  void hold_tree(node_id n) {
    if(!n || n.value >= db.size()) return;
    if(db.born.contains(n.value)) db.hold(n.value, derived_nodes);
    db.for_each_child(n, [&](node_id c) { hold_tree(c); });
  }

  // A value-code node (empty code is an empty node); not a code is a value node like the
  // #x insert (number, string, type, function).
  node_id child_node(value const & v, std::string_view what, span where) {
    if(code_agg(v)) {
      value const & nd = v.as<agg_v>().fields[0].second;
      if(nd.is<int64_t>()) return node_id(uint32_t(nd.as<int64_t>()));
    }
    if(v.is<array_v>() || v.is<unit_v>()) fail(cat(what, ": ожидался код, получено ", types.describe(v)), where);
    return value_node(v, where);
  }
  // A child of a new kind node owner: a node without a parent passes to him; a node already
  // standing in a tree (in another place or a second time) is copied. node has one place in the
  // tree - the insert does not take it away from the original (quote, function).
  node_id adopt(node_id c, node_id owner, std::set<uint32_t> & taken) {
    if(!c) return c;
    bool elsewhere = db[c].parent && db[c].parent != owner;
    if(elsewhere || !taken.insert(c.value).second) {
      c = db.clone(c, [](node_id) -> std::optional<node_id> { return std::nullopt; }, &derived_nodes);
      taken.insert(c.value);
    }
    return c;
  }
  // The field of the form node from the value: type - as in the struct node program (node_field_type).
  void set_field(auto & f, value const & v, std::string_view what, node_id owner, std::set<uint32_t> & taken, span where) {
    using field_t = std::remove_cvref_t<decltype(f)>;
    auto need = [&](bool ok, std::string_view type) {
      if(!ok) fail(cat(what, ": ожидалось ", type, ", получено ", types.describe(v)), where);
    };
    auto items = [&](auto && each) {
      need(v.is<array_v>(), "array");
      for(uint32_t k = 0; k < v.as<array_v>().size(); ++k) each(v.as<array_v>().get(k));
    };
    if constexpr(std::is_same_v<field_t, node_id>) f = adopt(child_node(v, what, where), owner, taken);
    else if constexpr(std::is_same_v<field_t, node_list>) {
      field_t xs;
      items([&](value const & x) { xs.push_back(adopt(child_node(x, what, where), owner, taken)); });
      f = std::move(xs);
    } else if constexpr(std::is_same_v<field_t, std::vector<symbol>>) {
      field_t xs;
      items([&](value const & x) {
        if(!x.is<str_v>()) fail(cat(what, ": ожидались строки, получено ", types.describe(x)), where);
        xs.push_back(x.as<str_v>().view().empty() ? symbol{} : sym(x.as<str_v>().view()));
      });
      f = std::move(xs);
    } else if constexpr(std::is_same_v<field_t, std::vector<uint32_t>>) {
      field_t xs;
      items([&](value const & x) {
        if(!x.is<int64_t>()) fail(cat(what, ": ожидались числа, получено ", types.describe(x)), where);
        xs.push_back(uint32_t(x.as<int64_t>()));
      });
      f = std::move(xs);
    } else if constexpr(std::is_same_v<field_t, symbol>) {
      need(v.is<str_v>(), types.str_name());
      f = v.as<str_v>().view().empty() ? symbol{} : sym(v.as<str_v>().view());
    } else if constexpr(std::is_same_v<field_t, std::string>) {
      need(v.is<str_v>(), types.str_name());
      f = std::string(v.as<str_v>().view());
    } else if constexpr(std::is_same_v<field_t, bool>) {
      need(v.is<bool>(), types.bool_name());
      f = v.as<bool>();
    } else if constexpr(std::is_floating_point_v<field_t>) {
      need(v.is<double>() || v.is<int64_t>(), types.float_name());
      f = v.is<double>() ? v.as<double>() : double(v.as<int64_t>());
    } else {
      need(v.is<int64_t>(), types.int_name());
      f = field_t(v.as<int64_t>());
    }
  }

  // n_X {a, b, …} is a new node of the form n_X, field by position (missing - empty); run-time
  // field (in parsing) - code unknown Type node - VM (ast.if) or declared program (struct
  // ast.twice { code x; }).
  bool node_kind_named(std::string_view k) const { return is_node_kind(k) || (k.starts_with("ast.") && program_kind(db, k)); }

  value node_literal(std::string_view kind, std::vector<value> const & items, span where) {
    for(auto const & x: items)
      if(!x.known()) return erase(code_value(node_id(1)));
    if(auto pk = program_kind(db, kind)) { // Type of program: fields - node in order of announcement
      if(items.size() > pk->size()) fail(cat(kind, ": полей ", pk->size(), ", значений ", items.size()), where);
      n_node u{std::string(kind), {}};
      for(std::size_t k = 0; k < pk->size(); ++k)
        u.items.push_back(k < items.size() ? code_node(items[k], where) : node_id{});
      node_id n = db.derive(std::move(u), where, derived_nodes);
      db.link_parents(n);
      return code_value(n);
    }
    std::optional<node_kind> out;
    std::set<uint32_t> taken;
    template for(constexpr auto t: std::define_static_array(std::meta::template_arguments_of(std::meta::dealias(^^node_kind)))) {
      if(lang_kind_name(std::meta::identifier_of(t)) == kind) {
        typename[:t:] x{};
        uint32_t k = 0;
        template for(constexpr auto m: fields_of<typename[:t:]>()) {
          if(k < items.size()) set_field(x.[:m:], items[k], cat(kind, ".", lang_field(std::meta::identifier_of(m))), node_id{}, taken, where);
          ++k;
        }
        if(items.size() > k) fail(cat(kind, ": полей ", k, ", значений ", items.size()), where);
        out = node_kind(std::move(x));
      }
    }
    node_id n = db.derive(std::move(*out), where, derived_nodes);
    db.link_parents(n);
    return code_value(n);
  }

  // e.field = v is a node mutation: only in elaboration is the ct module and ct code instance in the
  // parsing program (instantiation_pass), with the code evoked from them.
  //  
// - the execution code only reads.
  void mutate_node(node_id c, symbol name, value const & v, span where) {
    bool own = deriving && db.birth_of(c) > derive_base; // node created by the parsing block - completion
    bool inst = !elaborating && !own && ct_depth && !deriving; // ct code instance
    if(!elaborating && !own && !inst)
      fail(deriving ? "изменение кода из правила разбора блока — только узлов его итога" : "изменение кода — только при компиляции (ct-код модуля или экземпляра)", where);
    if(!c) fail("изменение кода: пустой узел", where);
    if(!v.known()) fail(cat("изменение кода: значение поля ", str(name), " должно быть известно при компиляции"), where);
    if(inst && !instantiating && done_by_pass(where)) return;
    node_kind k = db[c].kind; // copy: its appearance in node - on the field
    bool found = false;
    if(auto * u = std::get_if<n_node>(&k)) { // Type of program: field - node on the announcement
      if(auto pk = program_kind(db, u->kind))
        for(std::size_t i = 0; i < pk->size() && i < u->items.size(); ++i)
          if((*pk)[i] == name) {
            std::set<uint32_t> taken;
            for(std::size_t j = 0; j < u->items.size(); ++j)
              if(j != i) taken.insert(u->items[j].value);
            set_field(u->items[i], v, cat(u->kind, ".", str(name)), c, taken, where);
            found = true;
          }
    } else
    std::visit([&](auto & x) {
      std::set<uint32_t> taken; // Children of other node fields are busy
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>()) {
        using field_t = [:std::meta::type_of(m):];
        if(lang_field(std::meta::identifier_of(m)) != str(name)) {
          if constexpr(std::is_same_v<field_t, node_id>) taken.insert(x.[:m:].value);
          else if constexpr(std::is_same_v<field_t, node_list>)
            for(node_id y: x.[:m:]) taken.insert(y.value);
        }
      }
      template for(constexpr auto m: fields_of<std::remove_cvref_t<decltype(x)>>())
        if(lang_field(std::meta::identifier_of(m)) == str(name)) {
          found = true;
          set_field(x.[:m:], v, cat(lang_kind_of(db[c]), ".", str(name)), c, taken, where);
        }
    },
        k);
    if(!found) fail(cat("у узла ", lang_kind_of(db[c]), " нет поля ", str(name)), where);
    if(own) {
      db.mutate(c, std::move(k), 0);
      db.link_parents(c);
      return;
    }
    if(inst) { // parsing elaboration: change with source - ct- node instance
      if(db.birth_of(c) <= pass_base) // It is impossible to change the already disassembled run-time code: the execution and parsing would diverge
        if(node_id u = used_within(c)) {
          code_failed = true;
          diagnostic d(cat("изменение кода после его использования: ", lang_kind_of(db[c]), " уже разобран в каноническом порядке инстанцирования — менять код можно до первого использования"), where);
          d.note("использован здесь", db[u].where);
          for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
          throw d;
        }
      if(db.birth_of(c) <= pass_base) code_changes.push_back({db[ct_source].where, stack.empty() ? std::string("модуль") : stack.back().what, cat(lang_kind_of(db[c]), ".", str(name))});
      db.mutate(c, std::move(k), db.birth_of(c) > pass_base ? 0 : ct_source.value + 1);
      db.link_parents(c);
      return;
    }
    if(elab_nested && db.birth_of(c) <= elab_base) return; // Written knot changes external elaboration
    // node created by this elaboration is completion (without writing and rollback): nested
    // elaboration (parsing, JIT) rolls back only mutations of the written
    db.mutate(c, std::move(k), db.birth_of(c) > elab_base ? 0 : ct_source.value + 1);
    db.link_parents(c);
  }

  // ct code instance outside parsing elaboration: its code changes made parsing. parsing inside
  // the ongoing elaboration (sample, static_level) does not change - it changes; launch - only
  // instance, which parsing passed in canonical order.
  bool done_by_pass(span where) {
    if(db.elab_depth) return true;
    if(db.elab_current() && (checking() || db.elab_ct.contains(ct_top))) return true;
    if(db.elab_pass_error) {
      diagnostic d(cat("изменение кода из ct-кода экземпляра: разбор программы при элаборации не прошёл — ", db.elab_pass_error->what()), where);
      d.note("ошибка разбора", db.elab_pass_error->where);
      throw d;
    }
    fail("изменение кода из ct-кода экземпляра, которого нет в каноническом порядке инстанцирования (разбор программы при элаборации)", where);
  }
  // Subtree node c, already disassembled as run-time-code (empty no); first in order and deeper
  node_id used_within(node_id c) const {
    node_id found;
    db.for_each_child(c, [&](node_id x) {
      if(!found) found = used_within(x);
    });
    return found ? found : used_rt.contains(c.value) ? c : node_id{};
  }

  // The object of assignment is code: the name that stores the code, or a chain of fields from it
  // (f.body.items = ...); it can be calculated without side effects.
  bool code_place(node_id n, scope * env) {
    while(db.is_member(n)) n = db.member_of(n).object;
    if(!db.is<n_ident>(n)) return false;
    value const * c = find(env, db.as<n_ident>(n).name);
    return c && (code_agg(*c) || (c->is<rt_v>() && shape_of(*c) && code_agg(*shape_of(*c))));
  }

  node_id code_node(value const & v, span where) {
    if(v.is<agg_v>() && types.name(v.as<agg_v>().type) == "code" && v.as<agg_v>().fields[0].second.is<int64_t>())
      if(node_id c(uint32_t(v.as<agg_v>().fields[0].second.as<int64_t>())); c) return c;
    fail(v.is<agg_v>() ? "пустой узел (поле узла без значения: проверьте given)" : "ожидался код (quote)", where);
  }

  // The value > node for inserting #x.
  node_id value_node(value const & v, span where) {
    if(v.is<int64_t>()) return db.derive(n_int{v.as<int64_t>()}, where, derived_nodes);
    if(v.is<double>()) return db.derive(n_float{v.as<double>()}, where, derived_nodes);
    if(v.is<str_v>()) return db.derive(n_str{v.as<str_v>().str()}, where, derived_nodes);
    if(v.is<bool>()) return db.derive(n_ident{sym(v.as<bool>() ? "true" : "false")}, where, derived_nodes);
    if(v.is<type_v>()) return db.derive(n_ident{sym(types.name(v.as<type_v>().type))}, where, derived_nodes);
    if(v.is<fn_v>() && v.as<fn_v>().name) return db.derive(n_ident{v.as<fn_v>().name}, where, derived_nodes);
    if(v.is<agg_v>() && types.name(v.as<agg_v>().type) == "code") return code_node(v, where);
    // aggregate and array - constructor by positions: Name {…}, array {…} (*my*: the value of the
    // parsing block code becomes the code that builds it)
    if(v.known() && (v.is<agg_v>() || v.is<array_v>())) {
      n_construct c;
      bool anon = v.is<agg_v>() && types.anonymous(v.as<agg_v>().type); // {x: …} - no type, field by name
      if(!anon) c.type = db.derive(n_ident{sym(v.is<agg_v>() ? types.name(v.as<agg_v>().type) : "array")}, where, derived_nodes);
      if(v.is<agg_v>())
        for(auto const & f: v.as<agg_v>().fields) {
          if(anon) c.names.push_back(f.first);
          c.items.push_back(value_node(f.second, where));
        }
      else
        for(uint32_t k = 0; k < v.as<array_v>().size(); ++k) c.items.push_back(value_node(v.as<array_v>().get(k), where));
      node_id n = db.derive(std::move(c), where, derived_nodes);
      db.link_parents(n);
      return n;
    }
    fail(cat("#: значение типа ", types.describe(v), " нельзя вставить в код"), where);
  }

  // --- parsing code (§4.4): raw block → node ----------------------------------- Construction block
  // with the rule `syntax {r} => f(r);` is stored raw (n_raw); node from it makes f when analyzed -
  // lazy, like the instantiation template:
// The result is derivative.
  // (ast_db::derived): editing a block, structure, or any database text recalculates it; it is
  // neither human editing nor a mutation from the journal source. The parsing error is a hole with
  // a cause in place of an atom.
  node_id raw_result(node_id n) {
    auto const & r = db.as<n_raw>(n);
    node_id decl(r.rule ? r.rule - 1 : 0);
    node_id rule = r.rule ? raw_rule_of(db, decl) : node_id{};
    std::array<uint64_t, 4> key{db.node_revision(n), r.rule ? db.node_revision(decl) : 0, rule ? db.node_revision(rule) : 0, db.edits};
    if(auto it = db.derived.find(n.value); it != db.derived.end()) {
      if(it->second.key == key) return node_id(it->second.node);
      db.derived.erase(it); // The previous result is free: the same result will take it node
    }
    std::size_t reborn_from = db.reborn.size();
    span w = db[n].where;
    auto hole = [&](std::string why, span at) {
      uint32_t saved = std::exchange(db.pinned_source, at.source);
      node_id h = db.derive(n_hole{r.text, std::move(why)}, at, derived_nodes);
      db.pinned_source = saved;
      return h;
    };
    node_id out;
    if(!rule) out = hole(r.rule ? "у конструкции нет правила разбора блока: syntax {r} => …" : "подблок разбирает правило конструкции", w);
    else {
      // rule - ct-code over the raw block: executed immediately (check mode does not erase it)
      auto saved_mode = std::exchange(run_mode, mode::run);
      auto saved_deriving = std::exchange(deriving, n);
      uint64_t saved_base = std::exchange(derive_base, db.births);
      uint32_t saved_pin = std::exchange(db.pinned_source, w.source);
      struct restore {
        interpreter & vm;
        mode m;
        node_id d;
        uint64_t b;
        uint32_t p;
        ~restore() {
          vm.run_mode = m;
          vm.deriving = d;
          vm.derive_base = b;
          vm.db.pinned_source = p;
          --vm.ct_depth;
        }
      } hold{*this, saved_mode, saved_deriving, saved_base, saved_pin};
      ++ct_depth;
      try {
        scope * s = new_scope(globals);
        scope_hold sh{*this, s};
        s->declare(db.as<n_param>(db.as<n_fn>(rule).params[0]).name, code_value(n));
        flow f = run_frame(db.as<n_fn>(rule).body, s, sym("{}"), false);
        if(f.kind == flow_kind::thr) out = hole(cat("разбор блока: исключение ", exception_type(f.v), ": ", show_v(f.v)), w);
        else if(code_agg(f.v)) out = code_node(f.v, w);
        else if(f.v.is<agg_v>() && types.is_a(types.describe(f.v), "parse_error")) { // parsing error in place of the atom
          auto const & e = f.v.as<agg_v>().fields;
          span at = w;
          if(code_agg(e[0].second) && e[0].second.as<agg_v>().fields[0].second.is<int64_t>() && e[0].second.as<agg_v>().fields[0].second.as<int64_t>())
            at = db[code_node(e[0].second, w)].where;
          // parse_error_in {атом, причина, смещение} is the place inside the atom (parsing characters)
          if(e.size() > 2 && e[2].second.is<int64_t>() && e[2].second.as<int64_t>() > 0 && at.begin + uint64_t(e[2].second.as<int64_t>()) <= at.end)
            at.begin += uint32_t(e[2].second.as<int64_t>());
          out = hole(cat("разбор блока: ", e[1].second.is<str_v>() ? std::string(e[1].second.as<str_v>().view()) : show_v(e[1].second)), at);
        } else out = value_node(f.v, w);
      } catch(diagnostic const & d) {
        out = hole(cat("разбор блока: ", d.what()), d.where.end > d.where.begin && d.where.source == w.source ? d.where : w);
      }
    }
    db[out].parent = n; // the names of the result - in the place of the block (the search goes up through the call)
    db.derived[n.value] = {key, out.value};
    db.flush_reborn(std::min(reborn_from, db.reborn.size()));
    return out;
  }

  // The text of an atom of a raw block is a piece of block text by positions (both from one parsing).
  std::optional<std::string_view> raw_atom_text(node_id c) const {
    node_id p = db[c].parent;
    if(!p || !db.is<n_raw>(p) || db.is<n_raw>(c)) return std::nullopt;
    span a = db[c].where, b = db[p].where;
    std::string_view t = db.as<n_raw>(p).text;
    if(a.source != b.source || a.begin < b.begin || a.end < a.begin || a.end - b.begin > t.size()) return std::nullopt;
    return t.substr(a.begin - b.begin, a.end - a.begin);
  }
  // The atom close to the previous element of its raw block (without a space) - in positions. The
  // first in the block is not glued (with the block bracket - does not count).
  bool raw_glued(node_id c) const {
    node_id p = db[c].parent;
    if(!p || !db.is<n_raw>(p)) return false;
    auto const & xs = db.as<n_raw>(p).items;
    for(size_t k = 1; k < xs.size(); ++k)
      if(xs[k] == c) return db[xs[k - 1]].where.end == db[c].where.begin && db[xs[k - 1]].where.source == db[c].where.source;
    return false;
  }

  flow eval_node(node_id n, n_raw const & r, scope * env, uint32_t frame) {
    if(!r.rule) fail("подблок сырого блока не исполняется: его разбирает правило конструкции", db[n].where);
    return eval(raw_result(n), env, frame);
  }
  flow eval_node(node_id n, n_sym const &, scope *, uint32_t) { fail("символ сырого блока не исполняется", db[n].where); }

  // parse_block(b) is a subblock {…} of the raw block by standard parsing according to the rules
  // of the block place (contexts around the call and the rules of the body of the design):
// Positions in
  // Errors are holes inside, like everywhere else.
  node_id parse_sub(node_id b, span where) {
    if(!db.is<n_raw>(b) || db.as<n_raw>(b).open != "{") fail("parse_block: ожидался подблок {…} сырого блока", where);
    node_id top = b;
    while(db[top].parent && db.is<n_raw>(db[top].parent)) top = db[top].parent;
    span w = db[b].where;
    uint32_t saved = std::exchange(db.pinned_source, w.source);
    struct restore {
      ast_db & db;
      uint32_t p;
      ~restore() { db.pinned_source = p; }
    } hold{db, saved};
    parser pr(db, w.source, w);
    pr.place(db[top].parent, top);
    node_id x = pr.parse_unit(true);
    if(!x) return db.add(n_hole{db.as<n_raw>(b).text, pr.errors().empty() ? "незакрытая скобка" : pr.errors().front().what()}, w);
    return x;
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
      value * cell = find(env, name);
      if(!cell) fail(cat("#", str(name), ": нет такого значения"), db[x].where);
      flow v = read(cell);
      if(!v.v.known()) fail(cat("#", str(name), ": значение неизвестно при компиляции"), db[x].where);
      return value_node(v.v, db[x].where);
    }, &derived_nodes);
    return ok(code_value(copy));
  }

  flow eval_node(node_id n, n_splice const & s, scope *, uint32_t) { fail(cat("#", str(s.name), " вне quote"), db[n].where); }

  // ref(f) - node-link to the ad f (function, struct, ct-variable module): in the created code
  // is not caught by the name of the place of insertion (author's decision 2026-10-02: names are
  // linked by the analysis at the insertion site, specific entity - reference). Argument: An ad
  // name or ad code.
  bool ref_target(node_id d) const { return db.is<n_fn>(d) || db.is_struct(d) || (db.is<n_var>(d) && db.as<n_var>(d).ct); }
  flow eval_ref(node_id arg, scope * env, uint32_t frame, span where) {
    node_id d = code_of(arg, env);
    if(value * c = db.is<n_ident>(d) ? find(env, db.as<n_ident>(d).name) : nullptr; c && ct_cells.contains(c)) // ct-variable module named
      for(node_id item: db.as<n_module>(mod).items)
        if(db.is<n_var>(item) && db.as<n_var>(item).ct && db.as<n_var>(item).name == db.as<n_ident>(d).name) d = item;
    if(!ref_target(d)) {
      TZ_TRY(v, eval(arg, env, frame));
      if(code_agg(v) && v.known()) d = code_node(v, where);
    }
    if(!ref_target(d)) fail("ref: ожидалось объявление — функция, struct или ct-переменная модуля", where);
    return ok(code_value(db.derive(n_ref{d.value}, where, derived_nodes)));
  }
  // entity of the goal: the function is its overload (under the current name, not in the area
  // of the place), the struct is the type, the ct-variable is its value.
  flow eval_node(node_id n, n_ref const & r, scope *, uint32_t) {
    node_id d(r.target);
    if(db.is<n_fn>(d)) return ok(value(fn_v{db.as<n_fn>(d).name, {}, nullptr, {}}));
    if(db.is_struct(d)) return ok(value(type_v{types.named(str(db.as<n_extend>(d).name))}));
    if(db.is<n_var>(d))
      if(value * c = globals->find(db.as<n_var>(d).name)) return read(c);
    fail("ref: цель ссылки не объявлена", db[n].where);
  }

  flow eval_try_compile(node_id n, node_id body, scope * env, uint32_t frame) {
    auto saved_may = may_return;
    try {
      return eval(body, env, frame);
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
    if(!c.type) { // {x: e, …} - anonymous aggregate, type - by structure
      field_list fs;
      for(uint32_t k = 0; k < c.items.size(); ++k) {
        for(auto const & [name, _]: fs)
          if(name == c.names[k]) fail(cat("{…}: поле ", str(name), " дважды"), db[c.items[k]].where);
        TZ_TRY(x, eval(c.items[k], env, frame));
        fs.push_back({c.names[k], std::move(x)});
      }
      return ok(record_value(std::move(fs)));
    }
    if(db.is<n_ident>(c.type)) service_guard(str(db.as<n_ident>(c.type).name), n, "создать");
    std::vector<value> items;
    bool open = false; // the run-time-length array is disclosed: the result is the run-time-length array
    for(node_id item: c.items) {
      if(db.is<n_spread>(item)) { // ...a are elements of the array
        if(str(db.as<n_ident>(c.type).name) != "array") fail("...: раскрытие — в array{…} или аргументах вызова", db[item].where);
        TZ_TRY(t, eval(db.as<n_spread>(item).value, env, frame));
        value const * x = shape_of(t);
        if(!x || !x->is<array_v>()) fail(cat("...: раскрывается массив или кортеж, получено ", types.describe(t)), db[item].where);
        if(!t.known() && t.is<rt_v>()) {
          open = true;
          items.push_back(element_type(t, db[item].where));
          continue;
        }
        for(uint32_t k = 0; k < x->as<array_v>().size(); ++k) items.push_back(x->as<array_v>().get(k));
        continue;
      }
      TZ_TRY(x0, eval(item, env, frame));
      TZ_TAKE(x, item, x0, env, frame);
      items.push_back(std::move(x));
    }
    auto tname = str(db.as<n_ident>(c.type).name);
    if(node_kind_named(tname)) return ok(node_literal(tname, items, db[n].where)); // Ast.X {…} - Live Base Node
    if(auto it = structs.find(db.as<n_ident>(c.type).name); it != structs.end()) { // Name {a, b} - by position n
      auto all = fields_of_struct(it->first);
      if(items.size() > all.size()) fail(cat(tname, ": полей ", all.size(), ", значений ", items.size()), db[n].where);
      agg_v g{types.named(tname), {}};
      for(std::size_t k = 0; k < all.size(); ++k) {
        auto const & f = db.as<n_var>(all[k]);
        if(k < items.size()) // Struct field: {x: …} becomes struct field
          g.fields.push_back({f.name, f.type && db.is<n_ident>(f.type) ? to_type(std::move(items[k]), str(db.as<n_ident>(f.type).name), db[c.items[k]].where) : std::move(items[k])});
        else if(f.init) {
          TZ_TRY(d, eval(f.init, globals, frame));
          g.fields.push_back({f.name, field_typed(f, std::move(d), db[f.init].where)}); // foo f = {} = value of field type
        } else fail(cat(tname, ": не задано поле ", str(f.name)), db[n].where);
        if(f.ct) pin(g.fields.back().second, f.name, db[n].where);
      }
      mark_resource(g);
      return ok(value(std::move(g)));
    }
    // Tag above representation: i64 {x} is the value represented in i64 (as i64 y = x): the tag is a
    // type as a value, its constructor is the same as the location of this type
    if(tname != "array" && types.builtin(tname) && items.size() == 1 && !open) return ok(placed(std::move(items[0]), db.as<n_ident>(c.type).name, false, db[c.items[0]].where));
    if(tname != "array") fail(cat("конструктор ", tname, "{} пока не поддержан"), db[n].where);
    for(auto const & x: items)
      if(!x.known() || open) { // at least one run-time element is an array of run-time; tuple is the length and types of elements in its type
        array_v all = array_v::of(items);
        if(!open && is_tuple(all)) return ok(erase(value(std::move(all))));
        return ok(erase(value(array_v::of({x}))));
      }
    return ok(value(array_v::of(std::move(items))));
  }

  // --- field access ---

  flow eval_member(node_id n, member_view const & m, scope * env, uint32_t frame) {
    value r;
    if(value const * o = peek(m.object, env); o && o->is<array_v>()) r = member(*o, m.name, db[n].where);
    else {
      TZ_TRY(obj, eval(m.object, env, frame));
      if(!vm_root->service_types.empty())
        if(value const * x = obj.is<agg_v>() ? &obj : shape_of(obj); x && x->is<agg_v>()) service_guard(types.name(x->as<agg_v>().type), n, "читать поля");
      r = lift(obj, [&](value const & o) { return member(o, m.name, db[n].where); });
    }
    // xs.size - the size of this array (fact: range gives its indexes from it)
    if(checking() && str(m.name) == "size" && db.is<n_ident>(m.object))
      if(value const * cell = peek(m.object, env); cell && (cell->is<array_v>() || shape_of(*cell)))
        r.proof = add_fact({fact::size_of, cell, db.as<n_ident>(m.object).name, book.version(cell)});
    return ok(std::move(r));
  }

  value member(value const & obj, symbol name, span where) {
    if(is_shared(obj) && str(name) != "slot") return member(slot_of(obj).v, name, where); // shareable
    if(obj.is<rt_v>()) {
      value const * shape = shape_of(obj);
      if(!shape) return value(rt_v{});
      if(code_agg(*shape) && str(name) != "node") { // node field: species unknown - type named field
        if(auto f = node_field_sample(name)) return *f;
        fail(cat("у узлов нет поля ", str(name)), where);
      }
      return erase(member(*shape, name, where));
    }
    if(code_agg(obj) && str(name) != "node") { // e.cond is the field node (§7.4)
      node_id c = code_node(obj, where);
      if(auto f = node_field(c, name)) return *f;
      fail(cat("у узла ", lang_kind_of(db[c]), " нет поля ", str(name)), where);
    }
    if(obj.is<fn_v>())
      if(auto c = entity_context(obj.as<fn_v>(), name)) return *c;
    if(str(name) == "size") {
      if(obj.is<array_v>()) return value(int64_t(obj.as<array_v>().size()));
      if(obj.is<str_v>()) return value(int64_t(obj.as<str_v>().size()));
    }
    if(obj.is<agg_v>())
      for(auto & [k, v]: obj.as<agg_v>().fields)
        if(k == name) return v;
    // Tag over representation (struct i64 { __int64 value; }): the value field itself
    if(str(name) == "value" && (obj.is<int64_t>() || obj.is<double>() || obj.is<bool>())) return obj;
    fail(cat("у значения типа ", types.describe(obj), " нет поля ", str(name)), where);
  }

  // A variable cell without copying a value (an array by name is not copied for a
  // single element or size).
  value const * peek(node_id n, scope * env) {
    if(!db.is<n_ident>(n)) return nullptr;
    value const * c = find(env, db.as<n_ident>(n).name);
    if(c && foreign_global(c, n, env, db.as<n_ident>(n).name)) return nullptr; // other variable module - the error will give eval
    if(c) not_hidden(*c, db.as<n_ident>(n).name, db[n].where);
    return c && !c->is<thunk_v>() ? c : nullptr;
  }

  // v.[f] is a field by descriptor (from fields(T)) or by line name; the descriptor is
  // known when compiled: the field cycle is disclosed (§7.4).
  flow eval_field(node_id n, field_view const & f, scope * env, uint32_t frame) {
    TZ_TRY(key, eval(f.key, env, frame));
    std::string name;
    if(key.is<str_v>()) name = key.as<str_v>();
    else if(key.is<agg_v>() && !key.as<agg_v>().fields.empty() && key.as<agg_v>().fields[0].second.is<str_v>())
      name = key.as<agg_v>().fields[0].second.as<str_v>();
    else fail(cat("v.[f]: дескриптор поля должен быть известен при компиляции, получено ", types.describe(key)), db[n].where);
    TZ_TRY(obj, eval(f.object, env, frame));
    return ok(lift(obj, [&](value const & o) { return member(o, sym(name), db[n].where); }));
  }

  flow eval_index(node_id n, index_view const & i, scope * env, uint32_t frame) {
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

  flow index_rt(node_id n, index_view const & i, value const & obj, value const & idx, scope * env) {
    span where = db[n].where;
    if(checking()) { // node array cell - for cycle invariant (jit_loop_keeps)
      value * cell = db.is<n_ident>(i.object) ? find(env, db.as<n_ident>(i.object).name) : nullptr;
      if(cell) index_cells[n.value].insert(cell);
      else index_cells[n.value].insert(nullptr);
    }
    // index type - property of instance, not values: place of use instantiation error
    // (§13.4), including when array and index is run-time
    auto index_type = [&](value const & x) {
      if(std::string t = types.describe(x); t != types.int_name()) fail(cat("индекс должен быть ", types.int_name(), ", получено ", t), where);
    };
    if(idx.is<split_v>())
      for(auto const & a: idx.as<split_v>().alts) index_type(a);
    else index_type(idx);
    bool proven_here = checking() && index_holds(i, idx, env);
    return ok(lift(obj, [&](value const & o) {
      if(value const * sh = shape_of(o); sh && sh->is<str_v>()) { // string - bytes (§4.0): s[k] - byte, i64
        if(o.known() && idx.known()) {
          auto const & str = o.as<str_v>();
          int64_t k = idx.as<int64_t>();
          if(k < 0 || uint64_t(k) >= str.size()) fail_rt("out_of_bounds", cat("индекс ", k, " вне строки длины ", str.size()), where);
          return value(int64_t(uint8_t(str[std::size_t(k)])));
        }
        if(checking()) rt_check("out_of_bounds", where);
        return erase(value(int64_t(1)));
      }
      if(!sh_array(o)) fail(cat("индексация значения типа ", types.describe(o)), where);
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

  bool sh_array(value const & o) const {
    value const * sh = shape_of(o);
    return !sh || sh->is<array_v>();
  }

  // An index is a safe index of this array (a fact on the value of the index).
  bool index_holds(index_view const & i, value const & idx, scope * env) {
    fact const * f = fact_of(idx, fact::index_of);
    if(!f || !db.is<n_ident>(i.object)) return false;
    value const * cell = find(env, db.as<n_ident>(i.object).name);
    return cell && holds(*f, cell, db.as<n_ident>(i.object).name);
  }

  // The range(xs.size) element on the safe index is the safe xs index.
  uint32_t index_result(index_view const & i, value const & arr, value const & idx, scope * env) {
    fact const * r = fact_of(arr, fact::range_of);
    if(!r || !index_holds(i, idx, env)) return 0;
    return add_fact({fact::index_of, r->cell, r->name, r->version});
  }

  void note_index(node_id n, bool proven_here) {
    if(!proven_here) {
      emit({report_event::unproven, {}, {}, {}, {}, {}, n.value});
      return;
    }
    auto const & i = db.index_of(n);
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

  // Program type node: execution - eval type (author's decision 2026-10-17:
// form - member of the species.
  // node fields are eval arguments in order, as in a call in place node: the expr field is calculated
  // in place (semantic inlining: return and break from it - out). parsing is the same execution (value
  // approach). No eval - the meaning of the view is only disclosure.
  flow eval_node(node_id n, n_node const & u, scope * env, uint32_t frame) {
    std::vector<node_id> evals;
    if(auto it = kind_members.find(sym(u.kind)); it != kind_members.end())
      for(node_id f: it->second)
        if(str(db.as<n_fn>(f).name) == "eval") evals.push_back(f);
    if(evals.empty())
      fail(cat("узел ", u.kind, " не раскрыт: смысл вида программы — раскрытие ([[transform]] f(", u.kind, " x) возвращает код из известных узлов) или ", u.kind, " { eval(…) => …; }"), db[n].where);
    for(node_id x: u.items)
      if(!x) fail(cat("узел ", u.kind, ": поле пусто — eval получает все поля"), db[n].where);
    return call_overloaded(evals, u.items, env, frame, {db[n].where, cat(u.kind, ".eval"), n.value, frame});
  }

  flow eval_node(node_id n, auto const &, scope *, uint32_t) {
    fail(cat("узел ", lang_kind_of(db[n]), " нельзя исполнить"), db[n].where);
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
  //
  // Associativity is semantics, not parsing: the property of operator announcements
  // seen in the context of execution.
// 
// 
// 
  // 
//  (, об
  // Invariants of operators: "association, priority".
  flow eval_node(node_id n, n_chain const & c, scope * env, uint32_t frame) {
    node_id tree = assoc_run(n, c, env) ? assoc_tree(n, c) : resolve_chain(n, c, env, chain_assoc(n, c, env));
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

  // Visible operator announcements: module elements and prelude, block operators in the field
  // (local functions, design body rules), using operator active function-contexts (checked,
  // amd64...). [[associative]] is an invariant in this context. *My:* context complements,
  // doesn't cancel
// - Selected "to supplement": so the former
  // programmes do not change the meaning.
  bool assoc_attr(node_id fn) {
    for(node_id a: db.as<n_fn>(fn).attrs)
      if(db.is<n_ident>(a) && db.as<n_ident>(a).name == known(sym_associative)) return true;
    return false;
  }
  symbol infix_entity(symbol op) {
    auto [it, fresh] = infix_entities.try_emplace(op.value);
    if(fresh) it->second = sym(cat("infix", str(op)));
    return it->second;
  }
  bool op_associative(symbol op, scope * env, node_id at) {
    if(op_associative(op, at)) return true;
    if(local_ops.empty() && overrides.empty()) return false;
    symbol f = infix_entity(op);
    if(local_ops.contains(f.value))
      if(value * cell = env->find(f); cell && cell->is<fn_v>())
        for(node_id fn: cell->as<fn_v>().set)
          if(assoc_attr(fn)) return true;
    for(auto const & r: overrides)
      if(r.entity == f && assoc_attr(r.fn)) return true;
    return false;
  }
  // The associativity of each chain operator in the context is the string “0/1” (cache key).
  std::string chain_assoc(node_id n, n_chain const & c, scope * env) {
    std::string out;
    for(symbol o: c.ops) out += op_associative(o, env, n) ? '1' : '0';
    return out;
  }
  // The chain of one operator, associative in context, is one tree without brute
  // force: a + b + c п (a + b) + c.
  bool assoc_run(node_id n, n_chain const & c, scope * env) {
    for(symbol o: c.ops)
      if(o != c.ops[0]) return false;
    return op_associative(c.ops[0], env, n);
  }
  node_id assoc_tree(node_id n, n_chain const & c) {
    auto [it, fresh] = assoc_trees.try_emplace(n.value);
    if(fresh) {
      node_id t = c.atoms[0];
      for(uint32_t k = 0; k < c.ops.size(); ++k) {
        node_id l = t;
        t = db.derive(n_op{c.ops[k], {l, c.atoms[k + 1]}}, {db[n].where.begin, db[c.atoms[k + 1]].where.end}, derived_nodes);
        db[t].parent = n;
      }
      it->second = t;
    }
    return it->second;
  }

  // Assoc is the chain_assoc of context (in a process machine, its context).
  node_id resolve_chain(node_id n, n_chain const & c, scope * env, std::string const & assoc) {
    auto & names = chain_names[n.value];
    if(names.empty()) {
      for(node_id a: c.atoms) free_names(a, names);
      if(names.empty()) names.push_back(symbol{}); // literary
    }
    std::string key = cat(n.value, ":", assoc, ":");
    bool cacheable = true;
    for(symbol s: names) {
      value const * cell = s ? find(env, s) : nullptr;
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
    // The choice of the same chain with the same types is already underway (the test performs a recursive
    // call, in the body of which the same chain; the samples are nested - different interpreters) - the
    // type is still derived, as in recursion parsing : otherwise the search is endless.
    if(!checking()) { // Execution - choice as is; protection only in parsing samples
      node_id tree = enumerate_chain(n, c, env, assoc);
      if(tree && cacheable && !chain_open) resolved_chains[key] = tree;
      return tree;
    }
    std::string guard = cat(n.value, ":", assoc, ":"); // Type Numbers of Registries - Names
    for(symbol s: names) {
      value const * cell = s ? find(env, s) : nullptr;
      guard += cell ? types.describe(*cell) + "," : std::string("-,");
    }
    if(!chains_resolving().insert(guard).second) return {};
    node_id tree;
    try {
      tree = enumerate_chain(n, c, env, assoc);
    } catch(...) {
      chains_resolving().erase(guard);
      throw;
    }
    chains_resolving().erase(guard);
    if(tree && cacheable && !chain_open) resolved_chains[key] = tree;
    return tree;
  }
  bool chain_open = false; // The last choice of the chain came with an atom (do not cache)
  // sample selection of the chain tree: the values are erased to types, so the recursion
  // according to known values will not stop - the recursion key only by types, the
  // recursion converges with a fixed point (*my*; the sample needs only types)
  bool probing = false;
  static std::set<std::string> & chains_resolving() {
    static std::set<std::string> keys;
    return keys;
  }

  interpreter & prober() {
    if(probes.empty()) {
      probes.emplace_back(db, mod, std::vector<std::string>{}, mode::check, std::nullopt, nullptr, this);
      probes.back().fuel = 200000;
      probes.back().probing = true;
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
        } else if(code_agg(v)) { // code - reference to the node, run-time -part: erased, it is not the node (code_kind(c)!= ast.int |...)
          s->declare(it->first, v);
        } else {
          value e = erase(v);
          e.proof = 0; // Facts - their own parsing
          s->declare(it->first, std::move(e));
        }
      }
    return s;
  }

  node_id enumerate_chain(node_id n, n_chain const & c, scope * env, std::string const & assoc_of) {
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
    bool open = false; // The type of atom is still derived (recursion) - : operators decide for themselves
    chain_open = false;
    for(node_id a: c.atoms) {
      flow f = p.eval(a, ps, 0);
      if(!f.normal()) return {}; // type still unknown
      if(unknown(f.v)) open = true;
      shapes.push_back(f.v);
    }
    // brute force - in language (chain_resolve prelude, M6 step 4):
// Here are the types of atoms, the sample.
    // operator over types (chain_probe, chain_key) and node of the selected tree
    if(chain_helper) fail("разбор цепочки в коде разбора цепочек (chain_resolve): там каждое выражение — один оператор в скобках", db[n].where);
    if(!chain_fn)
      for(uint32_t i = db.size(); i-- > 0;)
        if(db.is<n_fn>(node_id(i)) && db.strings.text(db.as<n_fn>(node_id(i)).name) == "chain_resolve") {
          chain_fn = node_id(i);
          break;
        }
    if(!chain_fn) fail("нет chain_resolve в прелюдии — цепочку не разобрать", db[n].where);
    chain_job job{&p, std::move(shapes), c.ops, {}};
    chain_jobs.push_back(&job);
    struct pop_job {
      std::vector<chain_job *> & jobs;
      ~pop_job() { jobs.pop_back(); }
    } pop{chain_jobs};
    std::vector<value> ops, assoc;
    for(symbol o: c.ops) ops.push_back(value(std::string(str(o))));
    for(char a: assoc_of) assoc.push_back(value(a == '1'));
    value r = in_helper(chain_fn, {value(int64_t(k)), value(array_v::of(std::move(ops))), value(array_v::of(std::move(assoc))), value(open)});
    auto field = [&](std::string_view f) -> value const & {
      for(auto const & [s, v]: r.as<agg_v>().fields)
        if(str(s) == f) return v;
      fail(cat("chain_resolve: нет поля ", f), db[n].where);
    };
    int64_t status = field("status").as<int64_t>();
    if(status == 1) return {}; // atom with and not one tree - the type is still bred
    chain_open = open; // tree with an atom is not cached (*my*)
    if(status == 2) fail(cat("ни один разбор цепочки не инстанцируется", job.why.empty() ? "" : cat(": ", job.why)), db[n].where);
    if(status == 3) {
      diagnostic d("цепочка операторов неоднозначна: подходят несколько разборов — расставьте скобки", db[n].where);
      auto const & notes = field("notes").as<array_v>();
      for(uint32_t i = 0; i < notes.size(); ++i) d.note(cat("разбор типа ", notes.get(i).as<str_v>().view()), db[n].where);
      throw d;
    }
    // only tree in node (one per chain and types: cache above); prefix: -1 atom | m left right
    auto const & tree = field("tree").as<array_v>();
    uint32_t pos = 0;
    auto build = [&](auto & self) -> node_id {
      int64_t m = tree.get(pos++).as<int64_t>();
      if(m < 0) return c.atoms[uint32_t(tree.get(pos++).as<int64_t>())];
      node_id l = self(self);
      node_id r = self(self);
      node_id t = db.derive(n_op{c.ops[uint32_t(m)], {l, r}}, {db[l].where.begin, db[r].where.end}, derived_nodes);
      db[t].parent = n;
      return t;
    };
    return build(build);
  }

  // The search of the chain is going on: types (number - index), operators, the reason for the failure of the last sample.
  struct chain_job {
    interpreter * p;
    std::vector<value> shapes;
    std::vector<symbol> ops;
    std::string why;
  };
  std::vector<chain_job *> chain_jobs;
  interpreter * chain_subject = nullptr; // Whose helper (chain_probe, chain_key)
  bool chain_helper = false;
  // the program does not touch (the code of the editor in the language, his assistants):
  // elaboration - nested, mutations of the ct program and its "active result" - as were
  bool detached = false;
  int helper_depth = 0; // Rule interpreter level (0 is not an assistant)
  node_id chain_fn;
  std::deque<interpreter> chain_langs;
  // Calling the rule in the assistant is the default memory level: the assistant lives longer than
  // the context of the with_alloc(r) program, its values should not be in pages r (§12.2).
  value in_helper(node_id fn, std::vector<value> args) {
    struct default_level {
      address_space * saved = std::exchange(current_space, nullptr);
      ~default_level() { current_space = saved; }
    } level;
    return chain_lang().analyze_instance(fn, std::move(args));
  }
  interpreter & chain_lang() {
    if(chain_langs.empty()) {
      chain_langs.emplace_back(db, db.prelude ? db.prelude : mod, std::vector<std::string>{"chain"}, mode::run);
      chain_langs.back().chain_helper = true;
      chain_langs.back().detached = detached;
      chain_langs.back().helper_depth = helper_depth + 1;
      chain_langs.back().chain_subject = this;
    }
    return chain_langs.back();
  }

  bool op_associative(symbol op) { // Module elements and prelude
    auto [it, fresh] = assoc_ops.try_emplace(op.value, false);
    if(fresh)
      if(auto f = functions.find(infix_entity(op)); f != functions.end())
        for(node_id fn: f->second)
          if(assoc_attr(fn)) it->second = true;
    return it->second;
  }
  // Chain in the code of the module with control of extensions ([[extension(g)]] module k):
  // associativity - from its definition context (he himself, prelude, his use), the
  // announcements of other modules do not change it; the context of execution (block
  // operators, using operator) - like everyone else.
// My module,
  // Having declared control, protects parsing of its code and from other people's
  // [[associative]] not their entity (operator prelude) - their check does not see.
  bool op_associative(symbol op, node_id at) {
    if(!extension_any) return op_associative(op);
    node_id m = module_of(at);
    if(!m || db.as<n_module>(m).attrs.empty() || !extension_check(db.as<n_module>(m).attrs)) return op_associative(op);
    auto [it, fresh] = definition_assoc.try_emplace((uint64_t(op.value) << 32) | m.value, false);
    if(fresh)
      if(auto f = functions.find(infix_entity(op)); f != functions.end())
        for(node_id fn: f->second)
          if(assoc_attr(fn) && definition_sees(m, module_of(fn))) it->second = true;
    return it->second;
  }

  // Type of application of infix operator to samples (or failure - SFINAE).
  std::optional<value> probe_apply(interpreter & p, symbol op, value const & l, value const & r, std::string & why) {
    std::string key = cat(str(op), "|", types.describe(l), "|", types.describe(r));
    if(auto it = applied.find(key); it != applied.end()) return it->second;
    auto & node = apply_nodes[op.value];
    if(!node) {
      node_id a = db.derive(n_ident{sym("__l")}, {}, derived_nodes);
      node_id b = db.derive(n_ident{sym("__r")}, {}, derived_nodes);
      node = db.derive(n_op{op, {a, b}}, {}, derived_nodes);
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
// The link is tied to a support - an array buffer. Meaning of verification
  // parsing is the code prelude over the ct state (hacks below): the support is the ct
  // state of the array, the link remembers it when taking, the use checks requires;
  // “dangerous-dangerous” is the parsing error with the track. When executed - VM buffer
  // generation: obsolete link - exception of invalidated_alias. Recording in other
  // elements and values of elements - "dangerous-safe", allowed.

  // Array or erased array (including all splitting paths).
  bool array_like(value const & v) const {
    if(v.is<split_v>()) return std::ranges::all_of(v.as<split_v>().alts, [&](value const & a) { return array_like(a); });
    value const * s = shape_of(v);
    return s && s->is<array_v>();
  }

  // auto&e = v[i] / place-argument of the variable parameter: reference to the element.
  std::optional<value> element_ref(node_id place_node, scope * env, uint32_t frame, node_id born) {
    if(!db.is_index(place_node) || !db.is<n_ident>(db.index_of(place_node).object)) return std::nullopt;
    auto const & i = db.index_of(place_node);
    symbol name = db.as<n_ident>(i.object).name;
    value * cell = find(env, name);
    if(!cell || cell->is<thunk_v>() || cell->is<ref_v>()) return std::nullopt;
    not_hidden(*cell, name, db[place_node].where);
    flow fi = eval(i.index, env, frame);
    if(!fi.normal()) return std::nullopt;
    value idx = fi.v;
    span where = db[place_node].where;
    ref_v r;
    r.cell = cell;
    r.born = born.value + 1;
    r.name = name;
    if(!checking()) {
      if(!cell->is<array_v>()) return std::nullopt;
      r.index = checked_index(*cell, idx, where);
      r.gen = cell->as<array_v>().gen;
      return value(r);
    }
    if(!array_like(*cell)) return std::nullopt;
    index_cells[place_node.value].insert(cell); // link array cell - for jit_ref_keeps
    if(cell->is<array_v>() && idx.known()) r.index = checked_index(*cell, idx, where);
    else {
      r.rt_index = true;
      bool proven_here = index_holds(i, idx, env);
      note_index(place_node, proven_here);
      if(!proven_here) rt_check("out_of_bounds", where);
    }
    r.memo = hook_ref_taken(cell, name, born, where, frame);
    return value(r);
  }

  flow declare_ref(node_id n, n_var const & v, scope * env, uint32_t frame) {
    if(db.is<n_ident>(v.init)) { // auto&a = b is the second name of the same cell.
      value * cell = find(env, db.as<n_ident>(v.init).name);
      if(!cell || cell->is<thunk_v>()) fail("auto& — ссылка на переменную, поле или элемент массива", db[v.init].where);
      not_hidden(*cell, db.as<n_ident>(v.init).name, db[v.init].where);
      env->alias(v.name, cell);
      return ok();
    }
    auto r = element_ref(v.init, env, frame, n);
    if(!r) r = path_ref(v.init, env, frame, n);
    if(!r) fail("auto& — ссылка на переменную, поле или элемент массива", db[v.init].where);
    env->declare(v.name, std::move(*r));
    return ok();
  }

  // --- link-path (§12.1, tail of step 10)
  // ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// (§12.1, D13). root
  // - Variable cell, steps - field and element (buffer generation when taking); the place is searched anew with
  // each call - to hang nothing, like the field in the variable parameter (§6.2): the field of the replaced
  // aggregate - the field of the new, the element of the moved buffer - "dangerous-dangerous" invalidated_alias
  // . * My: * up to three steps (without a heap: steps - in the link itself); parsing follows hooks for the
  // support-variable first step, supports inside (b.xs, g[1]) - generation of execution.
  static ref_step step_of(ref_v const & r, uint32_t k) { return k == 0 ? ref_step{r.index, r.gen, r.field, r.rt_index} : r.rest[k - 1]; }
  static void set_step(ref_v & r, uint32_t k, ref_step const & s) {
    if(k == 0) {
      r.index = s.index;
      r.gen = s.gen;
      r.field = s.field;
      r.rt_index = s.rt_index;
    } else {
      r.rest[k - 1] = s;
      r.more = uint8_t(k);
    }
  }
  std::optional<value> path_ref(node_id place_node, scope * env, uint32_t frame, node_id born) {
    std::vector<node_id> chain;
    node_id x = place_node;
    while(db.is_index(x) || db.is_member(x)) {
      chain.push_back(x);
      x = db.is_index(x) ? db.index_of(x).object : db.member_of(x).object;
    }
    if(!db.is<n_ident>(x) || chain.empty()) return std::nullopt;
    symbol root_name = db.as<n_ident>(x).name;
    value * cell = find(env, root_name);
    if(!cell || cell->is<thunk_v>()) return std::nullopt;
    not_hidden(*cell, root_name, db[place_node].where);
    span where = db[place_node].where;
    ref_v r;
    uint32_t n = 0;
    if(cell->is<ref_v>()) { // link from the link: its path, then its steps
      r = cell->as<ref_v>();
      n = 1 + r.more;
    } else {
      r.cell = cell;
      r.name = root_name;
    }
    r.born = born.value + 1;
    for(auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if(n == 3) fail("ссылка — не глубже трёх шагов (поле, элемент)", where);
      ref_step st;
      if(db.is_member(*it)) st.field = db.member_of(*it).name;
      else {
        flow fi = eval(db.index_of(*it).index, env, frame);
        if(!fi.normal()) return std::nullopt;
        value box = n ? path_value(r, n, where) : *r.cell; // stride
        if(!checking()) {
          if(!box.is<array_v>()) fail(cat("ссылка на элемент: ", types.describe(box), " — не массив"), where);
          st.index = checked_index(box, fi.v, where);
          st.gen = box.as<array_v>().gen;
        } else if(box.is<array_v>() && fi.v.known()) st.index = checked_index(box, fi.v, where);
        else {
          st.rt_index = true;
          rt_check("out_of_bounds", where);
        }
      }
      set_step(r, n++, st);
      if(!checking() && st.field) path_place(r, n, where); // The field is - immediately (parsing - in value below)
    }
    if(checking()) {
      value probe = path_value(r, n, where); // Type of place - field error here, not when reading
      (void)probe;
      if(r.cell == cell && !r.field && !cell->is<ref_v>() && array_like(*cell)) r.memo = hook_ref_taken(cell, root_name, born, where, frame);
    }
    return value(r);
  }
  // Place cell after the first n steps (execution): the element is a cell of the common array; the
  // buffer is changed - invalidated_alias. soft - parsing: no cell (run-time - value) - empty.
  value * path_place(ref_v const & r, uint32_t n, span where, bool soft = false) {
    value * c = r.cell;
    for(uint32_t k = 0; k < n; ++k) {
      ref_step st = step_of(r, k);
      if(is_shared(*c)) c = &slot_of(*c).v; // shareable
      if(st.field) {
        value * f = nullptr;
        if(c->is<agg_v>())
          for(auto & [name, v]: c->as<agg_v>().fields)
            if(name == st.field) f = &v;
        if(!f) {
          if(soft) return nullptr;
          fail(cat("у значения типа ", types.describe(*c), " нет поля ", str(st.field)), where);
        }
        c = f;
        continue;
      }
      if(soft && (st.rt_index || !c->is<array_v>())) return nullptr;
      if(!checking() && (!c->is<array_v>() || c->as<array_v>().gen != st.gen))
        fail_rt("invalidated_alias", "ссылка на элемент использована после инвалидации опоры", where);
      uint32_t i = checked_index(*c, value(st.index), where);
      value * e = c->as<array_v>().generic_cell(i);
      if(!e) {
        if(soft) return nullptr;
        fail(cat("элемент массива ", types.describe(*c), " — значение, а не место: дальше пути нет"), where);
      }
      c = e;
    }
    return c;
  }
  // The value after the first n steps is by value (parsing: run-time -index - element type, field
  // run-time -aggregate - by sample).
  value path_value(ref_v const & r, uint32_t n, span where) {
    value cur = *r.cell;
    for(uint32_t k = 0; k < n; ++k) {
      ref_step st = step_of(r, k);
      if(st.field) {
        symbol f = st.field;
        cur = lift(cur, [&](value const & o) { return member(o, f, where); });
      } else if(!st.rt_index && cur.is<array_v>()) {
        if(!checking() && cur.as<array_v>().gen != st.gen) fail_rt("invalidated_alias", "ссылка на элемент использована после инвалидации опоры", where);
        value e = cur.as<array_v>().get(checked_index(cur, value(st.index), where));
        cur = std::move(e);
      } else cur = lift(cur, [&](value const & a) { return element_type(a, where); });
    }
    return cur;
  }
  value path_deref(ref_v const & r, span where) {
    if(checking()) {
      hook_ref_used(r, where);
      return path_value(r, 1 + r.more, where);
    }
    uint32_t n = 1 + r.more;
    ref_step last = step_of(r, n - 1);
    if(last.field) return *path_place(r, n, where);
    value * box = path_place(r, n - 1, where);
    if(!box->is<array_v>() || box->as<array_v>().gen != last.gen) fail_rt("invalidated_alias", "ссылка на элемент использована после инвалидации опоры", where);
    return box->as<array_v>().get(checked_index(*box, value(last.index), where));
  }
  flow path_assign(ref_v const & r, std::string_view op, value const & rhs, span where) {
    value cur = path_deref(r, where);
    value res = op == "=" ? rhs : binary(op.substr(0, 1), cur, rhs, where);
    if(op == "=" && is_record(res)) res = to_type(std::move(res), place_type(cur), where); // Struct holds it, {x: …} becomes it.
    if(!unknown(cur) && !unknown(res) && types.describe(erase(cur)) != types.describe(erase(res)))
      fail(cat("место ссылки типа ", types.describe(cur), " нельзя заменить значением типа ", types.describe(res)), where);
    uint32_t n = 1 + r.more;
    ref_step last = step_of(r, n - 1);
    if(last.field) {
      if(value * p = path_place(r, n, where, checking())) {
        if(op == "=" && res_any() && has_resource(*p)) drop_value(*p, where); // Overwrite - the old resource is released
        *p = std::move(res);
      } else *r.cell = erase(*r.cell); // parsing: the place behind run-time-step is the value of the variable run-time
      return ok();
    }
    value * box = path_place(r, n - 1, where, checking());
    if(box && box->is<array_v>() && !last.rt_index && (!checking() || (res.known() && cur.known()))) {
      box->as<array_v>().set(checked_index(*box, value(last.index), where), std::move(res));
      return ok();
    }
    if(box) *box = erase(*box); // element on run-time -index changed - array run-time
    else *r.cell = erase(*r.cell);
    return ok();
  }

  void ref_valid(ref_v const & r, span where) {
    if(checking()) return hook_ref_used(r, where);
    if(!r.cell->is<array_v>() || r.cell->as<array_v>().gen != r.gen)
      fail_rt("invalidated_alias", "ссылка на элемент использована после инвалидации опоры", where);
  }

  value deref(ref_v const & r, span where) {
    if(r.path()) return path_deref(r, where);
    ref_valid(r, where);
    value const & a = *r.cell;
    if(!r.rt_index && a.is<array_v>()) return a.as<array_v>().get(checked_index(a, value(r.index), where));
    return lift(a, [&](value const & x) { return element_type(x, where); });
  }

  flow ref_assign(ref_v const & r, std::string_view op, value const & rhs, span where) {
    if(r.path()) return path_assign(r, op, rhs, where);
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

  // pin_storage(xs) - pin core: array-variable data - in its range of addresses, their growth no
  // longer moves (pages.hpp); the support pointer model puts prelude.
  flow pin_storage(node_list const & args, scope * env, node_id n) {
    span where = db[n].where;
    if(args.size() != 1 || !db.is<n_ident>(args[0])) fail("pin_storage(массив)", where);
    value * cell = find(env, db.as<n_ident>(args[0]).name);
    if(!cell || cell->is<thunk_v>() || cell->is<ref_v>()) fail("pin: аргумент — переменная-массив", where);
    if(checking()) return ok();
    if(!cell->is<array_v>()) fail(cat("pin: ожидался массив, получено ", types.describe(*cell)), where);
    cell->as<array_v>().pin();
    return ok();
  }

  // append(xs, x) is the growth of the array, the core push. What growth means for links, the library
  // declares: [[relocates(xs)]] at push and the pivot pointer model (the parsing hook on the language).
  flow append(node_list const & args, scope * env, uint32_t frame, node_id n) {
    span where = db[n].where;
    if(args.size() != 2 || !db.is<n_ident>(args[0])) fail("append(массив, значение)", where);
    value * cell = find(env, db.as<n_ident>(args[0]).name);
    if(!cell || cell->is<thunk_v>() || cell->is<ref_v>()) fail("append: первый аргумент — переменная-массив", where);
    not_hidden(*cell, db.as<n_ident>(args[0]).name, where);
    TZ_TRY(x0, eval(args[1], env, frame));
    TZ_TAKE(x, args[1], x0, env, frame); // resource in an array - transfer or copy, as in the assignment
    if(!checking()) {
      if(!cell->is<array_v>()) fail(cat("append: ожидался массив, получено ", types.describe(*cell)), where);
      array_v & a = cell->as<array_v>();
      void const * was = a.size() ? a.base() : nullptr;
      a.push(x);
      // 
// a generation, only if the data has moved
      if(was && a.base() != was) a.gen = ++array_gen;
      return ok();
    }
    book.written(cell, true);
    on_alternatives(cell, [&] {
      value const * shape = shape_of(*cell);
      if(!shape || !shape->is<array_v>()) fail(cat("append: ожидался массив, получено ", types.describe(*cell)), where);
      if(!shape->as<array_v>().empty() && !unknown(x) && types.describe(erase(shape->as<array_v>().get(0))) != types.describe(erase(x)))
        fail(cat("append: элемент типа ", types.describe(x), " в массив ", types.describe(*cell)), where);
      if(cell->is<array_v>() && x.known()) cell->as<array_v>().push(x);
      else {
        uint32_t keep = cell->ct; // ct-state of place - same
        *cell = erase(value(array_v::of({shape->as<array_v>().empty() ? x : shape->as<array_v>().get(0)})));
        cell->ct = keep;
      }
    });
    return ok();
  }

  // --- ct state and hooks parsing (§5.6, §12.1) ---
  //
  // 
// 
// 
// 
//.
  // ct-state - at the value (value::ct): ct_state(x, иначе) reads, set_ct_state(x, s) changes
  // place; different paths - splitting (§5.5), error - on the way. Hookie (my): the kernel in
  // parsing calls prelude functions, if they are declared - the meaning of the check is written in
  // the language; they are not in the executable code.

  uint32_t intern_ct(value const & v) { return ct_states.intern(v, cat(types.describe(v), "=", show_v(v))); }

  // Location of value in the program: the nearest call outside prelude
//.
  // The [[g(…)]] hook is not the place of the program: the place is the challenge it tests.
  node_id program_site() const {
    for(auto it = stack.rbegin(); it != stack.rend(); ++it)
      if(it->node && !db.in_prelude(node_id(it->node)) && !hook_call(node_id(it->node))) return node_id(it->node);
    return {};
  }
  span program_where(node_id n) const {
    if(!db.in_prelude(n)) return db[n].where;
    for(auto it = stack.rbegin(); it != stack.rend(); ++it)
      if((!db.prelude || it->where.source != db[db.prelude].where.source) && !(it->node && hook_call(node_id(it->node)))) return it->where;
    return db[n].where;
  }
  bool hook_call(node_id n) const {
    node_id p = db[n].parent;
    return p && db.is<n_fn>(p) && std::ranges::find(db.as<n_fn>(p).attrs, n) != db.as<n_fn>(p).attrs.end();
  }

  // set_ct_state(x, s): The ct-state of the place x is the compile value.
  flow set_ct_state(node_list const & args, scope * env, uint32_t frame, span where) {
    if(args.size() != 2 || !db.is<n_ident>(args[0])) fail("set_ct_state(переменная, состояние)", where);
    value * cell = find(env, db.as<n_ident>(args[0]).name);
    if(!cell || cell->is<thunk_v>() || cell->is<ref_v>()) fail("set_ct_state: первый аргумент — переменная", where);
    TZ_TRY(s, eval(args[1], env, frame));
    if(!s.known()) fail(cat("ct-состояние — значение при компиляции, получено рт ", types.describe(s)), where);
    uint32_t id = intern_ct(s);
    if(checking()) book.written(cell, false); // Branches and cycles see the previous state
    if(cell->is<split_v>())
      for(auto & a: cell->as<split_v>().alts) a.ct = id;
    else cell->ct = id;
    return ok();
  }

  // Code above place by alternatives: the cell is split - the body sees each
  // alternative separately, the result is again alternatives (§5.5).
  void on_alternatives(value * cell, auto && body) {
    if(!cell->is<split_v>()) return body();
    std::vector<value> alts = cell->as<split_v>().alts;
    value out;
    for(std::size_t k = 0; k < alts.size(); ++k) {
      *cell = alts[k];
      body();
      out = k ? merge(out, *cell) : *cell;
    }
    *cell = std::move(out);
  }

  // Calling the prelude function from the kernel: arguments - places (alias) and values.
  flow call_hook(symbol fn, std::vector<std::pair<symbol, value *>> const & places, std::vector<std::pair<symbol, value>> const & vals, span where, uint32_t node, uint32_t frame) {
    scope * s = new_scope(globals);
    scope_hold hold{*this, s};
    node_list args;
    auto ident = [&](symbol n) {
      auto & id = hook_idents[n.value];
      if(!id) id = db.derive(n_ident{n}, {}, derived_nodes);
      args.push_back(id);
    };
    for(auto const & [n, c]: places) {
      s->alias(n, c);
      ident(n);
    }
    for(auto const & [n, v]: vals) {
      s->declare(n, v);
      ident(n);
    }
    hook_hold hh{*this};
    return call_named(fn, args, s, frame, where, node);
  }
  struct hook_hold {
    interpreter & vm;
    hook_hold(interpreter & vm) : vm(vm) { ++vm.hook_depth; }
    ~hook_hold() { --vm.hook_depth; }
  };

  // Taking a link to an item: ref_taken(опора) prelude, the result remembers the link.
  uint32_t hook_ref_taken(value * cell, symbol name, node_id born, span where, uint32_t frame) {
    symbol h = sym("ref_taken");
    if(!functions.contains(h)) return 0;
    if(cell->is<split_v>()) { // The state of support is different in ways: the new link begins with a clean
      book.written(cell, false);
      std::vector<value> alts = cell->as<split_v>().alts;
      value out;
      for(std::size_t k = 0; k < alts.size(); ++k) {
        alts[k].ct = 0;
        out = k ? merge(out, alts[k]) : alts[k];
      }
      *cell = std::move(out);
    }
    value memo;
    bool first = true;
    on_alternatives(cell, [&] {
      flow f = call_hook(h, {{name, cell}}, {}, where, born.value, frame);
      memo = first ? f.v : merge(memo, f.v);
      first = false;
    });
    if(!memo.known()) fail(cat("ref_taken: ссылка запоминает значение при компиляции, получено ", types.describe(memo)), where);
    return intern_ct(memo);
  }

  // Use of reference: ref_used(опора, что запомнила) prelude.
  void hook_ref_used(ref_v const & r, span where) {
    symbol h = sym("ref_used");
    if(!r.memo || !functions.contains(h)) return;
    call_hook(h, {{r.name, r.cell}}, {{sym("ссылка"), ct_states.at(r.memo)}}, where, 0, 0);
  }

  // [[g(…)]] for a function where g is a function of the language: ct-hook of the call - for parsing
  // each call is executed with the parameters of the function associated with arguments (place -
  // alias). [[relocates(xs)]] for push is the code prelude, not the rule of the compiler. The
  // argument is split - the call goes on each alternative (dispatch_split), the hook changes it.
  void attribute_hooks(node_id f, std::vector<value> const & vals, node_list const & args, scope * env, call_site const & site) {
    if(!checking()) return;
    auto const & fn = db.as<n_fn>(f);
    for(node_id a: fn.attrs) {
      if(!db.is<n_call>(a) || !db.is<n_ident>(db.as<n_call>(a).callee) || !functions.contains(db.as<n_ident>(db.as<n_call>(a).callee).name)) continue;
      scope * s = new_scope(globals);
      scope_hold hold{*this, s};
      struct alt_place {
        value * cell;
        value saved;
        std::size_t k;
      };
      std::vector<alt_place> alts;
      for(uint32_t k = 0; k < fn.params.size() && k < vals.size(); ++k) {
        symbol p = db.as<n_param>(fn.params[k]).name;
        value * cell = k < args.size() ? place_or_null(args[k], env) : nullptr;
        if(cell && cell->is<ref_v>()) cell = cell->as<ref_v>().cell;
        if(!cell) {
          s->declare(p, vals[k]);
          continue;
        }
        if(cell->is<split_v>() && !vals[k].is<split_v>()) { // This alternative is for hook time.
          auto const & as = cell->as<split_v>().alts;
          for(std::size_t j = 0; j < as.size(); ++j)
            if(same_alt(as[j], vals[k])) {
              book.written(cell, false);
              alts.push_back({cell, *cell, j});
              *cell = as[j];
              break;
            }
        }
        s->alias(p, cell);
      }
      stack.push_back(site); // place of call - in the Trace and for call_site()
      hook_hold hh{*this};
      try {
        eval(a, s, site.frame);
      } catch(diagnostic & d) { // The hook error only sees parsing: it goes before launch
        stack.pop_back();
        for(auto & x: alts) *x.cell = std::move(x.saved);
        d.hook = true;
        throw;
      } catch(...) {
        stack.pop_back();
        for(auto & x: alts) *x.cell = std::move(x.saved);
        throw;
      }
      stack.pop_back();
      for(auto it = alts.rbegin(); it != alts.rend(); ++it) {
        value now = std::move(*it->cell);
        *it->cell = std::move(it->saved);
        it->cell->as<split_v>().alts[it->k] = std::move(now);
      }
    }
  }

  bool same_alt(value const & a, value const & b) const {
    if(state_of(a) != state_of(b) || types.describe(a) != types.describe(b)) return false;
    return !a.known() || !b.known() || show_v(a) == show_v(b);
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
    std::swap(current_space, e.space);
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
    if(on_bg_lane) {
      std::lock_guard g(vm_lock);
      ++root->lane_live;
    }
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
      ruled_hold rh{*this, t, ruled_env(t)};
      flow r;
      try {
        r = eval(t.expr, rh.s, t.frame);
      } catch(...) {
        ctx = saved;
        throw;
      }
      ctx = saved;
      (void)r;
      return ok();
    }
    scope * body_env = ruled_env(t); // Rule layer lives with fiber
    capture(body_env);
    fiber_rec & f = new_fiber();
    f.body = t.expr;
    f.env = body_env;
    f.frame = t.frame;
    f.body_ctx = t.ctx;
    return ok();
  }

  // on_pressure { … } (step 12.3): The unit is the process pressure handler; the VM triggers the
  // process fiber (pressure). parsing is a body in place like a spawn.
  struct pressure_rec {
    node_id body;
    scope * env;
    uint32_t frame;
    uint64_t ctx;
  };
  std::vector<pressure_rec> pressure_handlers;
  int psi_fd = -1; // at the root: memory pressure trigger host
  flow pressure_handler(node_list const & args, scope * env, uint32_t frame, span where) {
    if(checking()) return spawn(args, env, frame, where);
    if(args.size() != 1) fail("on_pressure { … }", where);
    thunk_v t = thunk_arg(args[0], env, where);
    scope * body_env = ruled_env(t);
    capture(body_env);
    pressure_handlers.push_back({t.expr, body_env, t.frame, t.ctx});
    arm_pressure();
    return ok();
  }
  // PSI trigger - with the first handler; trigger - completion in the ring (op_psi)
  void arm_pressure() {
    interpreter & r = *root;
    if(r.psi_fd == -1) {
      if(!r.ring) r.ring.emplace();
      r.psi_fd = r.ring->ok ? os::memory_pressure_fd() : -1;
      if(r.psi_fd < 0) r.psi_fd = -2; // There is no PSI (or ring) in host - only an external command
    }
    if(r.psi_fd >= 0 && !r.psi_armed) {
      r.psi_armed = true;
      io().poll(r.psi_fd, POLLPRI, io_tag(op_psi, 0, 0)); // No application: io_pending is not growing
    }
  }
  bool psi_armed = false;
  // Pressure: Background handlers first ("the rest of the trash"), then programs (*my*: order)
  void run_pressure(interpreter & vm) {
    for(auto const & h: vm.pressure_handlers) {
      fiber_rec & f = vm.new_fiber();
      f.body = h.body;
      f.env = h.env;
      f.frame = h.frame;
      f.body_ctx = h.ctx;
    }
  }
  void pressure() {
    interpreter & r = *root;
    if(r.bg) { // In the background, in his lane: asks her
      {
        std::lock_guard g(vm_lock);
        r.bg->lane_pressure = true;
      }
      poke(*r.bg);
    } else
      for(auto & p: r.procs)
        if(p.background) run_pressure(p);
    run_pressure(r);
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
    {
      lane_guard g(vm_lock);
      if(on_bg_lane) --vm.root->lane_live;
      for(fiber_rec * w: std::exchange(vm.vm_root->joiners, {})) vm.wake(w);
    }
    vm.schedule(); // They don't come back
  }

  void wake(fiber_rec * f) {
    if(lanes_on.load(std::memory_order_relaxed) && f->vm->root != root) return post(f); // fiber of the other band
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
    if(lanes_on.load(std::memory_order_relaxed) && f->vm->root != root) return post(f);
    if(f->state != fiber_rec::blocked) return;
    f->state = fiber_rec::ready;
    if(root->handoff_run >= handoff_limit) return root->ready.push_back(f);
    f->direct = true;
    root->ready.push_front(f);
  }

  // ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
  // The state of the filer changes only its bandwidth (drain inbox).
  //
  std::vector<fiber_rec *> inbox;
  int wake_fd = -1; // eventfd bands: wakes her waiting in the ring
  bool wake_armed = false;
  uint64_t wake_buf = 0;
  bool lane_idle = false; // background sleeps without applications and incoming (under vm_lock)
  bool lane_stop = false; // The main request is to stop (under vm_lock).
  bool lane_cancel = false; // ... and promote fiber (program error)
  bool lane_pressure = false; // pressure: background handlers - on their lane
  uint32_t lane_live = 0; // live fiber background band (under vm_lock)
  interpreter * bg = nullptr; // primary root: background band
  std::list<interpreter> lane_vms; // its root (list: type not yet complete)
  pthread_t lane_thread{};
  bool lane_running = false;

  void post(fiber_rec * f) {
    interpreter & L = *f->vm->root;
    {
      std::lock_guard g(vm_lock);
      L.inbox.push_back(f);
    }
    poke(L);
  }
  static void poke(interpreter & L) {
    uint64_t one = 1;
    if(L.wake_fd >= 0) (void)!::write(L.wake_fd, &one, sizeof one);
  }
  void drain_inbox() {
    interpreter & r = *root;
    std::vector<fiber_rec *> in;
    bool press = false, stop = false;
    {
      std::lock_guard g(vm_lock);
      in.swap(r.inbox);
      press = std::exchange(r.lane_pressure, false);
      stop = r.lane_stop && (r.lane_live == 0 || r.lane_cancel);
    }
    for(fiber_rec * f: in)
      if(f->state == fiber_rec::blocked) {
        f->state = fiber_rec::ready;
        r.ready.push_back(f);
      }
    if(press) // pressure (from the main): background process handlers - fiber of this band
      for(auto & p: vm_root->procs) run_pressure(p);
    if(stop && !r.fibers.empty() && r.fibers[0].state == fiber_rec::blocked) { // main fiber band - to the exit
      r.fibers[0].state = fiber_rec::ready;
      r.ready.push_back(&r.fibers[0]);
    }
  }
  void arm_wake() {
    interpreter & r = *root;
    if(r.wake_armed) return;
    r.wake_armed = true;
    io().read(r.wake_fd, reinterpret_cast<char *>(&r.wake_buf), sizeof r.wake_buf, ~uint64_t{0}, io_tag(op_wake, 0, 0)); // program
  }
  // Background falls asleep: releases the code (the writer does not wait), without applications and
  // incoming - “simple”; the main one recognizes the mutual blocking of both bands (push - to check)
  void lane_sleep(bool asleep) {
    interpreter & r = *root;
    if(asleep) {
      bool idle;
      {
        std::lock_guard g(vm_lock);
        idle = r.lane_idle = r.io_pending == 0 && r.inbox.empty();
      }
      code_lock.unlock_shared();
      if(idle) poke(*vm_root);
      return;
    }
    code_lock.lock_shared();
    std::lock_guard g(vm_lock);
    r.lane_idle = false;
  }
  // Main: waiting for someone - the background sleeps without applications, no one enters
  bool lane_deadlock() {
    if(on_bg_lane) return false;
    interpreter & r = *root;
    std::lock_guard g(vm_lock);
    return r.bg && r.bg->lane_idle && r.bg->inbox.empty() && r.inbox.empty();
  }
  // Background band - according to the first process(…): its root-planner and host flow on
  // energy-efficient cores; the main - on productive cores
// 
  interpreter & bg_lane() {
    interpreter & v = *vm_root;
    if(v.bg) return *v.bg;
    db.prepare_lanes(); // base caches by character numbers - capacity in advance
    v.wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    interpreter & b = v.lane_vms.emplace_back(db, mod, std::vector<std::string>{"process"}, mode::run, std::nullopt, nullptr, nullptr, &v);
    b.wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    b.slice_points = v.slice_points;
    b.captured = v.captured;
    v.bg = &b;
    if(!v.ring) v.ring.emplace();
    lanes_on.store(true, std::memory_order_release);
    os::pin_to(os::core_kind::performance);
    b.lane_thread = os::start_thread(std::size_t(256) << 20, [](void * p) { static_cast<interpreter *>(p)->lane_main(); }, &b);
    b.lane_running = true;
    return b;
  }
  // Background Band Flow: The main fiber band gives way to the core of its fiber until the main one
  // asks to stop (and all of its fiber runs out - or spin them)
  void lane_main() {
    on_bg_lane = true;
    os::pin_to(os::core_kind::efficiency);
    ring.emplace();
    running(); // main fiber band - flow stack
    code_lock.lock_shared();
    for(;;) {
      {
        std::lock_guard g(vm_lock);
        if(lane_stop && (lane_live == 0 || lane_cancel)) break;
      }
      fibers[0].state = fiber_rec::blocked;
      schedule();
    }
    if(lane_cancel) // Program error: fiber processes are unwinded like the main
      for(auto & p: vm_root->procs)
        for(auto & f: p.fibers) {
          if(f.state == fiber_rec::done || !f.stack) continue;
          f.cancel = true;
          f.state = fiber_rec::ready;
          ready.push_front(&f);
          fibers[0].state = fiber_rec::ready;
          ready.push_back(&fibers[0]);
          schedule();
        }
    code_lock.unlock_shared();
  }
  // End of the program: background band - stop and wait (its fiber ended or unwind); after - no
  // stripes
  void stop_lanes(bool cancel) {
    interpreter & v = *vm_root;
    if(!v.bg || !v.bg->lane_running) return;
    interpreter & b = *v.bg;
    {
      std::lock_guard g(vm_lock);
      b.lane_stop = true;
      b.lane_cancel = cancel;
    }
    poke(b);
    pthread_join(b.lane_thread, nullptr);
    b.lane_running = false;
  }

  // Give the kernel to the next finished fiber. "" There are no ready ones, the current
  // one is locked, no one will wake it up: mutual lock.
  void schedule() {
    interpreter & r = *root;
    fiber_rec & me = running();
    auto drop_stale = [&] {
      while(!r.ready.empty() && r.ready.front()->state != fiber_rec::ready) r.ready.pop_front();
    };
    bool lanes = lanes_on.load(std::memory_order_acquire);
    if(lanes) drain_inbox();
    drop_stale();
    while(r.ready.empty() && me.state != fiber_rec::ready && (r.io_pending > 0 || lanes)) { // All wait for host (or other band): sleep in io_uring
      if(lanes && r.io_pending == 0 && lane_deadlock()) break;
      if(lanes) arm_wake();
      if(on_bg_lane) lane_sleep(true);
      auto cs = r.ring->wait();
      if(on_bg_lane) lane_sleep(false);
      for(auto const & c: cs) io_complete(c);
      if(lanes) drain_inbox();
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
    if(next.vm->background) { // quantum - from obtaining the nucleus
      next.vm->preempt_left = r.slice_points ? r.slice_points : 1024;
      if(!r.slice_points) r.slice_start = os::uring::now_ms();
    }
    void const * bottom = next.stack ? next.stack->base : r.host_bottom;
    std::size_t size = next.stack ? next.stack->size : r.host_size;
    char const * floor_saved = os::stack_floor;
    os::stack_floor = next.stack ? static_cast<char const *>(next.stack->base) + 4096 : os::thread_floor(); // The bottom of the stack is from the one who fulfills
    switch_to(&me.rsp, next.rsp, bottom, size);
    os::stack_floor = floor_saved; // returned to me
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

  // Expulsion (§9.5, step 12.2):
// 
// Not a signal, but a departure from
  // safe point (reverse edge of the cycle, call): fiber, which held the nucleus longer than a
  // quantum, goes to the end of the queue if there are ready-made ones. Only background processes:
  // monopoly is never superseded (author's decision 2026-10-19), background - without JIT:
// *Mine: * Quantum 10 ms, time
  // is seen once in 1024 safe points; TZ_SLICE = N is a quantum at N points (deterministically).
  static constexpr uint64_t slice_ms = 10;
  bool background = false; // background VM process: being displaced
  uint32_t slice_points = 0; // at the root: quantum at safe points (0 in time)
  uint64_t slice_start = 0; // at the root: when the current fiber received the kernel
  uint64_t preemptions = 0; // at the root: how many times the background is displaced
  uint32_t preempt_left = 1; // The first safe point puts the quantum
  // edge - reverse edge; call - point only for the quantum in time (TZ_SLICE considers iterations
  // of cycles: the same for the interpreter and the machine)
  [[gnu::always_inline]] void safe_point(bool edge) {
    if(!background) return;
    if(code_writers.load(std::memory_order_relaxed) && on_bg_lane) code_pause(); // The main one is writing code - stand.
    if((edge || !root->slice_points) && --preempt_left == 0) preempt_point();
  }
  [[gnu::noinline]] void preempt_point() {
    interpreter & r = *root;
    preempt_left = r.slice_points ? r.slice_points : 1024;
    if(lanes_on.load(std::memory_order_relaxed) && on_bg_lane) drain_inbox(); // could have come from the main
    if(r.ready.empty()) return;
    if(!r.slice_points && os::uring::now_ms() - r.slice_start < slice_ms) return;
    ++r.preemptions;
    yield();
    if(running().cancel) throw fiber_cancel{}; // end of the program with an error: repressed - to promote
  }

  void each_fiber(auto && f) {
    for(auto & x: root->fibers) f(x);
    for(auto & p: vm_root->procs)
      for(auto & x: p.fibers) f(x);
  }

  // fiber, left to wait (the program ended in error), are unwinded: their areas and
  // values are released as in the usual output.
  void cancel_fibers() {
    if(root != this || root->fibers.empty() || root->running_fiber != &root->fibers[0]) return stop_lanes(true);
    auto cancel = [&](fiber_rec & f) {
      if(f.state == fiber_rec::done || !f.stack) return;
      f.cancel = true;
      f.state = fiber_rec::ready;
      ready.push_front(&f);
      fibers[0].state = fiber_rec::ready;
      ready.push_back(&fibers[0]);
      schedule();
    };
    if(lanes_on.load(std::memory_order_acquire)) // fiber background spins their band
      for(auto & f: fibers) cancel(f);
    else each_fiber(cancel);
    stop_lanes(true);
  }

  // The end of the main: VM awaits fiber and processes; the program's fiber error is its error
  // 
//.
  void join_all() {
    if(root != this || (fibers.size() <= 1 && !bg)) return;
    for(;;) {
      bool alive = false;
      auto see = [&](fiber_rec & f) {
        if(&f != &fibers[0] && f.state != fiber_rec::done) alive = true;
      };
      if(lanes_on.load(std::memory_order_acquire)) // processes in the background: their fiber she considers
        for(auto & f: fibers) see(f);
      else each_fiber(see);
      {
        lane_guard g(vm_lock);
        if(bg && bg->lane_live > 0) alive = true;
        if(alive) joiners.push_back(&fibers[0]);
      }
      if(!alive) break;
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
    // background process - in the background band (step 12.3b); TZ_LANES = 0 - in the same (without
    // threads). Elaboration is the root of VM (adopt_elaboration): the process does not change the
    // base, it can be started from the background process.
    static bool const lanes_wanted = !std::getenv("TZ_LANES") || std::string_view(std::getenv("TZ_LANES")) != "0";
    interpreter * lane = lanes_wanted ? (on_bg_lane ? root : &bg_lane()) : root;
    interpreter * childp;
    uint32_t pid;
    {
      lane_guard g(vm_lock); // process list - common to bands
      childp = &vm_root->procs.emplace_back(db, mod, std::vector<std::string>{"process"}, mode::run, std::nullopt, nullptr, lane);
      childp->jit = nullptr; // background: without machine code - it is replaced at secure points of the interpreter
      childp->background = true;
      childp->captured = captured;
      childp->adopt_elaboration(*vm_root);
      auto & meta = vm_root->proc_meta.emplace_back();
      meta.vm = childp;
      pid = uint32_t(vm_root->proc_meta.size() - 1);
    }
    interpreter & child = *childp;
    if(lane == root || on_bg_lane) { // fiber is in her turn.
      fiber_rec & f = child.new_fiber();
      f.entry = fname;
      f.entry_args = std::move(vals);
      f.proc = pid;
    } else { // fiber is the background band: it makes it ready (incoming)
      fiber_rec & f = child.fibers.emplace_back();
      f.id = uint32_t(child.fibers.size() - 1);
      f.vm = &child;
      f.stack.emplace(std::size_t(64) << 20);
      f.rsp = f.stack->start(&fiber_main, &f);
      f.state = fiber_rec::blocked;
      f.entry = fname;
      f.entry_args = std::move(vals);
      f.proc = pid;
      {
        std::lock_guard g(vm_lock);
        ++lane->lane_live;
      }
      post(&f);
    }
    return ok(value(agg_v{types.named("process"), {{sym("id"), value(int64_t(pid))}}}));
  }

  void process_main(fiber_rec & f) {
    adopt_elaboration(*vm_root);
    auto it = functions.find(f.entry);
    call_site site{{}, std::string(str(f.entry)), 0, 0};
    flow r = dispatch_split(it->second, f.entry_args, 0, {}, globals, 0, site);
    if(r.kind == flow_kind::thr) uncaught(r);
  }

  void process_done(fiber_rec & f) {
    lane_guard g(vm_lock); // finale
    auto & m = vm_root->proc_meta[f.proc];
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
    if(!id.is<int64_t>() || uint64_t(id.as<int64_t>()) >= vm_root->proc_meta.size()) fail("неизвестный процесс", where);
    return vm_root->proc_meta[std::size_t(id.as<int64_t>())];
  }

  value process_op(std::string_view n, std::vector<value> & a, span where) {
    if(checking()) {
      rt_effect();
      return n == "process_wait" ? erase(value(true)) : erase(value(std::string("x")));
    }
    for(;;) {
      {
        lane_guard g(vm_lock); // process
        auto & m = proc_of(a[0], where);
        if(m.done) return n == "process_wait" ? value(!m.failed) : value(m.error);
        m.waiters.push_back(&running());
      }
      park();
    }
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
    op_fwrite,
    op_psi,
    op_wake };
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
    auto & pr = vm_root->streams[sid].peers[p];
    if(pr.sending || pr.out.empty() || pr.fd < 0) return;
    pr.sending = true;
    ++root->io_pending;
    io().send(pr.fd, pr.out.front().data() + pr.sent, pr.out.front().size() - pr.sent, io_tag(op_send, sid, p));
  }

  void arm_recv(std::size_t sid, std::size_t p) {
    auto & pr = vm_root->streams[sid].peers[p];
    if(pr.fd < 0) return;
    ++root->io_pending;
    io().recv_all(pr.fd, io_tag(op_recv, sid, p));
  }

  // The completion of the kernel application is an event in the stream, the awakening of those waiting.
  void io_complete(os::uring::completion const & c) {
    if(!c.tag) return; // cancellation
    auto op = io_op(c.tag >> 56);
    if(op == op_wake) { // push of the other lane: incoming will analyze the schedule
      root->wake_armed = false;
      return;
    }
    if(op == op_psi) { // host lacks memory:
      root->psi_armed = false;
      if(c.res >= 0) {
        pressure();
        arm_pressure();
      }
      return;
    }
    lane_guard g(vm_lock); // flux
    std::size_t sid = (c.tag >> 24) & 0xFFFFFFFF, p = c.tag & 0xFFFFFF;
    bool more = c.flags & IORING_CQE_F_MORE;
    if(!more) --root->io_pending;
    stream_rec & s = vm_root->streams[sid];
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
    stream_rec & s = vm_root->streams[sid];
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

  // Locking the flows between the bands (step 12.3b): the operation takes it, waiting (park) releases
  struct stream_lock {
    std::unique_lock<std::recursive_mutex> lk{vm_lock, std::defer_lock};
    stream_lock() {
      if(lanes_on.load(std::memory_order_acquire)) lk.lock();
    }
    void park(interpreter & vm) {
      bool had = lk.owns_lock();
      if(had) lk.unlock();
      vm.park();
      if(had) lk.lock();
    }
  };

  uint32_t stream_id(value const & s, span where) {
    lane_guard g(vm_lock);
    value const * v = shape_of(s);
    if(!v || !v->is<agg_v>() || types.name(v->as<agg_v>().type) != "stream" || v->as<agg_v>().fields.empty()) fail("ожидался поток", where);
    value const & id = v->as<agg_v>().fields[0].second;
    if(!id.is<int64_t>() || id.as<int64_t>() < 0 || uint64_t(id.as<int64_t>()) >= vm_root->streams.size()) fail("неизвестный поток", where);
    return uint32_t(id.as<int64_t>());
  }

  // parsing: The value of the flow carries the type of elements, if it is known at creation
  // - the fusion (cycle, branches) erases the number, but not the type.
  value stream_value(std::size_t id) {
    lane_guard g(vm_lock);
    agg_v a{types.named("stream"), {{sym("id"), value(int64_t(id))}}};
    if(checking() && id < vm_root->streams.size() && vm_root->streams[id].typed) a.fields.push_back({sym("elem"), erase(vm_root->streams[id].shape)});
    return value(std::move(a));
  }

  // Network and file operations (net *, file *): in parsing, only types.
  value io_op_call(std::string_view n, std::vector<value> & a, span where) {
    lane_guard g(vm_lock); // flow table - common for bands
    auto & ss = vm_root->streams;
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
    // host resources are only given (step 12.4)
// 
// Not given - not dump, but "0 devices".
    // Gives a program: transmits an open stream; takes away - closes it (the process sees the end).
    // Program - what the author gave (TZ_GRANT); background process - nothing (program gives).
    if((n == "net_listen" || n == "net_connect" || n == "file_open" || n == "file_create") && !granted(n.starts_with("file_") ? "fs" : "net")) {
      std::string_view who = background ? "процессу не дана (ресурс даёт программа: передаёт поток)" : "программе не дана (даёт пользователь: tz run --grant fs)";
      if(n.starts_with("file_")) fail_rt("io_error", cat(a.empty() || !a[0].is<str_v>() ? std::string("?") : a[0].as<str_v>().str(), ": нет файла — файловая система ", who), where);
      ss.emplace_back();
      ss.back().net = true; // Network without devices: no one to listen to, nowhere to connect
      if(n == "net_connect") {
        ss.back().peers.push_back({-1});
        ss.back().peers[0].open = false;
        deliver(ss.back(), make_event(0, "close", background ? "сеть процессу не дана" : "сеть программе не дана"));
      }
      return stream_value(ss.size() - 1);
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
      if(a.size() != 2 || !a[0].is<str_v>() || !a[1].is<int64_t>()) fail("connect(адрес, порт)", where);
      ss.emplace_back();
      stream_rec & s = ss.back();
      s.net = true;
      if(!os::ipv4(a[0].as<str_v>().str(), int(a[1].as<int64_t>()), s.addr)) fail_rt("io_error", "connect: неверный адрес", where);
      int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if(fd < 0) fail_rt("io_error", cat("connect: ", std::strerror(errno)), where);
      s.peers.push_back({fd});
      ++root->io_pending;
      io().connect(fd, &s.addr, io_tag(op_connect, ss.size() - 1, 0));
      return stream_value(ss.size() - 1);
    }
    if(n == "file_open" || n == "file_create") {
      if(a.size() != 1 || !a[0].is<str_v>()) fail(cat(n == "file_open" ? "open" : "create", "(путь)"), where);
      int fd = n == "file_open" ? ::open(a[0].as<str_v>().str().c_str(), O_RDONLY | O_CLOEXEC)
                                : ::open(a[0].as<str_v>().str().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
      if(fd < 0) fail_rt("io_error", cat(a[0].as<str_v>(), ": ", std::strerror(errno)), where);
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
      if(a.size() != 3 || !a[1].is<int64_t>() || !a[2].is<str_v>()) fail("send(сеть, peer, байты)", where);
      auto p = std::size_t(a[1].as<int64_t>());
      if(!s.net || p >= s.peers.size()) fail_rt("io_error", cat("send: нет соединения ", p), where);
      if(!s.peers[p].open) return value(false);
      s.peers[p].out.push_back(a[2].as<str_v>().str());
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
  void close_host(std::size_t sid) { // The queue of the file has already been completed (stream_do)
    stream_rec & s = vm_root->streams[sid];
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
      lane_guard g(vm_lock);
      auto & ss = vm_root->streams;
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
    if(checking()) { // parsing: type only; flow operation - runtime effect
      lane_guard g(vm_lock);
      stream_rec & s = vm_root->streams[sid];
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
    stream_lock sl; // flow can touch both lanes
    stream_rec & s = vm_root->streams[sid]; // Deque: The link is experiencing anticipation
    fiber_rec * me = &running();
    if(op == s_write) {
      if(s.net) fail("в сеть — send(сеть, peer, байты)", where);
      if(s.file_fd >= 0) {
        if(!s.file_write) fail("файл открыт на чтение", where);
        if(!x.is<str_v>()) fail(cat("в файл — байты (str), получено ", types.describe(x)), where);
        s.peers[0].out.push_back(x.as<str_v>().str());
        file_write_next(sid);
        return value();
      }
      if(!transferable(x)) fail(cat("в поток — только данные: ", types.describe(x)), where);
      while(s.buf.size() >= s.cap && !s.closed) {
        s.writers.push_back(me);
        sl.park(*this);
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
        sl.park(*this);
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
      if(s.file_fd >= 0 && s.file_write)
        while(!s.peers[0].out.empty()) { // queue
          s.writers.push_back(me);
          sl.park(*this);
        }
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
      if(value * c = find(env, db.as<n_ident>(n).name)) {
        // reference is its place (q.y = 40 at &q = ps[1]); the scalar array element is not a cell
        if(c->is<ref_v>() && db[n].parent && (db.is_member(db[n].parent) || db.is_index(db[n].parent))) {
          ref_v const & r = c->as<ref_v>();
          if(!r.path()) {
            ref_valid(r, db[n].where);
            if(checking() || !r.cell->is<array_v>()) return nullptr;
            return r.cell->as<array_v>().generic_cell(checked_index(*r.cell, value(r.index), db[n].where));
          }
          return path_place(r, 1 + r.more, db[n].where, checking());
        }
        return c;
      }
      fail(cat("неизвестная переменная ", str(db.as<n_ident>(n).name)), db[n].where);
    }
    if(db.is_index(n)) { // cell of the element is only in a common array
      auto const & i = db.index_of(n);
      value * arr = place(i.object, env, frame);
      flow idx = eval(i.index, env, frame);
      if(!arr || arr->is<rt_v>() || !idx.v.known()) return nullptr;
      return arr->as<array_v>().generic_cell(checked_index(*arr, idx.v, db[n].where));
    }
    if(db.is_member(n)) { // field
      auto const & m = db.member_of(n);
      value * obj = place(m.object, env, frame);
      if(obj && is_shared(*obj)) obj = &slot_of(*obj).v; // Shared - transparent: the field of stored
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
    if(db.is_member(n)) return eval_member(n, db.member_of(n), env, frame); // a.x is the kernel shape
    if(db.is_field(n)) return eval_field(n, db.field_of(n), env, frame);
    // a[i]: kernel if [] is not overloaded with code (overload to its types, otherwise the kernel below)
    if(db.is_index(n) && (user_ops.empty() || !user_ops.contains(known(sym_index_op).value) || db.in_prelude(n))) return eval_index(n, db.index_of(n), env, frame);
    auto op = str(o.op);
    bool assign_op = db.sym_kind(o.op) & ast_db::sc_assign;
    span where = db[n].where;
    // operator declared in a block (in the body of a function or structure for its blocks) is a
    // local function of a region
    if(!local_ops.empty() && !assign_op)
      if(symbol f = operator_entity(o); local_ops.contains(f.value))
        if(value * cell = env->find(f); cell && cell->is<fn_v>()) return call_value(*cell, o.args, env, frame, where, n.value);
    // Operator declared to be an unbuilt primitive code calls its entity (overload by
    // type; embedded is also a candidate)
    if(!user_ops.empty() || o.postfix)
      if(symbol f = operator_entity(o); user_ops.contains(f.value)) {
        auto const & ui = user_info(f);
        auto const & user = ui.cands;
        bool lazy = ui.lazy;
        auto site = [&] { return call_site{where, std::string(str(f)), n.value, frame}; };
        if(o.postfix || lazy) return call_overloaded(functions[f], o.args, env, frame, site());
        auto scalar = [](value const & v) { return v.is<int64_t>() || v.is<double>() || v.is<bool>(); };
        std::vector<value> vals;
        if(o.args.size() == 2 && !ui.scalar_params) { // Frequent: a + b over built-in scalars - no vector
          TZ_TRY(x, eval(o.args[0], env, frame));
          TZ_TRY(y, eval(o.args[1], env, frame));
          if(scalar(x) && scalar(y) && overrides.empty() && op != "&&" && op != "||") return ok(binary(op, x, y, where));
          vals.push_back(std::move(x));
          vals.push_back(std::move(y));
        } else
          for(node_id a: o.args) {
            TZ_TRY(v, eval(a, env, frame));
            vals.push_back(std::move(v));
          }
        bool builtin_scalars = !ui.scalar_params && std::ranges::all_of(vals, [](value const & v) { return v.is<int64_t>() || v.is<double>() || v.is<bool>(); });
        if(!builtin_scalars && (std::ranges::any_of(vals, [](value const & v) { return v.is<split_v>(); }) || user_applies(f, vals)))
          return dispatch_split(user, vals, 0, o.args, env, frame, site());
        if(auto r = apply_override(f, vals, o, n, env, frame, where)) return std::move(*r); // Checked { … } - and here
        // Built-in operation: user overload of these types is not accepted
        if(db.is_index(n)) return index_rt(n, db.index_of(n), vals[0], vals[1], env);
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
    if(!overrides.empty() && override_depth == 0 && o.args.size() == 2 && op != "&&" && op != "||" && !assign_op) {
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
    if(assign_op) {
      TZ_TRY(rhs0, eval(o.args[1], env, frame));
      value & rhs = rhs0;
      if(op == "=" && needs_take(o.args[1], rhs)) {
        flow t = take(o.args[1], std::move(rhs), env, frame);
        if(!t.normal()) return t;
        rhs = std::move(t.v);
      }
      // x = e and no name x is an announcement with type output
      if(op == "=" && db.is<n_ident>(o.args[0]) && [&] {
            value * c = env->find(db.as<n_ident>(o.args[0]).name);
            return !c || foreign_global(c, n, env, db.as<n_ident>(o.args[0]).name);
          }()) {
        symbol name = db.as<n_ident>(o.args[0]).name;
        if(checking()) implicit_decls.insert(n.value);
        if(checking())
          emit({report_event::stage, path, std::string(str(name)), rhs.known() ? cat("кт ", show_v(rhs)) : cat("рт ", types.describe(rhs)), where, {}});
        env->declare(name, std::move(rhs));
        return ok();
      }
      return assign(o.args[0], op, std::move(rhs), env, frame, where);
    }
    TZ_TRY(a, eval(o.args[0], env, frame));
    TZ_TRY(b, eval(o.args[1], env, frame));
    return ok(binary(op, a, b, where));
  }

  // place op = value. The expr parameter is the code of the caller: writing to it is writing to
  // its place (semantic inlining, §6: a = b in swap changes the x caller).
  flow assign(node_id lhs, std::string_view op, value rhs, scope * env, uint32_t frame, span where) {
    if(db.is<n_ident>(lhs))
      if(value * c = find(env, db.as<n_ident>(lhs).name); c && c->is<thunk_v>()) {
        thunk_v t = c->as<thunk_v>();
        uint64_t saved = std::exchange(ctx, t.ctx);
        ruled_hold rh{*this, t, ruled_env(t)};
        flow r;
        try {
          r = assign(t.expr, op, std::move(rhs), rh.s, t.frame, where);
        } catch(...) {
          ctx = saved;
          throw;
        }
        ctx = saved;
        return r;
      }
    if(db.is_member(lhs) && code_place(db.member_of(lhs).object, env)) { // code.field = v - mutation node (§7.4 )
      auto const & m = db.member_of(lhs);
      flow o = eval(m.object, env, frame);
      if(o.normal() && (code_agg(o.v) || (shape_of(o.v) && code_agg(*shape_of(o.v)))) && str(m.name) != "node") {
        if(op != "=") fail("изменение кода: только код.поле = значение", where);
        if(!o.v.known()) fail("изменение кода: узел должен быть известен при компиляции", where);
        mutate_node(code_node(o.v, where), m.name, rhs, where);
        return ok();
      }
    }
    written(lhs, env);
    if(db.is<n_ident>(lhs))
      if(value * c = find(env, db.as<n_ident>(lhs).name); c && c->is<ref_v>()) return ref_assign(c->as<ref_v>(), op, rhs, where);
    if(!checking() && db.is_index(lhs)) { // entry into an element: set, not cell
      auto const & ix = db.index_of(lhs);
      value * arr = place(ix.object, env, frame);
      if(!arr) fail("присваивание не в переменную", db[lhs].where);
      TZ_TRY(idx, eval(ix.index, env, frame));
      uint32_t k = checked_index(*arr, idx, db[lhs].where);
      value cur = arr->as<array_v>().get(k);
      arr->as<array_v>().set(k, op == "=" ? (is_record(rhs) ? to_type(std::move(rhs), place_type(cur), where) : std::move(rhs)) : binary(std::string_view(op).substr(0, 1), cur, rhs, where));
      return ok();
    }
    if(db.is_member(lhs) || db.is_index(lhs)) { // q.y = v, r[k] = v through a link - a record along its path (and in parsing )
      node_id root = lhs;
      while(db.is_member(root) || db.is_index(root)) root = db.is_member(root) ? db.member_of(root).object : db.index_of(root).object;
      if(db.is<n_ident>(root))
        if(value * c = find(env, db.as<n_ident>(root).name); c && c->is<ref_v>())
          if(auto r = path_ref(lhs, env, frame, lhs)) return path_assign(r->as<ref_v>(), op, rhs, where);
    }
    value * target = place(lhs, env, frame);
    if(!target) {
      if(checking() && db.is_index(lhs)) { // entry into the run-time-array element: type reconciliation
        node_id saved_write = std::exchange(index_write, lhs);
        flow cur_f = eval(lhs, env, frame);
        index_write = saved_write;
        if(!cur_f.normal()) return cur_f;
        value cur = std::move(cur_f.v);
        value res = op == "=" ? to_type(rhs, place_type(cur), where) : binary(std::string_view(op).substr(0, 1), cur, rhs, where);
        if(!unknown(cur) && !unknown(res) && types.describe(cur) != types.describe(res))
          fail(cat("элемент массива типа ", types.describe(cur), " нельзя заменить значением типа ", types.describe(res)),
              where);
        return ok();
      }
      fail("присваивание не в переменную", db[lhs].where);
    }
    if(ct_cells.contains(target) && ct_depth == 0 && !level_probe)
      fail("ct-переменная меняется только кодом при компиляции (ct { … })", where);
    bool pinned = target->pinned;
    bool out_init = op == "=" && db.is<n_ident>(lhs) && out_marker(target->hidden); // Out-Parameter: Recording - Building on Place
    if(!pinned && target->hidden && !out_init) not_hidden(*target, symbol{}, where); // x.f = ... with a hidden name.
    if(op == "=" && !target->hidden && res_any() && has_resource(*target)) drop_value(*target, where); // Overwrite - the old resource is released
    value result = op == "=" ? std::move(rhs) : binary(std::string_view(op).substr(0, 1), *target, rhs, where);
    if(op == "=" && is_record(result)) result = to_type(std::move(result), place_type(*target), where); // Struct holds it, {x: …} becomes it.
    if(vm_root->state_level && vm_root->state_level->holds(target)) { // place - state of the program: value in its level
      try {
        result = adopt_into(std::move(result), &*vm_root->state_level);
      } catch(diagnostic const & d) {
        fail(d.what(), where);
      }
    }
    *target = std::move(result);
    if(pinned) pin(*target, {}, where); // The ct field remains a state during compilation
    if(target->is<array_v>() && db.is<n_ident>(lhs)) target->as<array_v>().gen = ++array_gen; // New buffer - old links are invalid
    return ok();
  }

  value unary(std::string_view op, value const & x, span where) {
    if(unknown(x)) return x;
    if(!x.known()) return erase(unary(op, *shape_or_fail(x, where), where));
    if(!op_declared(op, x, nullptr)) fail(cat("оператор ", op, " не определён для ", types.describe(x)), where);
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
  // out-parameter (§6.2): the place of the result at the caller
// 
// The name of the argument is the place:
  // No caller is declared; the former value goes away; before writing the function is hidden (reading
  // is an error), before leaving the function is written on all paths. My:* recording form.
  value out_place(node_id arg, node_id param, scope * env) {
    symbol name = db.as<n_ident>(arg).name;
    value * cell = find(env, name);
    if(!cell) cell = env->declare(name, value());
    if(!checking() || !cell->is<split_v>()) *cell = value();
    else *cell = value();
    cell->hidden = param.value + 1;
    return *cell;
  }
  bool out_marker(uint32_t h) const {
    return h && db.is<n_param>(node_id(h - 1)) && db.as<n_param>(node_id(h - 1)).mode == param_mode::out;
  }
  void out_written(node_list const & params, scope * inner, call_site const & site) {
    for(node_id pn: params) {
      auto const & p = db.as<n_param>(pn);
      if(p.mode != param_mode::out) continue;
      value const * c = inner->find(p.name);
      if(!c) continue;
      bool unset = out_marker(c->hidden), some = false;
      if(c->is<split_v>())
        for(auto const & a: c->as<split_v>().alts) some = some || out_marker(a.hidden);
      if(unset || some) {
        diagnostic d(cat("out-параметр ", str(p.name), unset ? " не записан" : " не записан в одном из путей", " к выходу из функции"), db[pn].where);
        for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
        d.note("вызов", site.where);
        throw d;
      }
    }
  }

  void not_hidden(value const & v, symbol name, span where) {
    uint32_t h = v.hidden;
    if(!h && v.is<split_v>())
      for(auto const & a: v.as<split_v>().alts)
        if(a.hidden) h = a.hidden;
    if(!h) return;
    if(out_marker(h)) {
      diagnostic d(cat(name ? cat("имя ", str(name)) : std::string("значение"), " ещё не записано (out-параметр ", str(db.as<n_param>(node_id(h - 1)).name), ")"), where);
      for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
      throw d;
    }
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

  flow eval_hide(node_id n, symbol name, scope * env) {
    value * cell = find(env, name);
    if(!cell || cell->is<thunk_v>()) fail(cat("hide: нет переменной ", str(name)), db[n].where);
    not_hidden(*cell, name, db[n].where);
    hide_cell(cell, n);
    return ok();
  }

  // Compilation state within the value: ct-fields and hidden names (§5.6).
  std::string state_of(value const & x) const {
    std::string s;
    if(x.hidden) s += "H";
    if(x.ct) s += cat("C", x.ct);
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
      if(checking() && (op == "/" || op == "%") && !b.known() && types.describe(b) == types.int_name()) rt_check("div_by_zero", where);
      return erase(arith(op, *shape_or_fail(erase(a), where), *shape_or_fail(erase(b), where), where));
    }
    return arith(op, a, b, where);
  }

  // Whether a primitive operator is declared for these types (O15, M6 step 8): what operations are
  // in which type - prelude ads by type (see below).
// ); the machine performs only what is declared. *Mine: *
  // Primitive operator - only "type equal" (without i64 → f64); the result is a pair of types.
  bool op_declared(std::string_view op, value const & a, value const * b) {
    // fast path - built-in types and primitive operators: table [operator][type][type]; 0 - not yet
    // asked, 1 - not, 2 - announced; unary - their rows
    uint32_t ta = types.of(a).value, tb = b ? types.of(*b).value : 0;
    int oi = -1;
    if(ta < 16 && tb < 16 && !op.empty() && op.size() <= 2) {
      char c0 = op[0], c1 = op.size() > 1 ? op[1] : 0;
      if(!b) oi = !c1 && c0 == '-' ? 16 : !c1 && c0 == '!' ? 17 : -1;
      else if(!c1) {
        switch(c0) {
          case '+': oi = 0; break;
          case '-': oi = 1; break;
          case '*': oi = 2; break;
          case '/': oi = 3; break;
          case '%': oi = 4; break;
          case '<': oi = 7; break;
          case '>': oi = 8; break;
          case '&': oi = 11; break;
          case '|': oi = 12; break;
          case '^': oi = 13; break;
          default: break;
        }
      } else if(c1 == '=') oi = c0 == '=' ? 5 : c0 == '!' ? 6 : c0 == '<' ? 9 : c0 == '>' ? 10 : -1;
      else if(c0 == c1) oi = c0 == '<' ? 14 : c0 == '>' ? 15 : -1;
    }
    if(oi >= 0) {
      int8_t & c = declared_fast[oi][ta][tb];
      if(!c) c = op_declared_slow(op, a, b) ? 2 : 1;
      return c == 2;
    }
    return op_declared_slow(op, a, b);
  }
  int8_t declared_fast[18][16][16] = {};
  bool op_declared_slow(std::string_view op, value const & a, value const * b) {
    uint64_t packed = 0;
    for(char ch: op.substr(0, 3)) packed = (packed << 8) | uint8_t(ch);
    uint64_t key = (packed << 40) | (uint64_t(types.of(a).value & 0xFFFFF) << 20) | (b ? types.of(*b).value & 0xFFFFF : 0xFFFFF);
    auto [it, fresh] = declared_ops.try_emplace(key, false);
    if(fresh) {
      std::string ta = types.describe(a), tb = b ? types.describe(*b) : std::string();
      auto accepts = [&](node_id p, std::string const & t) {
        node_id ty = db.as<n_param>(p).type;
        return !ty || (db.is<n_ident>(ty) && str(db.as<n_ident>(ty).name) == t);
      };
      if(auto fs = functions.find(sym(cat(b ? "infix" : "prefix", op))); fs != functions.end())
        for(node_id fn: fs->second) {
          auto const & ps = db.as<n_fn>(fn).params;
          if(intrinsic_operator(fn) && ps.size() == (b ? 2u : 1u) && accepts(ps[0], ta) && (!b || accepts(ps[1], tb))) it->second = true;
        }
    }
    return it->second;
  }
  std::unordered_map<uint64_t, bool> declared_ops;

  value arith(std::string_view op, value const & a, value const & b, span where) {
    // Strictly: without implicit i64 <-> f64 (§5.8) - no operation if it is not in the ads
    if(!op_declared(op, a, &b)) fail(cat("оператор ", op, " не определён для ", types.describe(a), " и ", types.describe(b)), where);
    if(a.is<int64_t>() && b.is<int64_t>()) return int_op(op, a.as<int64_t>(), b.as<int64_t>(), where);
    if(a.is<double>() && b.is<double>()) return float_op(op, a.as<double>(), b.as<double>(), where);
    if(a.is<str_v>() && b.is<str_v>()) {
      if(op == "+") return value(a.as<str_v>() + b.as<str_v>());
      if(op == "==") return value(a.as<str_v>() == b.as<str_v>());
      if(op == "!=") return value(a.as<str_v>() != b.as<str_v>());
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
      if(y == -1) return op == "/" ? w(0 - uint64_t(x)) : value(int64_t(0)); // INT_MIN / -1 - modular, as in machine code
      return value(op == "/" ? x / y : x % y);
    }
    // bits (for encoding instructions in the language, §10.4); shift - module 64, like the
    // iron of both targets; >> - arithmetic (i64 with a sign)
    if(op == "&") return w(uint64_t(x) & uint64_t(y));
    if(op == "|") return w(uint64_t(x) | uint64_t(y));
    if(op == "^") return w(uint64_t(x) ^ uint64_t(y));
    if(op == "<<") return w(uint64_t(x) << (y & 63));
    if(op == ">>") return value(x >> (y & 63));
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
      value got;
      if(code_agg(x) && node_kind_named(want) && type_fit(x, want, false, got) != fit::no) return; // The code node ast.X is the instance ast. X
      if(std::string why; is_record(x) && structs.contains(sym(want)) && record_fit(x, want, got, &why) == fit::no) // Reason: Excessive or inappropriate field
        fail(cat(types.describe(x), " не становится ", want, ": ", why), where);
      if(empty_record(x)) fail(cat("{} — значение типа места: есть у struct, i64, f64, не у ", want), where);
      if(types.describe(x) != want && types.name(types.of(x)) != want)
        fail(cat("ожидался тип ", want, ", получен ", types.describe(x)), where);
    };
    if(v.is<split_v>())
      for(auto const & a: v.as<split_v>().alts) check1(a);
    else check1(v);
  }

  // --- challenge ---

  flow eval_node(node_id n, n_call const & c, scope * env, uint32_t frame) {
    if(node_id rc = db.require_cond(n)) return eval_require(n, rc, env, frame);
    if(symbol h = db.hide_name(n)) return eval_hide(n, h, env);
    if(node_id b = db.try_compile_body(n)) return eval_try_compile(n, b, env, frame);
    span where = db[n].where;
    if(db.is_member(c.callee)) { // a.f(b) ≡ f(a, b) (§4.5)
      auto const & m = db.member_of(c.callee);
      // entity .f(…) is a function declared in the body of the entity function:
//  
      if(node_id qm = qualified_module(m.object, env)) { // geo.vec.norm(…) - overload declared in the module
        std::vector<node_id> set;
        if(auto it = functions.find(m.name); it != functions.end())
          for(node_id f: it->second)
            if(module_of(f) == qm) set.push_back(f);
        if(set.empty()) fail(cat("в модуле ", str(db.as<n_module>(qm).name), " нет функции ", str(m.name)), where);
        return call_overloaded(set, c.args, env, frame, {where, std::string(str(m.name)), n.value, frame});
      }
      if(db.is<n_ident>(m.object)) {
        std::optional<fn_v> ent;
        if(value * cell = find(env, db.as<n_ident>(m.object).name)) {
          if(cell->is<fn_v>() && !entity_bodies(cell->as<fn_v>()).empty()) ent = cell->as<fn_v>();
        } else if(functions.contains(db.as<n_ident>(m.object).name)) ent = fn_v{db.as<n_ident>(m.object).name, {}, nullptr};
        if(ent) {
          std::vector<node_id> set;
          for(node_id f: entity_bodies(*ent))
            if(db.is<n_block>(db.as<n_fn>(f).body))
              for(node_id s: db.as<n_block>(db.as<n_fn>(f).body).items)
                if(db.is<n_fn>(s) && !db.as<n_fn>(s).scoped && db.as<n_fn>(s).name == m.name) set.push_back(s);
          // on arity - one: a call by node (type of code generator parameter - operand tag, value -
          // target register), otherwise - a set of overloads
          std::vector<node_id> fit;
          for(node_id s: set)
            if(db.as<n_fn>(s).params.size() == c.args.size()) fit.push_back(s);
          scope * home = ent->name ? entity_scope(ent->name) : ent->env; // entity neighbors are visible to each other.
          if(fit.size() == 1) return call_value(value(fn_v{{}, fit[0], home, {}}), c.args, env, frame, where, n.value);
          if(!set.empty()) return call_value(value(fn_v{m.name, {}, home, set}), c.args, env, frame, where, n.value);
        }
      }
      node_list args{m.object};
      args.insert(args.end(), c.args.begin(), c.args.end());
      return call_named(m.name, args, env, frame, where, n.value);
    }
    if(db.is<n_ident>(c.callee)) {
      symbol name = db.as<n_ident>(c.callee).name;
      if(value * cell = find(env, name)) {
        if(cell->is<thunk_v>()) {
          thunk_v t = thunk_arg(c.callee, env, where);
          if(db.is<n_lambda>(t.expr) && db.as<n_lambda>(t.expr).block) return call_block(t, c.args, env, frame, where);
        }
        TZ_TRY(fv, read(cell));
        return call_value(fv, c.args, env, frame, where, n.value);
      }
      // code(x) - AST expression value: the expr-parameter - the code of the caller
      // (bypass and build node, §7.4):
//.
      if(name == known(sym_code) && c.args.size() <= 1 && !functions.contains(name)) // code() is an empty node
        return ok(code_value(c.args.empty() ? node_id() : code_of(c.args[0], env)));
      if(name == known(sym_ref) && c.args.size() == 1 && !functions.contains(name)) return eval_ref(c.args[0], env, frame, where);
      if(name == sym_intercept && !functions.contains(name)) return intercept(c.args, env, frame, where);
      if(name == sym_alloc_in && !functions.contains(name)) return alloc_in(c.args, env, frame, where);
      if(name == sym_append && !functions.contains(name)) return append(c.args, env, frame, n);
      if(name == sym_pin_storage && !functions.contains(name)) return pin_storage(c.args, env, n);
      if(name == sym_set_ct_state && !functions.contains(name)) return set_ct_state(c.args, env, frame, where);
      if(name == sym_spawn && !functions.contains(name)) return spawn(c.args, env, frame, where);
      if(name == sym_pressure && !functions.contains(name)) return pressure_handler(c.args, env, frame, where);
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
    if(!f.set.empty()) { // a set of overloads (local or entity member): choice as a function, area as the set environment
      std::vector<node_id> fns = f.set;
      return call_overloaded(fns, args, env, frame, {where, std::string(str(db.as<n_fn>(fns[0]).name)), node, frame}, f.env);
    }
    if(f.name) return call_named(f.name, args, env, frame, where, node);
    node_list const & params = db.is<n_lambda>(f.lambda) ? db.as<n_lambda>(f.lambda).params : db.as<n_fn>(f.lambda).params;
    node_id body = db.is<n_lambda>(f.lambda) ? db.as<n_lambda>(f.lambda).body : db.as<n_fn>(f.lambda).body;
    if(params.size() != args.size()) fail(cat("ожидалось ", params.size(), " аргументов, передано ", args.size()), where);
    scope * inner = new_scope(f.env);
    inner->caller = env; // Call environment (O4): code generator sees acc, target...
    if(db.is<n_fn>(f.lambda)) inner->fn = f.lambda;
    scope_hold hold{*this, inner};
    for(uint32_t i = 0; i < params.size(); ++i) {
      auto const & p = db.as<n_param>(params[i]);
      if(p.mode == param_mode::expr) {
        inner->declare(p.name, value(thunk_v{args[i], frame, env, ctx}));
        continue;
      }
      if(value * cell = i < args.size() ? place_or_null(args[i], env) : nullptr) {
        inner->alias(p.name, cell);
        if(res_any() && has_resource(*cell)) inner->borrows.push_back({cell, args[i], env});
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
    if(auto it = functions.find(name); it != functions.end()) {
      if(node && module_order.size() > 1 && !name_visible(module_of(node_id(node)), name, it->second)) {
        node_id home = module_of(it->second.front());
        fail(cat("функция ", str(name), " объявлена в модуле ", home ? str(db.as<n_module>(home).name) : "?",
          ", а use его здесь нет: модули изолированы, use не транзитивен — use ", home ? str(db.as<n_module>(home).name) : "?",
          "; или квалифицированно: ", home ? str(db.as<n_module>(home).name) : "?", ".", str(name), "(…)"), where);
      }
      return call_overloaded(it->second, with_implicit(it->second, args, node), env, frame, {where, std::string(str(name)), node, frame});
    }
    if(args.size() == 1 && str(name) == "shared") { // shared(x) - the value goes to the storage (transfer, like an assignment)
      TZ_TRY(v0, eval(args[0], env, frame));
      TZ_TAKE(v, args[0], v0, env, frame);
      if(checking() && !v.known()) return ok(make_shared(std::move(v)));
      return ok(make_shared(std::move(v)));
    }
    if(args.size() == 1 && str(name) == "shared_count") { // Number of references to stored (observation)
      TZ_TRY(v, eval(args[0], env, frame));
      if(!is_shared(v)) fail("shared_count: ожидался shared", where);
      return ok(slot_of(v).count);
    }
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
  // candidate restriction (§5.4); choose the most specific. base - environment of a local
  // set of congestion (otherwise a global area)
  flow call_overloaded(std::vector<node_id> const & fns, node_list const & args_in, scope * env, uint32_t frame, call_site site, scope * base = nullptr) {
    node_list args = args_in;
    if(std::ranges::any_of(args_in, [&](node_id a) { return db.is<n_spread>(a); })) { // ...t - in separate arguments
      std::vector<value> vals;
      args.clear();
      uint32_t open_at = ~0u; // disclosure of run-time-length (parsing): place in arguments
      value open_elem;
      node_id open_node;
      for(node_id a: args_in) {
        if(!db.is<n_spread>(a)) {
          TZ_TRY(v, eval(a, env, frame));
          vals.push_back(std::move(v));
          args.push_back(a);
          continue;
        }
        TZ_TRY(t, eval(db.as<n_spread>(a).value, env, frame));
        value const * x = shape_of(t);
        if(!x || !x->is<array_v>()) fail(cat("...: раскрывается массив или кортеж, получено ", types.describe(t)), db[a].where);
        if(!t.known() && t.is<rt_v>()) { // run-time: parsing of all lengths that overload takes
          if(open_at != ~0u) fail("...: одно раскрытие массива рт-длины на вызов", db[a].where);
          open_at = uint32_t(vals.size());
          open_elem = element_type(t, db[a].where);
          open_node = a;
          continue;
        }
        for(uint32_t k = 0; k < x->as<array_v>().size(); ++k) {
          vals.push_back(x->as<array_v>().get(k));
          args.push_back(a); // No space: value of the element
        }
      }
      if(open_at != ~0u) return spread_lengths(fns, vals, args, open_at, open_elem, open_node, env, frame, site, base);
      if(!checking() && !std::ranges::any_of(fns, [&](node_id f) { return arity_fits(f, vals.size()); })) {
        diagnostic d(cat("...: ", vals.size(), " аргументов — нет перегрузки ", site.what, " такой длины"), site.where, diag_kind::runtime);
        d.type = "unpack_failed";
        for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
        throw d;
      }
      return dispatch_split(fns, vals, 0, args, env, frame, site, base);
    }
    std::vector<value> vals(args.size());
    for(uint32_t i = 0; i < args.size(); ++i) {
      bool expr_everywhere = true;
      for(node_id f: fns) {
        auto const & ps = db.as<n_fn>(f).params;
        if(i >= ps.size() || db.as<n_param>(ps[i]).mode != param_mode::expr) expr_everywhere = false;
      }
      bool out_everywhere = !fns.empty() && db.is<n_ident>(args[i]);
      for(node_id f: fns) {
        auto const & ps = db.as<n_fn>(f).params;
        if(i >= ps.size() || db.as<n_param>(ps[i]).mode != param_mode::out) out_everywhere = false;
      }
      if(expr_everywhere) vals[i] = value(thunk_v{args[i], frame, env, ctx});
      else if(out_everywhere) vals[i] = out_place(args[i], db.as<n_fn>(fns[0]).params[i], env);
      else if(value * cell = checking() ? nullptr : place_or_null(args[i], env); cell && cell->is<array_v>())
        vals[i] = value(array_v::borrow(cell->as<array_v>())); // place-link, not copy
      else {
        TZ_TRY(v, eval(args[i], env, frame));
        vals[i] = std::move(v);
      }
    }
    return dispatch_split(fns, vals, 0, args, env, frame, site, base);
  }

  // The split argument: Continuation-challenge is cloned by alternatives.
  bool arity_fits(node_id f, std::size_t n) const {
    std::size_t m = db.as<n_fn>(f).params.size();
    return rest_param(f) ? n + 1 >= m : n == m;
  }

  // f(...t), length t - run-time (parsing). *My:* length - a value like the sample array
  // {x, ...xs}: understand all the lengths that takes at least one overload - each exact
  // length separately, the rest (tail ...xs) - one "open" call, where the tail - the array
  // run-time - lengths; the results merge like branches if. The length that no one takes
  // is runtime, the unpack_failed error.
  flow spread_lengths(std::vector<node_id> const & fns, std::vector<value> const & vals, node_list const & args, uint32_t at,
      value const & elem, node_id where, scope * env, uint32_t frame, call_site const & site, scope * base) {
    std::size_t given = vals.size();
    auto with = [&](std::size_t k, std::vector<value> & out, node_list & out_args) {
      out.assign(vals.begin(), vals.begin() + at);
      out_args.assign(args.begin(), args.begin() + at);
      for(std::size_t j = 0; j < k; ++j) {
        out.push_back(elem);
        out_args.push_back(where);
      }
      out.insert(out.end(), vals.begin() + at, vals.end());
      out_args.insert(out_args.end(), args.begin() + at, args.end());
    };
    std::set<std::size_t> lengths;
    std::size_t open = ~std::size_t(0); // tail: lengths from open + 1
    for(node_id f: fns) {
      std::size_t m = db.as<n_fn>(f).params.size();
      if(rest_param(f)) {
        std::size_t k = m - 1 > given ? m - 1 - given : 0;
        lengths.insert(k);
        open = std::min(open, k);
      } else if(m >= given)
        lengths.insert(m - given);
    }
    if(open != ~std::size_t(0))
      for(std::size_t k = open; k < *lengths.rbegin(); ++k) lengths.insert(k); // between-tail
    if(lengths.empty()) fail(cat("нет перегрузки ", site.what, ", принимающей раскрытие ", printer(db).print(where)), site.where);
    if(open == ~std::size_t(0) || *lengths.begin() > 0 || std::ranges::adjacent_find(lengths, [](std::size_t a, std::size_t b) { return b != a + 1; }) != lengths.end())
      rt_check("unpack_failed", site.where); // There is a length that no one takes.
    flow out;
    bool first = true;
    for(std::size_t k: lengths) {
      std::vector<value> xs;
      node_list as;
      with(k, xs, as);
      flow r = dispatch_split(fns, xs, 0, as, env, frame, site, base);
      out = first ? r : join_branches(out, r);
      first = false;
    }
    if(open != ~std::size_t(0)) { // lengths longer than those listed - only tail overload
      std::vector<node_id> tails;
      for(node_id f: fns)
        if(rest_param(f)) tails.push_back(f);
      std::vector<value> xs;
      node_list as;
      with(*lengths.rbegin() + 1, xs, as);
      flow r = select_and_invoke(tails, xs, as, env, frame, site, base, false, true);
      out = join_branches(out, r);
    }
    return out;
  }

  flow dispatch_split(std::vector<node_id> const & fns, std::vector<value> & vals, uint32_t from, node_list const & args,
      scope * env, uint32_t frame, call_site const & site, scope * base = nullptr) {
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
          r = dispatch_split(fns, copy, i + 1, args, env, frame, site, base);
        } catch(diagnostic & d) { // Trace - the way: what is the argument in what state
          d.note(cat("в пути [", i < args.size() && db.is<n_ident>(args[i]) ? cat(str(db.as<n_ident>(args[i]).name), ": ") : std::string(),
                     alt.known() ? show_v(alt) : types.describe(alt), alt.ct ? cat(", ct ", show_v(ct_states.at(alt.ct))) : std::string(), "]"),
              site.where);
          throw;
        }
        out = first ? r : join_branches(out, r);
        first = false;
      }
      vals[i] = orig;
      return out;
    }
    return select_and_invoke(fns, vals, args, env, frame, site, base);
  }

  // The result of the rule of specificity (overload_pick prelude): the order of candidates,
  // how much to parse runtime - overload, ambiguity and how many are equal to the first.
  struct overload_choice {
    std::vector<uint32_t> order;
    uint32_t reach = 0;
    bool ambiguous = false;
    uint32_t tied = 0;
  };
  std::unordered_map<std::string, overload_choice> overload_choices;
  node_id overload_fn;
  overload_choice const & choose_overload(std::vector<std::vector<int64_t> const *> const & feats, std::vector<bool> const & maybes, span where) {
    std::string key = checking() ? "c" : "r";
    for(std::size_t k = 0; k < feats.size(); ++k) {
      key += maybes[k] ? '?' : '|';
      for(int64_t x: *feats[k]) key += cat(x, ",");
    }
    if(auto it = overload_choices.find(key); it != overload_choices.end()) return it->second;
    overload_choice out;
    if(feats.size() == 1) out.order = {0}; // There is nothing to choose (and without language: choice does not choose itself)
    else {
      if(chain_helper) fail("выбор перегрузки в коде правил выбора (overload_pick, chain_resolve): там вызовы без перегрузок", where);
      if(!overload_fn)
        for(uint32_t i = db.size(); i-- > 0;)
          if(db.is<n_fn>(node_id(i)) && db.strings.text(db.as<n_fn>(node_id(i)).name) == "overload_pick") {
            overload_fn = node_id(i);
            break;
          }
      if(!overload_fn) fail("нет overload_pick в прелюдии — перегрузку не выбрать", where);
      std::vector<value> fs, ms;
      for(std::size_t k = 0; k < feats.size(); ++k) {
        std::vector<value> xs;
        for(int64_t x: *feats[k]) xs.push_back(value(x));
        fs.push_back(value(array_v::of(std::move(xs))));
        ms.push_back(value(bool(maybes[k])));
      }
      value r = in_helper(overload_fn, {value(array_v::of(std::move(fs))), value(array_v::of(std::move(ms))), value(checking())});
      auto field = [&](std::string_view f) -> value const & {
        for(auto const & [k, v]: r.as<agg_v>().fields)
          if(str(k) == f) return v;
        fail(cat("overload_pick: нет поля ", f), where);
      };
      auto const & order = field("order").as<array_v>();
      for(uint32_t k = 0; k < order.size(); ++k) out.order.push_back(uint32_t(order.get(k).as<int64_t>()));
      out.reach = uint32_t(field("reach").as<int64_t>());
      out.ambiguous = field("ambiguous").as<bool>();
      out.tied = uint32_t(field("tied").as<int64_t>());
    }
    if(out.order.size() == 1 && checking() && maybes[0]) out.reach = 1; // Like the rule: the only "may be"
    return overload_choices.emplace(std::move(key), std::move(out)).first->second;
  }

  flow select_and_invoke(std::vector<node_id> const & fns, std::vector<value> & vals, node_list const & args,
      scope * env, uint32_t frame, call_site const & site, scope * parent = nullptr, bool repacked = false, bool open = false) {
    scope * base = parent ? parent : globals; // local set: its environment
    struct candidate {
      node_id fn;
      std::vector<int64_t> feat; // Signs of specificity (overload_pick prelude)
      bool maybe = false; // Whether it is suitable - decide the value (unpacking run-time values in parsing)
      std::vector<std::pair<uint32_t, value>> conv; // argumentation
    };
    std::vector<candidate> good;
    std::vector<std::string> rejected;
    uint32_t by_value = 0; // cut off by value (sample, require), not type/arnost
    for(node_id f: fns) {
      auto const & fn = db.as<n_fn>(f);
      bool tail = rest_param(f);
      if(tail ? vals.size() + 1 < fn.params.size() : fn.params.size() != vals.size()) {
        rejected.push_back(cat(describe_fn(f), ": ожидает ", tail ? "не меньше " : "", fn.params.size() - (tail ? 1 : 0), " аргументов"));
        continue;
      }
      std::vector<int64_t> feat{tail ? 1 : 0, 0}; // tail of ...xs leading to require, then parameters
      bool maybe = false;
      std::string why;
      std::vector<std::pair<uint32_t, value>> conv; // Converted arguments of the candidate (representable)
      for(uint32_t i = 0; i < fn.params.size() && why.empty(); ++i) {
        auto const & p = db.as<n_param>(fn.params[i]);
        if(tail && i + 1 == fn.params.size()) { // tail: T... xs - each element is represented in T
          if(p.type && db.is<n_ident>(p.type))
            for(uint32_t j = i; j < vals.size() && why.empty(); ++j) {
              value got;
              fit r = type_fit(vals[j], str(db.as<n_ident>(p.type).name), p.strict, got);
              if(r == fit::no) why = cat("аргумент ", j + 1, " не ", str(db.as<n_ident>(p.type).name));
              else if(r != fit::exact) conv.push_back({j, got});
              maybe = maybe || r == fit::maybe;
            }
          break;
        }
        if(p.type && db.is<n_pat>(p.type)) { // sample: unpacking - candidate condition (as required)
          scope * probe = new_scope(base);
          scope_hold hold{*this, probe};
          unpacked r = unpack_into(p.type, vals[i], probe, probe, 0, db[fn.params[i]].where);
          if(r == unpacked::no) {
            why = cat("аргумент ", i + 1, " не распаковывается в ", printer(db).print(p.type));
            ++by_value;
          }
          else feat.push_back(100 + int64_t(pattern_checks(p.type)));
          maybe = maybe || r == unpacked::maybe;
        } else if(p.mode == param_mode::out) {
          feat.push_back(2); // out - the place of the result: the value is given by the function
        } else if(p.type && db.is<n_ident>(p.type)) {
          auto want = str(db.as<n_ident>(p.type).name);
          value got;
          fit r = type_fit(vals[i], want, p.strict, got);
          if(r == fit::no) {
            why = cat("параметр ", str(p.name), " ожидает ", p.strict ? "ровно " : "", want, ", передано ", checking() || !vals[i].known() ? types.describe(vals[i]) : cat(types.describe(vals[i]), " ", show_v(vals[i])));
            if(std::string rw; !p.strict && is_record(vals[i]) && structs.contains(sym(want)) && record_fit(vals[i], want, got, &rw) == fit::no) why += cat(" (", rw, ")");
            by_value += numeric_pair(vals[i], want) && !p.strict; // type fits, doesn't fit the value
          } else {
            feat.push_back(r == fit::exact ? 2 : 1); // "Type equals" or "can represent"
            maybe = maybe || r == fit::maybe;
            if(r != fit::exact) conv.push_back({i, got});
          }
        }
      }
      if(why.empty()) {
        std::vector<value> * in = &vals;
        std::vector<value> converted;
        if(!conv.empty()) { // Demand sees values in parameter types
          converted = vals;
          for(auto & [k, v]: conv) converted[k] = v;
          in = &converted;
        }
        auto [passed, n_req, reason, unsure] = leading_requires(f, *in, args, env, base, open);
        if(!passed) {
          why = reason;
          by_value += reason == "require не выполнено";
        }
        feat[1] = n_req;
        maybe = maybe || unsure;
      }
      if(why.empty()) good.push_back({f, std::move(feat), maybe, std::move(conv)});
      else rejected.push_back(cat(describe_fn(f), ": ", why));
    }

    // No one approached directly - the argument pack and the tuple are interchangeable:
// 
// 
    // My:* This is a level below a direct coincidence - it only tries
    // when no one comes straight.
    if(good.empty() && !repacked) {
      bool pack = std::ranges::any_of(fns, [&](node_id f) {
        auto const & ps = db.as<n_fn>(f).params;
        return ps.size() == 1 && db.as<n_param>(ps[0]).type && db.is<n_pat>(db.as<n_param>(ps[0]).type) && db.as<n_pat>(db.as<n_param>(ps[0]).type).type &&
            str(db.as<n_ident>(db.as<n_pat>(db.as<n_param>(ps[0]).type).type).name) == "array";
      });
      if(pack && vals.size() != 1) { // f(1, 2) → f(array{1, 2})
        std::vector<value> packed{value(array_v::of(vals))};
        return select_and_invoke(fns, packed, {}, env, frame, site, parent, true);
      }
      value const * x = vals.size() == 1 ? shape_of(vals[0]) : nullptr;
      if(x && x->is<array_v>() && (vals[0].known() || !vals[0].is<rt_v>())) { // g(t) → g(t[0], t[1], …)
        std::vector<value> spread;
        for(uint32_t k = 0; k < x->as<array_v>().size(); ++k) spread.push_back(x->as<array_v>().get(k));
        if(std::ranges::any_of(fns, [&](node_id f) { return db.as<n_fn>(f).params.size() == spread.size() || (rest_param(f) && spread.size() + 1 >= db.as<n_fn>(f).params.size()); }))
          return select_and_invoke(fns, spread, {}, env, frame, site, parent, true);
      }
    }
    // all cut off values - runtime - overload found no branch: exception
    // 
    if(good.empty() && by_value && by_value == rejected.size()) {
      diagnostic d(cat("нет перегрузки ", site.what, " для этих значений"), site.where, diag_kind::runtime);
      d.type = "unpack_failed";
      for(auto & r: rejected) d.note(cat("кандидат ", r), site.where);
      for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
      throw d;
    }
    if(good.empty()) {
      diagnostic d(cat("нет подходящей перегрузки ", site.what), site.where);
      for(auto & r: rejected) d.note(cat("кандидат ", r), site.where);
      if(!path.empty()) d.note(cat("в продолжении ", path), site.where);
      for(auto it = stack.rbegin(); it != stack.rend(); ++it) d.note(cat("в ", it->what), it->where);
      throw d;
    }
    // the rule of specificity - in language (overload_pick prelude, M6 step 5); the result
    // by features is remembered: the rule is a pure function
    std::vector<std::vector<int64_t> const *> feats;
    std::vector<bool> maybes;
    for(auto const & c: good) {
      feats.push_back(&c.feat);
      maybes.push_back(c.maybe);
    }
    overload_choice const & pick = choose_overload(feats, maybes, site.where);
    {
      std::vector<candidate> sorted;
      for(uint32_t k: pick.order) sorted.push_back(std::move(good[k]));
      good = std::move(sorted);
    }
    // runtime - overload in terms of:
// parsing candidates,
    // whose unpacking depends on the run-time-value, all are sorted out - by specificity,
    // to the first exactly suitable; the results merge like if branches.
    if(pick.reach > 0) {
      std::vector<candidate> reach(good.begin(), good.begin() + pick.reach);
      if(reach.back().maybe && !covers_all_lengths(reach)) rt_check("unpack_failed", site.where); // Maybe none of them will.
      if(reach.size() > 1) {
        flow out;
        for(std::size_t k = 0; k < reach.size(); ++k) {
          auto copy = vals;
          for(auto & [i, v]: reach[k].conv) copy[i] = v;
          if(site.node) emit({report_event::resolved, {}, cat(reach[k].fn.value), {}, {}, {}, site.node});
          invoke_parent = parent;
          invoke_open = open;
          flow r = invoke(reach[k].fn, copy, args, env, site);
          out = k ? join_branches(out, r) : r;
        }
        return out;
      }
    }
    if(pick.ambiguous) {
      diagnostic d(cat("неоднозначный вызов ", site.what), site.where);
      for(uint32_t k = 0; k < pick.tied; ++k) d.note(cat("кандидат ", describe_fn(good[k].fn)), db[good[k].fn].where);
      throw d;
    }
    // Which overload has chosen the challenge in this instance to move from
    // editor
    if(checking() && site.node) emit({report_event::resolved, {}, cat(good[0].fn.value), {}, {}, {}, site.node});
    invoke_parent = parent;
    invoke_open = open;
    for(auto & [i, v]: good[0].conv) vals[i] = std::move(v); // Values - in the types of parameters selected
    return invoke(good[0].fn, vals, args, env, site);
  }

  // require at the beginning of the body: run-time - Candidate condition does not
  // cut off - it is checked in runtime (§5.4). The fourth condition is run-time
  // (will decide the value: runtime - overload).
  std::tuple<bool, int, std::string, bool> leading_requires(node_id f, std::vector<value> & vals, node_list const & args, scope * env, scope * base, bool open = false) {
    auto const & fn = db.as<n_fn>(f);
    if(!db.is<n_block>(fn.body)) return {true, 0, "", false}; // и f(x) => { require(…); … }
    scope * probe = new_scope(base);
    probe->caller = env;
    probe->fn = f;
    scope_hold hold{*this, probe};
    bind_values(fn.params, vals, args, env, probe, {}, open);
    int n = 0;
    bool unsure = false;
    for(node_id s: db.as<n_block>(fn.body).items) {
      if(!db.require_cond(s) || db.is_assume(s)) break;
      ++n;
      flow c = eval(db.require_cond(s), probe, 0);
      if(c.v.is<bool>() && !c.v.as<bool>()) return {false, n, "require не выполнено", false};
      if(!c.v.is<bool>() && c.v.known()) return {false, n, "require не bool", false};
      unsure = unsure || !c.v.is<bool>();
    }
    return {true, n, "", unsure};
  }

  // --- tail call yourself (O5, step 13) ---
  //
  // 
// 
// 
//.
  // Calling yourself in the tail position (the value of the block, the branch if, return - not in the cycle,
  // not in the call argument: there is a try / construction) - not a challenge, but a transition: a working
  // call of the same function binds the parameters with values and repeats the body; the stack does not grow.
  // *My:* parameters - values (without mut/out/own/expr, pack); call environment (O4) - original.
  std::unordered_map<uint32_t, std::unordered_set<uint32_t>> tail_calls_of; // function of its tail calls itself
  std::vector<value> tail_vals;
  // cell - in areas of the working call f (from the place of the call to the root of its body)
  static bool in_frame(array_v const * a, scope * env, node_id f) {
    for(scope * s = env; s; s = s->parent) {
      for(auto const & x: s->storage)
        if(x.is<array_v>() && &x.as<array_v>() == a) return true;
      if(s->fn == f) break;
    }
    return false;
  }
  bool tail_self(node_id f, uint32_t call) {
    auto [it, fresh] = tail_calls_of.try_emplace(f.value);
    if(fresh) {
      auto const & fn = db.as<n_fn>(f);
      bool plain = true;
      for(uint32_t k = 0; k < fn.params.size(); ++k) {
        auto const & pr = db.as<n_param>(fn.params[k]);
        if(pr.pack || (pr.mode != param_mode::let && pr.mode != param_mode::implicit) || (pr.type && db.is<n_pat>(pr.type))) plain = false;
        if(writes_param(f, k)) plain = false; // parameter changes - alias of the caller's location, not the value
      }
      auto & out = it->second;
      auto visit = [&](auto && self, node_id x, bool tail) -> void {
        if(!x) return;
        if(db.is<n_block>(x)) {
          auto const & b = db.as<n_block>(x);
          for(std::size_t i = 0; i < b.items.size(); ++i) self(self, b.items[i], tail && b.has_value && i + 1 == b.items.size());
        } else if(db.is<n_if>(x)) {
          self(self, db.as<n_if>(x).then_, tail);
          self(self, db.as<n_if>(x).else_, tail);
        } else if(db.is<n_return>(x) && !db.as<n_return>(x).target) self(self, db.as<n_return>(x).value, true);
        else if(db.is<n_call>(x) && tail && db.is<n_ident>(db.as<n_call>(x).callee) && db.as<n_ident>(db.as<n_call>(x).callee).name == fn.name && db.as<n_call>(x).words.empty())
          out.insert(x.value);
      };
      if(plain && !rest_param(f)) visit(visit, fn.body, true);
    }
    return it->second.contains(call);
  }

  flow invoke(node_id f, std::vector<value> & vals, node_list const & args, scope * env, call_site const & site) {
    if(!checking() && site.node && tail_self(f, site.node)) { // tail challenge itself - transition: values - working call
      for(value & v: vals) // The loan of the outgoing frame cell would hang - its value; the owner's loan above - lives.
        if(v.is<array_v>() && v.as<array_v>().borrowed && in_frame(v.as<array_v>().borrowed, env, f)) v.as<array_v>().own();
      tail_vals = std::move(vals);
      return flow{flow_kind::tail, {}, 0};
    }
    if(!checking()) {
      safe_point(false); // challenge
      if(!os::stack_floor) os::stack_floor = os::thread_floor();
      if(os::stack_floor && static_cast<char const *>(__builtin_frame_address(0)) < os::stack_floor + (std::size_t(2) << 20))
        fail_rt("stack_overflow", "рекурсия слишком глубокая: стек исполнения исчерпан (хвостовой вызов себя — переход, стек не растёт)", site.where);
    }
    std::unique_lock<std::recursive_mutex> svc(service_lock, std::defer_lock); // call service - under its lock (strips)
    if(lanes_on.load(std::memory_order_relaxed) && vm_root->service_fns.contains(f.value)) svc.lock();
    auto const & fn = db.as<n_fn>(f);
    scope * parent = std::exchange(invoke_parent, nullptr);
    bool open = std::exchange(invoke_open, false);
    attribute_hooks(f, vals, args, env, site);
    // own-параметр: значение уходит в функцию, имя вызывающего исчезает (явное перемещение, §5.6)
    for(uint32_t k = 0; k < fn.params.size() && k < args.size(); ++k)
      if(db.as<n_param>(fn.params[k]).mode == param_mode::own && db.is<n_ident>(args[k]) && !scalar(vals[k])) // scalar moves around
        if(value * cell = find(env, db.as<n_ident>(args[k]).name); cell && !cell->is<thunk_v>()) hide_cell(cell, node_id(site.node));
    if(fn.outline && inline_required(f))
      fail(cat(site.what, " помечена outline, но использует семантику инлайна"), db[f].where);
    if(checking() && !hook_depth && param_writes_checked.insert(f.value).second)
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
    key += open ? ", ...рт)" : ")"; // The tail of run-time - length - its instance
    if(caller_users.contains(f.value)) key += cat("@", site.node); // Takes the name of the caller - instance to the place of the call
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
    // The same function with the same types is nested deep with different known values (condition from run-time - `k ==
    // part.size`, values in parsing instance are erased): values are generalized to types - recursion converges at a
    // fixed point; in the sample - immediately (*my*: threshold 16; both the key and the values of the arguments are
    // generalized)
    std::string types_key;
    bool widen = probing;
    if(checking()) {
      types_key = active_key + "|";
      for(auto & v: vals) types_key += v.is<thunk_v>() ? cat("expr", v.as<thunk_v>().expr.value, "|") : types.describe(v) + "|";
      if(auto it = active_depth.find(types_key); it != active_depth.end() && it->second >= analysis_limit(recursion_widen)) widen = true;
      if(widen && !probing) // generalization - and the key, and values: otherwise the body considers the known
        for(auto & v: vals)
          if(!v.is<thunk_v>() && v.known()) v = erase(v);
    }
    if(checking()) {
      active_key += "|";
      // The expr-argument is the code of the caller: the nested for(…) for(…) is not
      // recursion, the internal has another block; recursion transmits the same node further.
      for(auto & v: vals)
        active_key += v.is<thunk_v>() ? cat("expr", v.as<thunk_v>().expr.value, "|") : v.known() && !widen ? show_v(v) + "|"
                                                                                                           : "·|";
      if(active.contains(active_key)) {
        recursed.insert(active_key);
        auto it = approx.find(active_key);
        return ok(it != approx.end() ? it->second : value(rt_v{}));
      }
      if(int64_t(frames.size()) > analysis_limit(recursion_depth)) fail("рекурсия не останавливается при статическом разборе", site.where);
    }

    // Reactive cache: instance with unchanged dependencies is not rediscovered
    std::string memo_key = active_key + "#" + path;
    bool memoize = checking() && cache && approx.empty() && !parent && memoizable(f, vals); // localist
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
        if(code_agg(it->second.result) && it->second.result.as<agg_v>().fields[0].second.is<int64_t>()) // Cache code - holds this parsing
          hold_tree(node_id(uint32_t(it->second.result.as<agg_v>().fields[0].second.as<int64_t>())));
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
      if(auto r = try_jit(f, vals, args, env, key)) return typed_result(f, std::move(*r));
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
    node_list const no_args;
    node_list const * argp = &args; // after the transition (tail call yourself) - parameters
    for(uint32_t pass = 0;; ++pass) {
      scope * inner = new_scope(parent ? parent : globals); // Local function - on top of the surroundings of its block
      inner->caller = env; // call environment (O4)
      inner->fn = f;
      scope_hold hold{*this, inner};
      node_list const & args = *argp;
      bind_values(fn.params, vals, args, env, inner, f, open);
      if(checking()) {
        active.insert(active_key);
        ++active_depth[types_key];
      }
      try {
        std::vector<value> escaped; // "painting": exceptions to instance
        throw_sinks.push_back(&escaped);
        try {
          r = run_frame(fn.body, inner, fn.name, fn.outline, site.frame);
          if(r.kind != flow_kind::thr && r.kind != flow_kind::tail) write_back_fields(f, fn.params, args, env, inner, site);
          if(r.kind != flow_kind::thr && r.kind != flow_kind::tail) out_written(fn.params, inner, site);
          if(res_any()) { // parameter-values (temporary caller, own) - their own: released here
            for(uint32_t k = 0; k < fn.params.size(); ++k) // a copy of the place (field to parameter) and a value without node (call from VM: drop, copy, text)
              if(k >= args.size() || db.is_member(args[k]) || db.is_index(args[k]))
                if(value * c = inner->find(db.as<n_param>(fn.params[k]).name); c && !c->hidden) c->hidden = fn.params[k].value + 1;
            if(r.kind == flow_kind::ret || r.normal())
              for(node_id pn: fn.params) // parameter returned by value - transfer
                if(r.v.is<agg_v>() && r.v.as<agg_v>().rid)
                  if(value * c = inner->find(db.as<n_param>(pn).name); c && c->is<agg_v>() && c->as<agg_v>().rid == r.v.as<agg_v>().rid) c->hidden = pn.value + 1;
            run_drops(inner, site.where);
          }
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
          --active_depth[types_key];
          approx.erase(active_key);
          recursed.erase(active_key);
        }
        throw;
      }
      if(checking()) {
        active.erase(active_key);
        --active_depth[types_key];
      }
      if(r.kind == flow_kind::tail) { // Transition: Body again, with new parameter values
        vals = std::move(tail_vals);
        argp = &no_args;
        safe_point(true); // rib-back
        continue;
      }
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
    r = typed_result(f, std::move(r));

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
        e.types_count = uint32_t(types.names.size());
        e.types_hash = types_prefix_hash(e.types_count);
        for(std::size_t i = journal_mark; i < journal.size(); ++i)
          e.journal.emplace_back(journal[i].node, journal[i].v, journal[i].seen, journal[i].diverted, journal[i].effect, journal[i].folded);
        cache->entries[memo_key] = std::move(e);
      }
    }
    return r; // return, addressed to the caller (semantic inlining), goes further
  }

  // f() => {x: …} is the result of a function with a type: the anonymous literal becomes it.
  flow typed_result(node_id f, flow r) {
    if(node_id rt = db.as<n_fn>(f).result_type; r.normal() && rt && db.is<n_ident>(rt)) r.v = to_type(std::move(r.v), str(db.as<n_ident>(rt).name), db[rt].where);
    return r;
  }

  std::optional<flow> try_jit(node_id f, std::vector<value> & vals, node_list const & args, scope * env,
      std::string const & key);

  bool valid(check_cache::entry const & e) {
    for(auto const & [node, rev]: e.deps)
      if(db.node_revision(node_id(node)) != rev) return false;
    return e.types_count <= types.names.size() && types_prefix_hash(e.types_count) == e.types_hash;
  }
  std::unordered_map<uint32_t, uint64_t> types_prefix; // hash of the first n registry names (the registry is only growing)
  uint64_t types_prefix_hash(uint32_t n) {
    if(auto it = types_prefix.find(n); it != types_prefix.end()) return it->second;
    uint64_t h = 1469598103934665603ull;
    for(uint32_t i = 0; i < n; ++i) {
      for(char c: types.nm(i)) h = (h ^ uint8_t(c)) * 1099511628211ull;
      h = (h ^ 0xff) * 1099511628211ull;
    }
    return types_prefix[n] = h;
  }

  // Parameters: alias per caller cell, if the argument is a place; otherwise its own cell. open
  // - tail run-time-length (parsing f(...t)): ...xs - erased array
  void bind_values(node_list const & params, std::vector<value> & vals, node_list const & args, scope * env, scope * inner, node_id f = {}, bool open = false) {
    for(uint32_t i = 0; i < params.size(); ++i) {
      auto const & p = db.as<n_param>(params[i]);
      if(p.pack && p.mode == param_mode::let && i + 1 == params.size()) { // ...xs - the remainder of arguments (cortege)
        std::vector<value> rest;
        for(uint32_t j = i; j < vals.size(); ++j) rest.push_back(vals[j]);
        if(open && !rest.empty()) {
          value e = erase(rest[0]);
          for(std::size_t j = 1; j < rest.size(); ++j) e = merge(e, erase(rest[j]));
          inner->declare(p.name, erase(value(array_v::of({e}))));
          break;
        }
        inner->declare(p.name, value(array_v::of(std::move(rest))));
        break;
      }
      if(p.type && db.is<n_pat>(p.type)) { // parameter-sample: names - from the unpacking of the argument
        unpack_into(p.type, vals[i], inner, inner, 0, db[params[i]].where);
        continue;
      }
      // array element to variable parameter - reference to element, not copy
      if(f && i < args.size() && p.mode != param_mode::expr && p.mode != param_mode::own && db.is_index(args[i]) && writes_param(f, i))
        if(auto r = element_ref(args[i], env, 0, args[i])) {
          inner->declare(p.name, std::move(*r));
          continue;
        }
      if(p.mode == param_mode::out && i < args.size() && db.is<n_ident>(args[i])) // The place of the result is the name of the caller.
        if(value * cell = find(env, db.as<n_ident>(args[i]).name)) {
          inner->alias(p.name, cell);
          continue;
        }
      if(p.mode != param_mode::expr && p.mode != param_mode::own && !vals[i].is<split_v>()) {
        // An argument converted to a parameter type (i64 → f64, {x: …} → struct) is another
        // value: not alias of the caller’s location.
        if(value * cell = i < args.size() ? place_or_null(args[i], env) : nullptr; cell && !cell->is<split_v>() && (cell->is<ref_v>() || types.of(*cell) == types.of(vals[i]))) {
          inner->alias(p.name, cell);
          if(res_any() && has_resource(*cell)) inner->borrows.push_back({cell, args[i], env});
          continue;
        }
      }
      inner->declare(p.name, args.empty() && vals[i].is<array_v>() ? std::move(vals[i]) : vals[i]); // after the transfer - a loan from the owner, without a copy
    }
  }

  // Field to variable parameter (§6.2: "parameter - alias", the change is output): copy at the input,
  // write in the caller field at the output. *My:* is observed as a alias place, but without reference
  // to the inside of the aggregate, which would hang if the summoned (through the environment of the
  // caller, O4) replaces the aggregate as a whole - "no UB" (§3.5); when excluded - does not write.
  void write_back_fields(node_id f, node_list const & params, node_list const & args, scope * env, scope * inner, call_site const & site) {
    for(uint32_t i = 0; i < params.size() && i < args.size(); ++i) {
      auto const & p = db.as<n_param>(params[i]);
      if(p.mode == param_mode::expr || p.mode == param_mode::own || p.pack || (p.type && db.is<n_pat>(p.type))) continue;
      if(!db.is_member(args[i]) || !writes_param(f, i)) continue;
      value * now = inner->find(p.name);
      if(!now || now->is<thunk_v>()) continue;
      value v = *now;
      flow w = assign(args[i], "=", std::move(v), env, site.frame, db[args[i]].where);
      if(!w.normal()) fail("запись поля-аргумента после вызова не завершилась", db[args[i]].where);
    }
  }

  value * place_or_null(node_id n, scope * env) {
    if(!db.is<n_ident>(n)) return nullptr;
    value * c = find(env, db.as<n_ident>(n).name);
    if(c) not_hidden(*c, db.as<n_ident>(n).name, db[n].where);
    return c && !c->is<thunk_v>() ? c : nullptr;
  }

  // The function uses inline semantics (§6.1): expr-parameter or address return.
  bool inline_required(node_id f) const {
    for(node_id p: db.as<n_fn>(f).params)
      if(db.as<n_param>(p).mode == param_mode::expr) return true;
    return has_addressed_return(db.as<n_fn>(f).body);
  }

  // overload operator, not built-in primitives; remembered by the symbol (reconciliation - by the
  // number of overloads: editing adds or removes them)
  mutable std::unordered_map<uint32_t, std::pair<std::vector<node_id>, std::vector<node_id>>> user_cands; // overload is not primitive
  // User overload operator (not built-in primitives) and their features - one entry per symbol,
  // reconciliation - by the number of overloads (they are only added by registration)
  struct user_op_info {
    std::size_t from = ~std::size_t{0};
    std::vector<node_id> cands;
    bool lazy = false; // There is an expr-parameter (lazy operands)
    bool scalar_params = false; // have a built-in scalar type parameter or no type
    std::vector<node_id> const * list = nullptr; // overloading
  };
  mutable std::deque<user_op_info> user_ops_memo; // by character number (addresses are stable)
  user_op_info const & user_info(symbol f) const {
    static user_op_info const none;
    if(f.value >= user_ops_memo.size()) user_ops_memo.resize(f.value + 1);
    auto & u = user_ops_memo[f.value];
    if(!u.list) { // node map does not move, records are not deleted: the address of the congestion list - once
      auto it = functions.find(f);
      if(it == functions.end()) return none;
      u.list = &it->second;
    }
    if(u.from != u.list->size()) {
      std::vector<node_id> const * list = u.list;
      u = {list->size(), {}, false, false, list};
      for(node_id fn: *list) {
        if(intrinsic_operator(fn)) continue;
        u.cands.push_back(fn);
        for(node_id p: db.as<n_fn>(fn).params) {
          auto const & pp = db.as<n_param>(p);
          u.lazy = u.lazy || pp.mode == param_mode::expr;
          u.scalar_params = u.scalar_params || !pp.type || !db.is<n_ident>(pp.type) || types.builtin(str(db.as<n_ident>(pp.type).name));
        }
      }
    }
    return u;
  }
  std::vector<node_id> const & user_candidates(symbol f) const { return user_info(f).cands; }

  // Does the user overload operator accept such types of operands? Does the operator’s custom
  // overload apply to values: In built-in scalars, the response to type numbers is remembered (f64 +
  // f64 with overload i32 - no strings)
  std::unordered_map<uint64_t, bool> user_applies_memo;
  bool user_applies(symbol f, std::vector<value> const & vals) {
    bool scalar_only = vals.size() <= 2;
    uint64_t key = uint64_t(f.value) << 40;
    for(std::size_t k = 0; k < vals.size() && scalar_only; ++k) {
      value const & v = vals[k];
      if(!(v.is<int64_t>() || v.is<double>() || v.is<bool>() || v.is<str_v>())) scalar_only = false;
      else key |= uint64_t(types.of(v).value & 0xFFFFF) << (20 * k);
    }
    if(scalar_only)
      if(auto it = user_applies_memo.find(key); it != user_applies_memo.end()) return it->second;
    std::vector<std::string> ts;
    for(auto const & v: vals) ts.push_back(types.describe(v));
    bool r = user_operator_applies(f, ts);
    if(scalar_only) user_applies_memo[key] = r;
    return r;
  }
  // redefinition of context (checked { … }) for the operator f over the calculated values
  std::optional<flow> apply_override(symbol f, std::vector<value> & vals, n_op const & o, node_id n, scope * env, uint32_t frame, span where) {
    if(overrides.empty() || override_depth != 0 || o.args.size() != 2) return std::nullopt;
    auto op = str(o.op);
    if(op == "&&" || op == "||" || (db.sym_kind(o.op) & ast_db::sc_assign)) return std::nullopt;
    for(auto it = overrides.rbegin(); it != overrides.rend(); ++it) {
      if(it->entity != f) continue;
      auto const & ps = db.as<n_fn>(it->fn).params;
      bool fits = true;
      for(std::size_t k = 0; k < ps.size() && fits; ++k) {
        auto const & p = db.as<n_param>(ps[k]);
        if(p.type && db.is<n_ident>(p.type) && str(db.as<n_ident>(p.type).name) != types.describe(vals[k])) fits = false;
      }
      if(!fits) continue;
      ++override_depth;
      struct depth_hold {
        uint32_t & d;
        ~depth_hold() { --d; }
      } dh{override_depth};
      return dispatch_split({it->fn}, vals, 0, o.args, env, frame, {where, std::string(str(f)), n.value, frame});
    }
    return std::nullopt;
  }
  bool user_operator_applies(symbol f, std::vector<std::string> const & ts) const {
    for(node_id fn: user_candidates(f)) {
      auto const & ps = db.as<n_fn>(fn).params;
      bool fits = ps.size() == ts.size();
      for(std::size_t k = 0; fits && k < ps.size(); ++k) {
        auto const & p = db.as<n_param>(ps[k]);
        // parameter type: type, family (array accepts array<i64>) or base:
// overload
        if(p.type && db.is<n_ident>(p.type) && str(db.as<n_ident>(p.type).name) != ts[k] && str(db.as<n_ident>(p.type).name) != std::string_view(ts[k]).substr(0, ts[k].find('<')) && !types.is_a(ts[k], str(db.as<n_ident>(p.type).name))) fits = false;
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

  // call (at any depth of node) that changes the location-argument: the mut-parameter of the name
  // closure function around node assigns (captured can change): local function or lambda in the
  // variable, in the body - assignment; name not found as a function - too yes
  bool local_writes(node_id n, symbol name) const {
    auto assigns = [&](auto && self, node_id c) -> bool {
      if(!c) return false;
      if(db.is<n_op>(c)) {
        auto op = db.strings.text(db.as<n_op>(c).op);
        if(op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") return true;
      }
      if(db.is<n_call>(c) && writes_args(c)) return true;
      bool w = false;
      db.for_each_child(c, [&](node_id x) { w = w || self(self, x); });
      return w;
    };
    for(node_id c = n, p = db[n].parent; p; c = p, p = db[p].parent) {
      if(db.is<n_block>(p))
        for(node_id s: db.as<n_block>(p).items) {
          if(s == c) break;
          if(db.is<n_fn>(s) && db.as<n_fn>(s).name == name) return assigns(assigns, db.as<n_fn>(s).body);
          if(db.is<n_op>(s) && db.strings.text(db.as<n_op>(s).op) == "=" && db.is<n_ident>(db.as<n_op>(s).args[0]) && db.as<n_ident>(db.as<n_op>(s).args[0]).name == name)
            return !db.is<n_lambda>(db.as<n_op>(s).args[1]) || assigns(assigns, db.as<n_lambda>(db.as<n_op>(s).args[1]).body);
        }
      if(db.is<n_module>(p)) break;
    }
    return local_name(n, name); // parameter-function and other - unknown: yes
  }

  // name declared lexically around node: variable, parameter, local function
  bool local_name(node_id n, symbol name) const {
    for(node_id c = n, p = db[n].parent; p; c = p, p = db[p].parent) {
      if(db.is<n_block>(p))
        for(node_id s: db.as<n_block>(p).items) {
          if(s == c) break;
          if(db.is<n_var>(s) && db.as<n_var>(s).name == name) return true;
          if(db.is<n_fn>(s) && db.as<n_fn>(s).name == name) return true;
          if(db.is<n_op>(s) && db.strings.text(db.as<n_op>(s).op) == "=" && db.is<n_ident>(db.as<n_op>(s).args[0]) && db.as<n_ident>(db.as<n_op>(s).args[0]).name == name) return true;
        }
      if(db.is<n_fn>(p) || db.is<n_lambda>(p)) {
        node_list const & ps = db.is<n_fn>(p) ? db.as<n_fn>(p).params : db.as<n_lambda>(p).params;
        for(node_id q: ps)
          if(db.as<n_param>(q).name == name) return true;
      }
      if(db.is<n_module>(p)) break;
    }
    return false;
  }

  // Through the node that counts VM (service 8), the output return(f) / return(caller) of the called
  // function can pass: the target is in machine code, in the VM chain it is not. Calls and subtree
  // operators are by name (all overload); the operator's frame is transparent (x? - try(x)), and the
  // output of his calls also passes.
  bool exits_through(node_id c, int depth = 0) const {
    bool r = false;
    auto see = [&](auto & self, node_id x) -> void {
      if(r || db.is<n_fn>(x)) return; // The attached function - its return
      symbol callee{};
      if(db.is<n_call>(x) && db.is<n_ident>(db.as<n_call>(x).callee)) callee = db.as<n_ident>(db.as<n_call>(x).callee).name;
      else if(db.is<n_op>(x)) callee = entity_of(db.as<n_op>(x));
      if(callee)
        if(auto it = functions.find(callee); it != functions.end())
          for(node_id f: it->second)
            if(returns_out(f, depth)) {
              r = true;
              return;
            }
      db.for_each_child(x, [&](node_id y) { self(self, y); });
    };
    see(see, c);
    return r;
  }
  bool returns_out(node_id f, int depth) const {
    auto const & fn = db.as<n_fn>(f);
    bool r = false;
    auto see = [&](auto & self, node_id x) -> void {
      if(r || db.is<n_fn>(x)) return;
      if(db.is<n_return>(x) && db.as<n_return>(x).target) r = true;
      else db.for_each_child(x, [&](node_id y) { self(self, y); });
    };
    if(fn.body) see(see, fn.body);
    return r || (depth < 4 && operator_name(str(fn.name)) && fn.body && exits_through(fn.body, depth + 1));
  }

  bool writes_args(node_id n) const {
    if(!n) return false;
    if(db.is<n_call>(n) && !db.is<n_ident>(db.as<n_call>(n).callee)) return true; // Significance Challenge - Not Knowing What Changes
    if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) {
      auto it = functions.find(db.as<n_ident>(db.as<n_call>(n).callee).name);
      if(it == functions.end() && local_writes(n, db.as<n_ident>(db.as<n_call>(n).callee).name)) return true; // Circuit that assigns: changes the captured
      if(it != functions.end())
        for(node_id fn: it->second)
          for(uint32_t i = 0; i < db.as<n_fn>(fn).params.size(); ++i)
            if(writes_param(fn, i)) return true;
    }
    bool w = false;
    db.for_each_child(n, [&](node_id c) { w = w || writes_args(c); });
    return w;
  }

  bool user_operator(node_id n) const { // node operator declared code (not primitive)
    if(!db.is<n_op>(n)) return false;
    auto const & o = db.as<n_op>(n);
    if(o.postfix) return !db.is_member(n);
    if(user_ops.empty() && local_ops.empty()) return false;
    uint32_t f = db.strings.intern(cat(o.args.size() == 2 ? "infix" : "prefix", str(o.op))).value;
    return user_ops.contains(f) || local_ops.contains(f);
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
      {"print", "(x)"}, {"process_class", "()"}, {"granted", "(вид)"}, {"read_floats", "(stdin)"}, {"range", "(n)"},
      {"typeof", "(x)"}, {"fields", "(T)"}, {"attrs", "(f)"}, {"emit", "(quote)"}, {"static_level", "(f)"}, {"code", "(x)"}, {"code_kind", "(c)"}, {"code_items", "(c)"}, {"code_name", "(c)"}, {"code_text", "(c)"}, {"code_fields", "(c)"}, {"given", "(x)"}, {"op_codegen", "(цель, узел, типы)"}, {"type_of_code", "(c)"}, {"type_family", "(t)"}, {"code_id", "(c)"}, {"code_parent", "(c)"}, {"code_module", "(c)"}, {"functions_named", "(имя)"}, {"kind_functions", "(узел, имя)"}, {"inline_context", "(ctx, c)"}, {"jit_type", "(c, ctx)"}, {"jit_proven", "(c)"}, {"jit_loop_keeps", "(цикл, c)"}, {"jit_ref_keeps", "(ссылка)"}, {"jit_const", "(c, ctx)"}, {"jit_known", "(c, ctx)"}, {"jit_implicit_args", "(вызов)"}, {"jit_tail_self", "(f, вызов)"}, {"jit_diverts", "(c)"}, {"jit_chain", "(c, ctx)"}, {"f64_bits", "(x)"}, {"zero", "(T)"}, {"ct_state", "(x, иначе)"}, {"set_ct_state", "(x, состояние)"}, {"call_site", "()"}, {"code_parse", "(подблок)"}, {"code_glued", "(атом)"}, {"code_comment", "(c)"}, {"code_declaration", "(имя)"}, {"code_targets", "(вызов)"}, {"code_users", "(объявление)"}, {"code_callers", "(функция)"}, {"code_calls", "(c)"}, {"code_deps", "(c)"}, {"code_instances", "(функция)"}, {"code_types", "(c)"}, {"code_values", "(c)"}, {"code_replace", "(c, новый)"}, {"code_rename", "(объявление, имя)"}, {"code_remove", "(c)"}, {"code_insert", "(блок, место, c)"},
      {"int_to_float", "(x)"}, {"f32_round", "(x)"}, {"float_to_int", "(x)"}, {"not", "(x)"}};

  // parsing instance compiler questions (jit_type, jit_const...) and answers - runtime - part of
  // instance at the language level (deduplication, §10.3):
// 
//.
  std::vector<jit_query> * jit_trace = nullptr;
  static bool jit_question(std::string_view n) {
    return n == "jit_type" || n == "jit_const" || n == "jit_known" || n == "jit_proven" || n == "jit_loop_keeps" || n == "jit_chain" || n == "jit_ref_keeps";
  }
  std::string jit_answer(value const & v) {
    if(code_agg(v)) return v.as<agg_v>().fields[0].second.is<int64_t>() ? cat("c", v.as<agg_v>().fields[0].second.as<int64_t>()) : std::string("c");
    if(v.is<array_v>()) {
      std::string s = "[";
      for(uint32_t i = 0; i < v.as<array_v>().size(); ++i) s += jit_answer(v.as<array_v>().get(i)) + ",";
      return s + "]";
    }
    if(v.is<type_v>()) return cat("t:", types.name(v.as<type_v>().type));
    return cat(types.describe(v), "=", v.known() ? show_v(v) : std::string("?"));
  }
  // Same questions to this parsing: answers match - runtime - part same
  bool jit_replay(std::vector<jit_query> const & qs) {
    for(auto const & q: qs) {
      std::vector<value> a;
      for(auto const & [k, x]: q.args) a.push_back(k == 'c' ? code_value(node_id(uint32_t(x))) : value(x));
      if(jit_answer(builtin(sym(q.name), a, {})) != q.answer) return false;
    }
    return true;
  }

  // Print meanings: your type has text(T x) i32 is a number, not i32{value: 5} *Mine: * Only overload
  // exactly this type.
  std::string print_text(value const & v) {
    if(v.is<agg_v>() && !types.anonymous(v.as<agg_v>().type))
      if(auto it = functions.find(sym("text")); it != functions.end()) {
        std::string_view tn = types.name(v.as<agg_v>().type);
        for(node_id f: it->second) {
          auto const & fn = db.as<n_fn>(f);
          if(fn.params.size() != 1) continue;
          node_id t = db.as<n_param>(fn.params[0]).type;
          if(!t || !db.is<n_ident>(t) || str(db.as<n_ident>(t).name) != tn) continue;
          std::vector<value> vals{v};
          flow r = invoke(f, vals, {}, globals, {db[f].where, "text", 0, 0});
          if(r.normal() && r.v.is<str_v>()) return std::string(r.v.as<str_v>().str());
        }
      }
    return show_v(v);
  }

  value builtin(symbol name, std::vector<value> & a, span where) {
    auto n = str(name);
    if(jit_trace && jit_question(n)) {
      auto * t = std::exchange(jit_trace, nullptr);
      value r = builtin(name, a, where);
      jit_trace = t;
      jit_query q{std::string(n), {}, jit_answer(r)};
      for(auto const & x: a) q.args.push_back(code_agg(x) ? std::pair{'c', int64_t(code_node(x, where).value)} : std::pair{'i', x.is<int64_t>() ? x.as<int64_t>() : int64_t(0)});
      t->push_back(std::move(q));
      return r;
    }
    // process class (§9.5): monopoly - program (main band), background - process(…); parsing - run-time
    // (one parsing for both classes)
    if(n == "granted" && a.size() == 1 && a[0].is<str_v>()) { // Whether the resource (fs, net) is available
      bool g = granted(a[0].as<str_v>().view());
      return checking() ? erase(value(g)) : value(g);
    }
    if(n == "process_class" && a.empty()) {
      std::string k = background ? "фоновый" : "монопольный";
      return checking() ? erase(value(k)) : value(k);
    }
    if(n == "print") {
      rt_effect();
      if(checking()) return {};
      std::string line;
      for(uint32_t i = 0; i < a.size(); ++i) line += (i ? " " : "") + print_text(a[i]);
      lane_guard g(out_lock); // line
      if(captured) *captured += line + "\n";
      else put(stdout, cat(line, "\n"));
      return {};
    }
    // --- entity compiler as a value (§7.4) -----------------------------------------------
    // operations of the base language over the state of the compiler-interpreter, and not
    // host calls "down": "Here in cp this is exactly what part of the program ... because in
    // the cp in the database there is the integration of conditional api into the language."
    if(n == "emit" && a.size() == 1) { // Advertisements from quote or ad node - to current elaboration
      node_id q = code_node(a[0], where);
      if(!elaborating && ct_depth && !deriving) { // ct-code instance (§7.4): ads - module elements
        if(!instantiating && done_by_pass(where)) return value();
        if(instantiating) {
          node_list items;
          if(db.is<n_quote>(q)) items = db.as<n_quote>(q).items;
          else if(db.is<n_fn>(q) || db.is<n_extend>(q)) items = {q};
          else fail("emit: ожидался quote { объявления } или узел объявления (n_fn, n_extend)", where);
          auto m = db.as<n_module>(mod);
          for(node_id item: items) {
            register_item(item);
            m.items.push_back(item);
          }
          code_changes.push_back({db[ct_source].where, stack.empty() ? std::string("модуль") : stack.back().what, cat("emit: ", items.size(), " объявл.")});
          db.mutate(mod, std::move(m), ct_source.value + 1);
          return value();
        }
      }
      if(db.is<n_quote>(q))
        for(node_id item: db.as<n_quote>(q).items) {
          register_item(item);
          emitted.push_back(item);
        }
      else if(db.is<n_fn>(q) || db.is<n_extend>(q)) { // n_fn {…}, n_extend {…} - generated by the ct code
        register_item(q);
        emitted.push_back(q);
      } else fail("emit: ожидался quote { объявления } или узел объявления (n_fn, n_extend)", where);
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
          for(node_id x: db.as<n_extend>(it->second).attrs) nodes.push_back(x);
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
      if(types.anonymous(a[0].as<type_v>().type)) { // Anonymous: fields - from type recording
        std::vector<value> vs;
        for(auto const & [name, t]: record_fields(a[0].as<type_v>().type)) {
          agg_v d{types.named("field"), {}};
          d.fields.push_back({sym("name"), value(name)});
          d.fields.push_back({sym("type"), value(type_v{t})});
          vs.push_back(value(std::move(d)));
        }
        return value(array_v::of(std::move(vs)));
      }
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
    if(n == "static_level" && a.size() == 1 && a[0].is<fn_v>() && a[0].as<fn_v>().name) return static_level(a[0].as<fn_v>().name, where);
    if(n == "f64_bits" && a.size() == 1 && types.describe(a[0]) == types.float_name()) // bits f64 - i64 (constant in machine code)
      return a[0].known() ? value(std::bit_cast<int64_t>(a[0].as<double>())) : erase(value(int64_t(1)));
    if(n == "given" && a.size() == 1) { // node field is not () and is not an empty node
      value const * x = shape_of(a[0]);
      if(x && x->is<agg_v>() && types.name(x->as<agg_v>().type) == "code" && !x->as<agg_v>().fields.empty()) {
        value const & nd = x->as<agg_v>().fields[0].second;
        if(!a[0].known() || !nd.known()) return erase(value(true));
        return value(node_id(uint32_t(nd.as<int64_t>())).valid());
      }
      return value(!a[0].is<unit_v>());
    }
    // a type that names a code type (type name in a program); () is not a type
    if(n == "type_of_code" && a.size() == 1) {
      node_id c = code_node(a[0], where);
      if(!db.is<n_ident>(c)) return value();
      auto name = str(db.as<n_ident>(c).name);
      if(auto t = types.builtin(name)) return value(type_v{t});
      return value(type_v{types.named(name)}); // struct, VM error, family (stream)
    }
    // family of parameterized type: stream(i64) → stream
    if(n == "type_family" && a.size() == 1) {
      if(!a[0].is<type_v>()) return value();
      auto name = types.name(a[0].as<type_v>().type);
      return value(type_v{types.named(name.substr(0, name.find('<')))});
    }
    // parameterized type - function over types:
//  
    if((n == "array" || n == "stream_type") && a.size() == 1 && a[0].is<type_v>())
      return value(type_v{types.named(cat(n == "array" ? "array" : "stream", "<", types.name(a[0].as<type_v>().type), ">"))});
    // cogenerator of node operator e in the context of the targetcodegen::eval(a + b),
    // author ): entity - operator of the node, operand types - values-types; () - no
    if(n == "op_codegen" && a.size() == 3 && a[0].is<fn_v>() && a[2].is<array_v>()) {
      node_id e = code_node(a[1], where);
      if(!db.is<n_op>(e)) fail("op_codegen: ожидался узел оператора", where);
      auto const & o = db.as<n_op>(e);
      std::string sym(str(o.op));
      if(sym == "+=" || sym == "-=" || sym == "*=" || sym == "/=") sym.pop_back(); // a op = b - cogenerator op
      std::string entity = cat(o.args.size() == 2 ? "infix" : o.postfix ? "postfix" : "prefix", sym);
      std::vector<std::string> names;
      for(uint32_t k = 0; k < a[2].as<array_v>().size(); ++k) {
        value t = a[2].as<array_v>().get(k);
        if(!t.is<type_v>()) fail("op_codegen: типы — значения-типы", where);
        names.push_back(std::string(types.name(t.as<type_v>().type)));
      }
      fn_v const & cg = a[0].as<fn_v>();
      // Enclosed context of the target (cg.unless) - code generator from his body
      auto [s, ctx_fn] = cg.name ? find_codegen(db, str(cg.name), entity, names) : codegen_in(db, cg.lambda, entity, names);
      if(!s) return value();
      return value(fn_v{{}, s, globals, {}}); // node call: parameters - target registers
    }
    // name congestion - code declarations (reflection as members of) member functions of the
    // program node type (ast.dbl {{}) eval(…) ) by name, for node c
    if(n == "kind_functions" && a.size() == 2 && a[1].is<str_v>()) {
      std::vector<value> xs;
      node_id c = code_node(a[0], where);
      if(db.is<n_node>(c))
        if(auto it = kind_members.find(sym(db.as<n_node>(c).kind)); it != kind_members.end())
          for(node_id f: it->second)
            if(str(db.as<n_fn>(f).name) == a[1].as<str_v>().view()) xs.push_back(code_value(f));
      return value(array_v::of(std::move(xs)));
    }
    if(n == "functions_named" && a.size() == 1 && a[0].is<str_v>()) {
      std::vector<value> xs;
      if(auto it = functions.find(sym(a[0].as<str_v>())); it != functions.end())
        for(node_id f: it->second) xs.push_back(code_value(f));
      return value(array_v::of(std::move(xs)));
    }
    // body context of a function embedded in a c node from the ctx context
    if(n == "inline_context" && a.size() == 2 && a[0].is<int64_t>())
      return value(int64_t(inline_context(uint64_t(a[0].as<int64_t>()), code_node(a[1], where).value)));
    // brute-force (M6, step 4): type of application of operator m to types l, r is type number or
    // -1 (not instance); line type to compare parsing and messages
    if((n == "chain_probe" || n == "chain_key") && chain_subject && !chain_subject->chain_jobs.empty()) {
      auto & job = *chain_subject->chain_jobs.back();
      auto num = [&](uint32_t i) { return a[i].is<int64_t>() ? a[i].as<int64_t>() : -1; };
      if(n == "chain_key" && a.size() == 1 && num(0) >= 0 && uint64_t(num(0)) < job.shapes.size())
        return value(chain_subject->types.describe(job.shapes[uint64_t(num(0))]));
      if(n == "chain_probe" && a.size() == 3 && num(0) >= 0 && uint64_t(num(0)) < job.ops.size() && num(1) >= 0 && num(2) >= 0) {
        value l = job.shapes[uint64_t(num(1))], r = job.shapes[uint64_t(num(2))];
        auto res = chain_subject->probe_apply(*job.p, job.ops[uint64_t(num(0))], l, r, job.why);
        if(!res) return value(int64_t(-1));
        job.shapes.push_back(std::move(*res));
        return value(int64_t(job.shapes.size() - 1));
      }
    }
    // Observation parsing for representation in language (M6, step 3b): node obs prelude (executed,
    // gave value, effect, left through return/break, the value is known and its literal, the
    // condition is bool); function - whether the program is needed; x = e - declared the name
    if(n.starts_with("view_") && a.size() == 1 && view_subject) {
      node_id c = code_node(a[0], where);
      if(n == "view_live") return value(view_subject->live(c));
      if(n == "view_declares") return value(view_subject->declares(c));
      if(n == "view_obs") {
        observation const * o = view_subject->observed(c);
        auto lit = o ? view_subject->literal(o->v) : std::nullopt;
        agg_v r{types.named("obs"), {}};
        auto field = [&](std::string_view k, value x) { r.fields.push_back({sym(k), std::move(x)}); };
        field("found", value(o != nullptr));
        field("present", value(o && o->present));
        field("seen", value(o && o->seen));
        field("effect", value(o && o->effect));
        field("diverted", value(o && o->diverted));
        field("known", value(o && o->v.known()));
        field("has_literal", value(lit.has_value()));
        field("literal", value(lit ? *lit : std::string()));
        field("is_bool", value(o && o->v.is<bool>()));
        field("truth", value(o && o->v.is<bool>() && o->v.as<bool>()));
        return value(std::move(r));
      }
    }
    // indexation of node c is proved by parsing instance within bounds (in all contexts): index -
    // range(xs.size) gives indexes of xs; no verification is required
    if(n == "jit_proven" && a.size() == 1) return value(jit_analysis && jit_analysis->bounds_proven(code_node(a[0], where)));
    // The node array indexing c for the entire cycle l did not change completely (replacement, growth)
    // - according to journal records parsing:
// parsing fact, not the compiler's guess; its data and size - once before the cycle
    if(n == "jit_loop_keeps" && a.size() == 2) return value(jit_analysis && jit_analysis->loop_keeps(code_node(a[0], where), code_node(a[1], where)));
    // link decl (&e = xs[k]): the array did not change completely during her life - the element is the same
    if(n == "jit_ref_keeps" && a.size() == 1) return value(jit_analysis && jit_analysis->ref_keeps(code_node(a[0], where)));
    // node type in the context of parsing instance embedding - value-type; () - not executed
    if(n == "jit_type" && a.size() == 2 && a[1].is<int64_t>()) {
      if(!jit_analysis) return value();
      observation const * o = jit_analysis->observed(code_node(a[0], where), uint64_t(a[1].as<int64_t>()));
      if(!o || !o->seen) return value();
      std::string d = jit_analysis->types.describe(o->v);
      // flow: element type - from parsing (execution - parsing of the entire program), as in gluing C + +
      if(value const * x = shape_of(o->v); x && x->is<agg_v>() && jit_analysis->types.name(x->as<agg_v>().type) == "stream" && !x->as<agg_v>().fields.empty()) {
        auto const & fs = x->as<agg_v>().fields;
        value const & id = fs[0].second;
        auto const & ss = jit_analysis->vm_root->streams;
        value shape;
        if(id.is<int64_t>() && id.as<int64_t>() >= 0 && uint64_t(id.as<int64_t>()) < ss.size() && ss[std::size_t(id.as<int64_t>())].typed) shape = ss[std::size_t(id.as<int64_t>())].shape;
        else if(fs.size() > 1) shape = fs[1].second;
        if(shape.known() || shape.is<rt_v>()) d = cat("stream<", jit_analysis->types.describe(shape), ">");
      }
      return value(type_v{types.named(d)});
    }
    // Data offsets and the size of the dense array in the VM value cell (§12.2): machine code reads
    // them from memory
    if(n == "region" && a.empty()) { // New Memory Level: Your Range of Addresses
      if(checking()) { // level - resource: its release is conducted by the interpreter (JIT does not take such instance)
        ++regions_made;
        ++resources_made;
        return erase(value(agg_v{types.named("region"), {{sym("id"), value(int64_t(0))}}}));
      }
      vm_root->regions.emplace_back();
      vm_root->regions.back().holders = 1;
      agg_v g{types.named("region"), {{sym("id"), value(int64_t(vm_root->regions.size() - 1))}}};
      g.rid = ++res_counter; // level - resource: holders are considered, the latter releases
      res_live[g.rid] = 1;
      vm_root->regions_alive = true;
      return value(std::move(g));
    }
    if(n == "jit_agg_layout" && a.empty()) return value(array_v::of({value(int64_t(agg_layout().data)), value(int64_t(agg_layout().stride)), value(int64_t(agg_layout().scalar))}));
    // field number in type t aggregate (order of type fields: base, declaration, extensions); -1 - no
    if(n == "field_index" && a.size() == 2 && a[0].is<type_v>() && a[1].is<str_v>()) {
      auto fs = fields_of_struct(sym(types.name(a[0].as<type_v>().type)));
      for(uint32_t k = 0; k < fs.size(); ++k)
        if(str(db.as<n_var>(fs[k]).name) == a[1].as<str_v>().view()) return value(int64_t(k));
      return value(int64_t(-1));
    }
    if(n == "jit_resource" && a.size() == 1) { // Type-resource (destroy, shared, array of resources): machine code releases it with VM
      if(!a[0].is<type_v>() || !res_any()) return value(false);
      return value(resource_type(types.name(a[0].as<type_v>().type)));
    }
    if(n == "jit_literal_place" && a.size() == 1) { // type-aggregate place anonymous literature (or ')
      std::string t = literal_place(code_node(a[0], where));
      return value(aggregate_struct(t) ? t : std::string());
    }
    if(n == "jit_str_layout" && a.empty()) { // the string in the value cell: data (bytes) and length (32 bits)
      static std::array<int64_t, 2> const off = [] {
        value v{str_v{}};
        auto const * b = reinterpret_cast<char const *>(&v);
        auto const & x = v.as<str_v>();
        return std::array<int64_t, 2>{int64_t(reinterpret_cast<char const *>(&x.w.data) - b), int64_t(reinterpret_cast<char const *>(&x.len) - b)};
      }();
      return value(array_v::of({value(off[0]), value(off[1])}));
    }
    if(n == "jit_layout" && a.empty()) return value(array_v::of({value(int64_t(array_layout().data)), value(int64_t(array_layout().size)), value(int64_t(array_layout().cap)), value(int64_t(array_layout().gen)), value(int64_t(array_layout().items_n))}));
    // the value of node if parsing knows it and the computation is clean (convolution "top"); empty - no
    if(n == "jit_const" && a.size() == 2 && a[1].is<int64_t>()) {
      observation const * o = jit_analysis ? jit_analysis->observed(code_node(a[0], where), uint64_t(a[1].as<int64_t>())) : nullptr;
      if(!o || !o->seen || o->effect || o->throws || o->diverted || !o->v.known() || !(o->v.is<int64_t>() || o->v.is<double>() || o->v.is<bool>())) return value(array_v::of({}));
      if(writes_args(code_node(a[0], where))) return value(array_v::of({})); // recording in the place of the caller is not a constant
      return value(array_v::of({o->v}));
    }
    // the value of node parsing knows completely, without effects (call without run-time - inputs
    // folded - the body in this place is not disassembled): the value of VM is not a scalar, it is
    // considered VM node call c - tail call itself function f (O5): transition, not call
    if(n == "jit_tail_self" && a.size() == 2) return value(tail_self(code_node(a[0], where), code_node(a[1], where).value));
    // Implicit call arguments (O9) are the same node as parsing instance
    if(n == "jit_implicit_args" && a.size() == 1) {
      std::vector<value> xs;
      node_id c = code_node(a[0], where);
      interpreter * an = jit_analysis ? const_cast<interpreter *>(jit_analysis) : this;
      if(db.is<n_call>(c) && db.is<n_ident>(db.as<n_call>(c).callee))
        if(auto it = an->functions.find(db.as<n_ident>(db.as<n_call>(c).callee).name); it != an->functions.end()) {
          node_list const & full = an->with_implicit(it->second, db.as<n_call>(c).args, c.value);
          for(std::size_t i = db.as<n_call>(c).args.size(); i < full.size(); ++i) xs.push_back(code_value(full[i]));
        }
      return value(array_v::of(std::move(xs)));
    }
    if(n == "jit_known" && a.size() == 2 && a[1].is<int64_t>()) {
      observation const * o = jit_analysis ? jit_analysis->observed(code_node(a[0], where), uint64_t(a[1].as<int64_t>())) : nullptr;
      return value(o && o->seen && !o->effect && !o->throws && !o->diverted && o->v.known() && !writes_args(code_node(a[0], where)));
    }
    // return/break output passes through the node (return(caller) in the operator ?): VM node will not
    // count it - there is no function from which the node exits in its chain
    if(n == "jit_diverts" && a.size() == 1) {
      node_id c = code_node(a[0], where);
      return value(exits_through(c));
    }
    // Operator chain tree selected by parsing (empty - path dependent)
    if(n == "jit_chain" && a.size() == 2 && a[1].is<int64_t>()) {
      node_id t = jit_analysis ? jit_analysis->chain_tree(code_node(a[0], where), uint64_t(a[1].as<int64_t>())) : node_id{};
      return t ? value(array_v::of({code_value(t)})) : value(array_v::of({}));
    }
    // --- editing the program from the language (author's decision 2026-10-17):
// through the database and journal, as
    // editing the editor, not mutations of the ct code (these are derivatives, roll back). Only with the
    // explicit application of the editing function (tz apply, editor), not at every run: otherwise, the
    // execution of the program would change the program itself. My names and boundaries.
    if(n == "code_replace" || n == "code_rename" || n == "code_remove" || n == "code_insert") {
      if(!editing_program) fail(cat(n, ": правка программы — только при её применении (tz apply функция, редактор Ctrl+Shift+R)"), where);
      if(n == "code_replace" && a.size() == 2) { // The node becomes different (id the same)
        node_id c = code_node(a[0], where), to = code_node(a[1], where);
        if(c != to) {
          db.replace(c, db[to].kind);
          db.link_parents(c);
        }
        ++program_edits;
        return value(unit_v{});
      }
      if(n == "code_rename" && a.size() == 2 && a[1].is<str_v>()) { // declaration and all its use
        node_id d = code_node(a[0], where);
        symbol to = sym(a[1].as<str_v>());
        auto users = users_of(d);
        auto renamed = [&](node_id x) {
          node_kind k = db[x].kind;
          if(auto * f = std::get_if<n_fn>(&k)) f->name = to;
          else if(auto * v = std::get_if<n_var>(&k)) v->name = to;
          else if(auto * pa = std::get_if<n_param>(&k)) pa->name = to;
          else if(auto * i = std::get_if<n_ident>(&k)) i->name = to;
          else fail("code_rename: объявление — функция, переменная, параметр или имя", where);
          db.replace(x, std::move(k));
        };
        renamed(d);
        for(node_id u: users) renamed(u);
        ++program_edits;
        return value(int64_t(users.size()));
      }
      if(n == "code_remove" && a.size() == 1) { // module
        node_id c = code_node(a[0], where), up = db[c].parent;
        if(!up || !db.is_context(up)) fail("code_remove: удалить можно элемент блока или модуля", where);
        node_kind k = db[up].kind;
        std::visit([&](auto & x) {
          if constexpr(requires { x.items; }) std::erase(x.items, c);
        }, k);
        db.replace(up, std::move(k));
        db.link_parents(up);
        ++program_edits;
        return value(unit_v{});
      }
      if(n == "code_insert" && a.size() == 3 && a[1].is<int64_t>()) { // c block or module in place k
        node_id up = code_node(a[0], where), c = code_node(a[2], where);
        if(!db.is_context(up)) fail("code_insert: вставлять можно в блок или модуль", where);
        node_kind k = db[up].kind;
        std::visit([&](auto & x) {
          if constexpr(requires { x.items; }) {
            std::size_t at = std::min<std::size_t>(std::size_t(std::max<int64_t>(a[1].as<int64_t>(), 0)), x.items.size());
            x.items.insert(x.items.begin() + std::ptrdiff_t(at), c);
          }
        }, k);
        db.replace(up, std::move(k));
        db.link_parents(up);
        ++program_edits;
        return value(unit_v{});
      }
    }
    // --- Code links (author's decision 2026-10-17):
// Whoa
    // Uses an ad that calls a function, what it calls, what the node depends on, its instance,
    // types and values of node. Name to the ad by area (blocks up, parameters, module); call to
    // overload selected by parsing, otherwise by name. My:* composition and names (modeled
    // code_kind, code_items).
    if(n.starts_with("code_") && a.size() == 1 && relation_builtin(n)) {
      node_id c = code_node(a[0], where);
      auto codes = [&](std::vector<node_id> const & xs) {
        std::vector<value> out;
        for(node_id x: xs) out.push_back(code_value(x));
        return value(array_v::of(std::move(out)));
      };
      if(n == "code_declaration") return code_value(declaration_of(c));
      if(n == "code_targets") return codes(targets_of(c));
      if(n == "code_users") return codes(users_of(c));
      if(n == "code_callers") {
        std::vector<node_id> xs;
        for(node_id u: users_of(c))
          if(node_id up = db[u].parent; up && db.is<n_call>(up) && db.as<n_call>(up).callee == u) xs.push_back(up);
        return codes(xs);
      }
      if(n == "code_calls") {
        std::vector<node_id> xs;
        walk_code(c, [&](node_id x) {
          if(db.is<n_call>(x)) xs.push_back(x);
        });
        return codes(xs);
      }
      if(n == "code_deps") {
        std::vector<node_id> xs;
        walk_code(c, [&](node_id x) {
          if(!db.is<n_ident>(x)) return;
          node_id up = db[x].parent;
          std::vector<node_id> ds = up && db.is<n_call>(up) && db.as<n_call>(up).callee == x ? targets_of(up) : std::vector<node_id>{declaration_of(x)};
          for(node_id d: ds)
            if(d && !std::ranges::contains(xs, d) && !within_code(d, c)) xs.push_back(d);
        });
        return codes(xs);
      }
      // parsing (check): instance, types and values of node by instance. In parsing -
      // disassembled so far; in execution - parsing program (once).
      if(n == "code_instances") {
        interpreter & an = analysis();
        std::vector<value> xs;
        if(auto it = an.layers.find(c.value); it != an.layers.end())
          for(auto const & [key, l]: it->second) xs.push_back(value(key));
        return value(array_v::of(std::move(xs)));
      }
      if(n == "code_types" || n == "code_values") {
        interpreter & an = analysis();
        std::vector<value> xs;
        std::set<std::string> seen;
        for(auto const & e: an.journal) {
          if(e.node != c.value || !e.seen) continue;
          if(n == "code_types") { // Type - the meaning of this performance (by name)
            std::string t = an.types.describe(e.v);
            if(seen.insert(t).second) xs.push_back(value(type_v{&an == this ? types.of(e.v) : types.named(an.types.name(an.types.of(e.v)))}));
          } else if(auto lit = an.literal(e.v); lit && seen.insert(*lit).second) xs.push_back(e.v);
        }
        return value(array_v::of(std::move(xs)));
      }
    }
    // Code bypass: type node, daughter node, name, text (projection)
    if(n.starts_with("code_") && a.size() == 1) {
      node_id c = code_node(a[0], where);
      if(n == "code_kind") return value(type_v{types.named(lang_kind_of(db[c]))}); // tag
      if(n == "code_fields") return value(array_v::of(node_fields(c))); // node fields in order for sample unpacking
      if(n == "code_items") {
        std::vector<value> xs;
        db.for_each_child(c, [&](node_id x) { xs.push_back(code_value(x)); });
        return value(array_v::of(std::move(xs)));
      }
      if(n == "code_name") {
        symbol s;
        if(db.is<n_ident>(c)) s = db.as<n_ident>(c).name;
        else if(db.is<n_fn>(c)) s = db.as<n_fn>(c).name;
        else if(db.is<n_var>(c)) s = db.as<n_var>(c).name;
        else if(db.is<n_param>(c)) s = db.as<n_param>(c).name;
        else if(db.is<n_call>(c) && db.is<n_ident>(db.as<n_call>(c).callee)) s = db.as<n_ident>(db.as<n_call>(c).callee).name;
        else if(db.is_member(c)) s = db.member_of(c).name; // a.x - name x
        else if(db.is<n_module>(c)) s = db.as<n_module>(c).name; // Module key (the program is empty)
        else if(db.is<n_extend>(c)) s = db.as<n_extend>(c).target ? db.as<n_fn>(node_id(db.as<n_extend>(c).target)).name : db.as<n_extend>(c).name; // Name { … }, & f(…)
        return value(std::string(s ? str(s) : ""));
      }
      // The atom of the raw block - as written (projection block - its text):
// 
      // The atom remains node, the text is its projection.
      if(n == "code_text") {
        if(auto t = raw_atom_text(c)) return value(std::string(*t));
        return value(printer(db).print(c));
      }
      if(n == "code_glued") return value(raw_glued(c)); // atom close to the previous one in a raw block
      if(n == "code_comment") { // comments on the element (§4.0): the lines in front of it and the tail - the text as written
        std::string t;
        if(auto const * x = db.note_of(c)) {
          for(auto const & l: x->lead) t += cat(t.empty() ? "" : "\n", l);
          if(!x->tail.empty()) t += cat(t.empty() ? "" : "\n", x->tail);
        }
        return value(std::move(t));
      }
      if(n == "code_id") return value(int64_t(c.value)); // node number - compare node
      if(n == "code_parent") return code_value(db[c].parent); // The root is empty; the root is empty.
      if(n == "code_module") return code_value(module_of(c)); // module where the node is written (extension control, §9.1)
      if(n == "code_parse") return code_value(parse_sub(c, where)); // Subblock {…} - standard parsing seat (§4.4)
    }
    if(n == "typeof" && a.size() == 1) { // Value Approach: Type is also known for run-time
      if(a[0].is<rt_v>() && !a[0].as<rt_v>()) fail("тип неизвестен", where);
      return value(type_v{types.of(a[0])});
    }
    if(n == "zero" && a.size() == 1 && a[0].is<type_v>()) {
      auto t = types.name(a[0].as<type_v>().type);
      if(t == types.int_name()) return value(int64_t(0));
      if(t == types.float_name()) return value(double(0));
      fail(cat("zero не определён для ", t), where);
    }
    if(n == "range" && a.size() == 1) {
      value r;
      if(!a[0].known() && types.describe(a[0]) == types.int_name()) r = erase(value(array_v::of({value(int64_t(0))})));
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
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 16> prim2{
        {{"add", "+"}, {"sub", "-"}, {"mul", "*"}, {"div", "/"}, {"rem", "%"}, {"band", "&"}, {"bor", "|"}, {"bxor", "^"}, {"shl", "<<"}, {"shr", ">>"}, {"eq", "=="}, {"ne", "!="}, {"lt", "<"}, {"gt", ">"}, {"le", "<="}, {"ge", ">="}}};
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
      if(checking() && types.describe(a[0]) == types.int_name() && types.describe(a[1]) == types.int_name()) rt_check("overflow", where); // run-time - may throw
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
    if(n == "index" && a.size() == 2) { // a[i] kernel (operator(a)[i] prelude): array element, byte string
      if(!a[0].known() || !a[1].known()) return element_type(a[0], where);
      if(a[0].is<str_v>() && a[1].is<int64_t>()) {
        auto const & s = a[0].as<str_v>();
        int64_t k = a[1].as<int64_t>();
        if(k < 0 || uint64_t(k) >= s.size()) fail_rt("out_of_bounds", cat("индекс ", k, " вне строки длины ", s.size()), where);
        return value(int64_t(uint8_t(s[std::size_t(k)])));
      }
      if(!a[0].is<array_v>()) fail(cat("индексация значения типа ", types.describe(a[0])), where);
      return a[0].as<array_v>().get(checked_index(a[0], a[1], where));
    }
    if(n == "is_index" && a.size() == 2) { // 0 <= k < size(xs): The test that makes k a safe index
      if(unknown(a[0]) || unknown(a[1])) return value(rt_v{}); // type
      if(types.describe(a[0]) != types.int_name()) fail(cat("is_index: индекс должен быть ", types.int_name(), ", получено ", types.describe(a[0])), where);
      if(a[0].is<int64_t>() && a[1].is<array_v>()) {
        int64_t k = a[0].as<int64_t>();
        return value(k >= 0 && uint64_t(k) < a[1].as<array_v>().size());
      }
      if(!a[1].is<array_v>() && !(shape_of(a[1]) && shape_of(a[1])->is<array_v>()))
        fail(cat("is_index: ожидался массив, получено ", types.describe(a[1])), where);
      return erase(value(true));
    }
    // f64(i64 x) and i64(f64 x) are prelude ads above them
    if(n == "f32_round" && a.size() == 1 && types.describe(a[0]) == types.float_name()) // f32 - Rounding to single accuracy
      return a[0].known() ? value(double(float(a[0].as<double>()))) : erase(value(double(1)));
    if(n == "int_to_float" && a.size() == 1 && types.describe(a[0]) == types.int_name())
      return a[0].known() ? value(double(a[0].as<int64_t>())) : erase(value(double(1)));
    if(n == "float_to_int" && a.size() == 1 && types.describe(a[0]) == types.float_name())
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
    // ct-state of value (§5.6): ct_state(x, иначе) - it is not - otherwise
    if(n == "ct_state" && a.size() == 2) return a[0].ct ? ct_states.at(a[0].ct) : a[1];
    // Location of value in the program - the nearest call outside prelude (Library Check Track)
    if(n == "call_site" && a.empty()) return code_value(program_site());
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
  std::map<symbol, std::vector<node_id>> kind_members; // functions of the program node type: ast.twice { eval(…) }
  std::vector<std::vector<value> *> throw_sinks; // run-time branch shooters: try and instance
  std::map<std::string, std::set<std::string>> throws_of; // instance → Types of Exceptions (Report)
  std::vector<call_site> throw_stack; // last-throw
  std::map<std::string, uint32_t> instance_counts;
  std::map<std::string, uint32_t> folded;
  std::set<std::string> active;
  std::unordered_map<std::string, uint32_t> active_depth; // nesting
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
  scope * invoke_parent = nullptr; // call from local overload set: set environment (takes invoke)
  bool invoke_open = false; // run-time-length tail call (takes invoke)
  // parsing instance, which compiles JIT: A compiler in the language asks it for node
  // types and chain trees (jit_type, jit_chain).
//.
  interpreter const * jit_analysis = nullptr;
  // parsing, which reads the representation in the language (residual program, §13.2):
  // view_obs/view_live/view_declares - his observations of node.
  interpreter const * view_subject = nullptr;
  std::vector<value> subjects; // unpackable values - roots of addresses .f: subsample
  std::vector<node_id> module_order; // used modules (previously), then the
  std::map<uint32_t, std::set<symbol>> module_names; // use k. {a, b}: visible module names
  std::set<uint32_t> whole_modules; // use k; all advertisements
  node_id registering; // module whose elements are recorded
  bool extension_any = false; // The program has [[extension(…)]], otherwise registrations are not recorded.
  bool extensions_checked = false; // Extensions were checked: new (emit)
  std::unordered_map<uint32_t, std::vector<ext_reg>> entity_regs; // entity - Registration in order
  std::vector<std::pair<uint32_t, uint32_t>> ext_order; // (entity, registration number) - registration procedure
  std::unordered_map<uint32_t, std::set<uint32_t>> definition_scope; // control modules visible to it
  std::unordered_map<uint64_t, bool> definition_assoc; // (operator, module with control) → [[associative]] in its definition context
  std::optional<diagnostic> setup_error; // error of modules during creation - thrown during elaboration
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
  std::set<uint32_t> caller_users; // functions that take names from the calling environment (O4)
  std::vector<std::string> param_writes; // What functions change the parameters
  mutable std::map<uint64_t, bool> writes;
  mutable std::set<uint64_t> writes_open;
  mutable bool writes_guess_used = false;
  std::set<uint32_t> proven; // node Indexations Proven within Boundaries
  // Cycle invariant (parsing): cells written entirely per cycle (all iterations and contexts) and
  // cells of node indexing arrays (nullptr - array not variable)
  std::unordered_map<uint32_t, std::set<value *>> loop_whole;
  std::unordered_map<uint32_t, std::set<value *>> index_cells;
  // The decl link array (&e = xs[k]) for her lifetime (block residue) did not change entirely.
  std::unordered_map<uint32_t, std::set<value *>> ref_whole;
  bool ref_keeps(node_id decl) const {
    if(!db.is<n_var>(decl) || !db.as<n_var>(decl).ref) return false;
    auto c = index_cells.find(db.as<n_var>(decl).init.value);
    if(c == index_cells.end() || c->second.contains(nullptr)) return false;
    auto l = ref_whole.find(decl.value);
    if(l == ref_whole.end()) return true; // There's nothing in the block.
    for(value * x: c->second)
      if(l->second.contains(x)) return false;
    return true;
  }
  bool loop_keeps(node_id loop, node_id index) const {
    auto l = loop_whole.find(loop.value);
    auto c = index_cells.find(index.value);
    if(l == loop_whole.end() || c == index_cells.end() || c->second.contains(nullptr)) return false;
    for(value * x: c->second)
      if(l->second.contains(x)) return false;
    return true;
  }
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
  symbol sym_alloc_in;
  std::string prog_path; // Program path (args[0]): Nearby is its state
  bool owns_state = false; // Executes the program (run_main, machine): opens its state
  std::optional<address_space> state_level; // non-volatile state level of the program (at the root VM)
  std::set<value const *> state_cells;
  std::deque<address_space> regions; // region() memory levels (at VM root); released with last holder
  bool regions_alive = false;
  symbol sym_intercept, sym_is_index, sym_append, sym_pin_storage, sym_spawn, sym_yield, sym_process, sym_pressure; // kernel forms analyzed prior to the calculation of arguments
  symbol sym_set_ct_state; // ct-state of place (disassembled in place as append)
  // Symbol on the text without internment (journal lines of the base does not change): while the
  // line is not, there is no name with it; it is searched again only when the lines become more.
  struct lazy_sym {
    std::string_view text;
    symbol s;
    uint32_t seen = 0;
  };
  symbol known(lazy_sym & l) {
    if(!l.s && l.seen != db.strings.size()) {
      l.seen = db.strings.size();
      if(auto it = db.strings.index.find(l.text); it != db.strings.index.end()) l.s = it->second;
    }
    return l.s;
  }
  lazy_sym sym_index_op{"infix[]"}, sym_code{"code"}, sym_ref{"ref"}, sym_associative{"associative"}; // Comparison by number, not text (hot path)
  uint32_t hook_depth = 0; // Executed by hook parsing (call_hook, attribute_hooks)
  std::unordered_map<uint32_t, node_id> hook_idents; // Names of hook arguments parsing (call_hook)
  std::vector<std::string> instance_keys; // instance in the parsing chain
  std::map<std::string, std::map<std::string, std::pair<span, std::string>>> rt_checks; // instance → run-time - checks
  bool level_probe = false; // This is parsing for static_level.

  std::deque<fiber_rec> fibers; // root 0 is the main
  // at the root of VM (common to processes): scheduler, threads, expectation host
  interpreter * root = this; // Lane scheduler: ready, ring, main fiber
  interpreter * vm_root = this; // general VM: threads, processes, program state (the background band is the main root)
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
  // The variable module → the module that declared it: visible to its code and the code of the
  // modules that see it (use, prelude) - hygiene by visibility (foreign_global).
  std::unordered_map<value const *, node_id> global_home;
  std::vector<symbol> ct_names;
  uint32_t ct_depth = 0;
  // elaboration: the ct module is executed - the source of code mutations (node ct { } / ct x = e)
  bool elaborating = false;
  node_id ct_source;
  uint64_t elab_base = 0; // birth cycle node before this elaboration (db.births): later - created by it
  node_id deriving; // parsing is a raw block (§4.4): its node is completed without a journal source
  uint64_t derive_base = 0; // clock before that parsing
  bool elab_nested = false; // Elaboration within another: mutations have already been made
  // parsing elaboration (instantiation_pass): ct code instance changes the code
  bool instantiating = false;
  uint64_t pass_base = 0; // clock to parsing : node, born later - completion
  std::pair<uint32_t, uint64_t> ct_top; // external executable ct node and instance
  std::unordered_set<uint32_t> used_rt; // node run-time - codes already disassembled (parsing elaboration)
  std::set<uint32_t> inst_registered; // copy
  bool code_failed = false; // parsing elaboration: code change rejected (program error)
  bool pass_ran = false; // This interpreter carried out parsing elaboration: ct - its results
  std::vector<std::tuple<span, std::string, std::string>> code_changes; // Inspector: place ct, instance, which is modified
  std::vector<node_id> emitted; // Advertisements generated by emit (tz fmt - ct)
  std::vector<override_rec> overrides;
  uint32_t override_depth = 0;
  uint64_t array_gen = 0; // generation of array buffers (performance)
  std::set<uint32_t> user_ops; // operators declared code (not built-in primitive)
  std::set<uint32_t> local_ops; // operators declared in blocks (local functions)
  std::vector<std::array<symbol, 3>> op_entities; // entity: prefix, infix, postfix
  std::unordered_map<uint32_t, bool> assoc_ops; // operator [[associative]] for a module element or prelude
  std::unordered_map<uint32_t, symbol> infix_entities; // + → infix+
  std::unordered_map<uint32_t, node_id> assoc_trees; // The chain of one associative operator
  std::unordered_map<uint32_t, std::vector<symbol>> chain_names; // chain-name
  std::unordered_map<std::string, node_id> resolved_chains; // (chain, types of names) → tree
  std::map<std::pair<uint32_t, uint64_t>, node_id> chain_trees; // (chain, context) → tree (for JIT)
  std::unordered_map<std::string, std::optional<value>> applied; // (operator, types) → type of result
  std::unordered_map<uint32_t, node_id> apply_nodes; // l op r for a sample
  std::deque<interpreter> probes; // parsing chains without side effects
  // Derivatives of node held by this interpreter (ast_db::derive): at its end are free for the
  // following (the same result is the same node)
  std::vector<uint32_t> derived_nodes;
  struct derived_release {
    interpreter & vm;
    ~derived_release() { vm.db.release(vm.derived_nodes); }
  } derived_hold{*this};
  uint64_t effects = 0;
  uint64_t checks = 0; // run-time - parsing check (observation: node can throw)
};

// Selection of printing code in language (§4.4, variant C): the net function of the fn
// module over the print descriptors is values and code.
// 
// Purity (my): no execution effects (output, I/O), no code change and emit; otherwise, as
// with error or non-bool, -1 and reason: printing takes a conservative choice.
//
int language_choice(ast_db const & cdb, choice_vms & pool, node_id fn, std::vector<choice_arg> const & args, std::string & why) {
  static thread_local bool busy = false; // printing within selection (code_text) - no choice
  if(busy) {
    why = "выбор печати внутри выбора печати";
    return -1;
  }
  ast_db & db = const_cast<ast_db &>(cdb); // Derivatives of node - Argument Names (Derive) - not code editing
  node_id m = fn;
  while(m && !db.is<n_module>(m)) m = db[m].parent;
  if(!m) {
    why = "функция выбора вне модуля";
    return -1;
  }
  struct busy_hold {
    busy_hold() { busy = true; }
    ~busy_hold() { busy = false; }
  } hold;
  std::size_t mutations = db.mutations.size();
  try {
    auto at = std::ranges::find(pool.mods, m);
    if(at == pool.mods.end()) { // one interpreter per module per print
      pool.vms.emplace_back(db, m, std::vector<std::string>{}, mode::run);
      pool.mods.push_back(m);
      at = pool.mods.end() - 1;
    }
    interpreter & vm = *std::next(pool.vms.begin(), at - pool.mods.begin());
    if(vm.setup_error) throw *vm.setup_error;
    uint64_t effects = vm.effects;
    std::size_t emitted = vm.emitted.size();
    scope * s = vm.new_scope(vm.globals);
    interpreter::scope_hold sh{vm, s};
    node_list names;
    auto const & params = db.as<n_fn>(fn).params;
    for(std::size_t i = 0; i < args.size(); ++i) {
      symbol n = vm.sym(cat("__choice", i));
      value v = std::visit([&](auto const & a) -> value {
        using T = std::remove_cvref_t<decltype(a)>;
        if constexpr(std::is_same_v<T, node_id>) return vm.code_value(a);
        else return value(a);
      },
          args[i]);
      s->declare(n, std::move(v));
      auto & id = vm.hook_idents[n.value];
      if(!id) id = db.derive(n_ident{n}, {}, vm.derived_nodes);
      names.push_back(id);
    }
    if(params.size() != args.size()) throw diagnostic(cat("ожидалось параметров: ", args.size()), db[fn].where);
    auto f = vm.call_overloaded({fn}, names, s, 0, {db[fn].where, std::string(db.strings.text(db.as<n_fn>(fn).name)), 0, 0});
    if(f.kind == interpreter::flow_kind::thr) why = cat("исключение: ", vm.show_v(f.v));
    else if(vm.effects != effects || vm.emitted.size() != emitted || db.mutations.size() != mutations) why = "не чистая (эффект, изменение кода или emit)";
    else if(!f.normal() || !f.v.is<bool>()) why = "ожидался итог bool";
    else return f.v.as<bool>() ? 1 : 0;
  } catch(diagnostic const & d) {
    why = d.what();
  }
  return -1;
}

} // namespace tz
