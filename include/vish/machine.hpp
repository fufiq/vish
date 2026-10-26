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

namespace vish {

constexpr uint32_t m_none = ~uint32_t{0};

// The state containers of the machine are the pages of its process level (§12.2): the dump is the same.
template<typename T>
using m_list = std::vector<T, pages_alloc<T>>;
using m_str = std::basic_string<char, std::char_traits<char>, pages_alloc<char>>;

struct mval;
struct m_field;
struct m_unit {};
struct m_type {
  type_id type;
};
struct m_arr {
  m_list<mval> items;
};
struct m_agg {
  type_id type;
  m_list<m_field> fields;
  uint32_t rid = 0; // Resource (type with destroy, shared): number - copies divided, destroy - once
};
// Function as a value: overload by name or lambda by environment (area number).
struct m_fn {
  symbol name;
  node_id lambda;
  uint32_t env = m_none;
  m_list<node_id> set; // Local set of overloads (block functions of the same name)
};
// expr-параметр: выражение вызывающего в его области и кадре (§6.1).
struct m_thunk {
  node_id expr;
  uint32_t env = m_none;
  uint32_t frame = 0;
  uint64_t ctx = 0; // context
  uint32_t rules = m_none; // structure block: area of her body with rules (operator, syntax...)
};
// Parameter - alias of the caller's place (author's decision): area and slot. alias is a place by
// number: a variable (scope, slot) or an element of its array (index: link &e = v[i], §12.1) is a
// base and an index, not an address: the growth and replacement of an array do not break the link.
struct m_alias {
  uint32_t scope = m_none;
  uint32_t slot = 0;
  uint32_t index = m_none;
  uint32_t field = m_none; // variable aggregate field: b.xs to variable parameter (§6.2)
  // link-path (§12.1, tail of step 10): &e = p.x, &f = ps[i].y, &e = r[k] from the link - the steps
  // lie in the area where the link is declared (live with it), from the variable (scope, slot)
  uint32_t path_scope = m_none;
  uint16_t path_at = 0;
  uint8_t path_n = 0;
};
// Step of the link path: field (number in aggregate) or element (index).
struct m_step {
  uint32_t at = 0;
  bool field = false;
};
struct mval {
  std::variant<m_unit, bool, int64_t, double, m_str, m_type, m_arr, m_agg, m_fn, m_thunk, m_alias> v;
  // ct-state of value (§5.6 as value::ct interpreter): the number in m_state::ct_states+1 travels with
  // copies of the value; set_ct_state(x, s) puts it in place x
  uint32_t ct = 0;

  mval() = default;
  mval(auto x)
    requires(!std::is_convertible_v<decltype(x), std::string_view>)
      : v(std::move(x)) {}
  mval(std::string_view s) : v(m_str(s)) {} // line - in process level pages

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

// shared(x) storage: value and number of references (§12.2)
struct m_shared {
  mval v;
  uint32_t count = 0;
};

struct m_var {
  symbol name;
  mval v;
  uint32_t hidden = 0; // Node hide/own +1: name hidden (moved)
};
struct m_scope {
  uint32_t parent = m_none;
  m_list<m_var> vars;
  bool captured = false; // Lambda environment - lives longer than its block
  bool live = true;
  uint32_t caller = m_none; // Body root function: call environment (O4)
  uint32_t fn = m_none;
  bool layer = false; // layer of design rules over the domain of the caller (ruled_scope)
  m_list<m_step> steps; // steps of link-paths declared in the field
  uint32_t site = 0; // parameter area: call node - transfer from parameter - alias solves and calling
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
  m_str what;
  span where;
};
struct m_error {
  bool present = false;
  m_str what;
  span where;
  m_str type;
  m_list<m_trace> trace;
};

enum : uint8_t { f_ready, f_blocked, f_done };

// fiber - its stack of tasks and values, frames: switching - changing the number (§8.3).
struct m_fiber {
  m_list<m_task> tasks;
  m_list<mval> vals;
  m_list<m_frame> frames;
  m_signal pending;
  m_list<m_override> overrides;
  uint8_t state = f_ready;
  bool direct = false; // In the queue first: awakened by exchange (direct transmission)
  m_error error;
  // VM process (§9.2, step 12.1): 0 is the program itself, otherwise the number in m_state::procs + 1; the
  // root of the process is its args on top of the general globals; input fiber is the end of the process
  uint32_t proc = 0;
  uint32_t globals = m_none;
  bool proc_entry = false;
};
// on_pressure { … } (step 12.3): The block and its environment, the process owner.
struct m_pressure {
  m_thunk body;
  uint32_t env = m_none;
  uint32_t proc = 0;
  uint32_t globals = m_none;
};
// The VM process within the machine process:
// Its error is the outcome, not the error of the program.
struct m_proc {
  bool done = false;
  bool failed = false;
  m_str error;
  m_list<uint32_t> waiters;
};

// Write/read between fiber (§8.3): A buffer waiting to be read and written.
struct m_stream {
  uint64_t cap = 16;
  m_list<mval> buf;
  bool closed = false;
  m_list<uint32_t> readers, writers;
  uint8_t host = 0; // host resource (§9.5 step 12.1): 1 file to read, 2 file to write, 3 network; resource itself outside the dump
};

// Body area entity functions: its local functions declared (entity .f(…)).
struct m_entity {
  symbol owner;
  uint32_t scope = m_none;
};

// The state of the process is everything that Dump writes.
struct m_state {
  m_list<m_scope> scopes;
  m_list<uint32_t> free_scopes;
  m_list<m_fiber> fibers;
  uint32_t frame_counter = 0;
  uint32_t globals = m_none;
  uint64_t steps = 0;
  m_list<m_str> type_names; // Numbers of value types - as in the register at the time of dump
  bool done = false;
  int64_t exit_code = 0;
  // Input: given strings (tests, session) or stdin host when the queue is not set
  bool queued_input = false;
  m_list<m_str> input;
  // fiber : current, queue of ready, waiting for the end of fiber ; streams
  uint32_t current = 0;
  m_list<uint32_t> ready;
  m_list<uint32_t> joiners;
  m_list<m_stream> streams;
  bool deadlock = false; // Everyone is waiting for each other: the mistake will be the main one.
  uint32_t handoff_run = 0; // direct
  m_list<m_ct> ct_memo; // Canonical order ct: results of parsing of the entire program
  m_list<symbol> ct_vars; // ct-variable module (change - only ct-code)
  m_list<m_entity> entities; // entity Body Area Functions for entity .f(…)
  // resources (§12.2): live numbers, shared storage; res_on - destroy or shared
  bool res_on = false;
  uint32_t res_counter = 0;
  m_list<uint32_t> res_live;
  m_list<m_shared> shared;
  m_list<mval> ct_states; // ct-state of values (mval::ct - number + 1)
  m_list<m_proc> procs; // VM processes (process(f, …))
  m_list<m_pressure> pressure; // Pressure handlers (on_pressure { … })
  m_list<symbol> service_names; // VM service variables (step 12.4): in the new VM - again
};

// Process dump header: everything except level pages.
struct dump_head {
  std::string magic = "VISHPROC10"; // 10: No Buffer Generations (link - Base and Index)
  std::string name;
  uint32_t root = 0;
  std::vector<journal_entry> journal;
  uint64_t base = 0, small_used = 0, small_committed = 0, big_next = 0, state_at = 0, pages_at = 0;
  std::vector<std::vector<uint64_t>> small_free = std::vector<std::vector<uint64_t>>(address_space::classes);
  std::vector<uint64_t> big_free;
  std::vector<address_space::piece> pieces;
};

enum : uint8_t { m_eval, m_call_body, m_lambda_body, m_thunk_eval, m_intercept, m_elem_ref, m_path_ref };
enum : uint8_t { sig_none, sig_break, sig_continue, sig_return, sig_throw, sig_tail };

struct process_machine {
  ast_db & db;
  node_id root;
  std::string name;
  interpreter helper; // Static Solutions and Primary Operations: No Process State
  // Process Level (§12.2): The entire state of the machine is the page of its address band; the
  // dump is the same as it is.
//.
  address_space band;
  address_space * outer_space = machine_space; // level of the car that was before
  m_state & st;
  std::string * captured = nullptr; // output - in line (tests), otherwise stdout
  std::string elab_out; // withdrawal of assistant elaboration (ct modules) to start

  // host resources (§9.5 step 12.1): Descriptors and kernel applications are not part of the process.
// 
// They do not enter the stream, but
  // After resume, the host stream is closed externally (author's decision 2026-10-19): read - end of
  // stream, write - exception, send - false. The device is like an interpreter (M3b): the network is one
  // event stream, files are streams of pieces, applications are io_uring.
  struct host_peer {
    int fd = -1;
    bool open = true;
    bool sending = false;
    std::deque<std::string> out;
    std::size_t sent = 0;
  };
  struct host_rec {
    int listen_fd = -1;
    int file_fd = -1;
    bool connected = false;
    bool io_busy = false;
    uint64_t offset = 0;
    std::string io_buf;
    sockaddr_in addr{};
    std::deque<host_peer> peers;
  };
  std::deque<host_rec> host; // by flow number (order addresses are stable)
  // What the author gave (as in the interpreter, VISH_GRANT at launch): not part of the process - in the new VM
  // gives it to the user; proc grant/revoke - on the go: took - streams of this kind are closed by the external side
  std::set<std::string, std::less<>> grants = interpreter::granted_by_user();
  bool machine_granted(std::string_view kind) const { return !st.fibers[st.current].proc && grants.contains(kind); }
  void grant(std::string_view kind, bool give) {
    if(give) {
      grants.emplace(kind);
      return;
    }
    grants.erase(std::string(kind));
    uint8_t want = kind == "net" ? 3 : 0;
    for(uint64_t sid = 0; sid < st.streams.size(); ++sid) {
      m_stream & s = st.streams[sid];
      bool hit = want ? s.host == want : (s.host == 1 || s.host == 2);
      if(!hit || s.closed) continue;
      close_host(sid);
      s.closed = true; //  
      for(uint32_t r: std::exchange(s.readers, {})) wake(r);
      for(uint32_t w: std::exchange(s.writers, {})) wake(w);
    }
  }
  std::optional<os::uring> ring;
  uint32_t io_pending = 0; // nucleus

  // Repression of backgrounds (step 12.2, as in the interpreter, interpreter::preempt_point): the safe point
  // is the reverse edge of the cycle and the call of the VM process; departure is after the step (in the
  // middle of the step, the fiber tasks are still changing). Monopoly (the program itself) is not replaced.
  uint32_t preempt_left = 1; // The first safe point puts the quantum
  uint32_t preempt_fiber = m_none; // fiber, which is time to leave
  uint64_t slice_start = 0;
  uint64_t preemptions = 0;
  void safe_point(bool edge) { // edge - reverse edge; call - only for time quanta
    if(!fib().proc || (!edge && helper.slice_points) || --preempt_left != 0) return;
    preempt_left = helper.slice_points ? helper.slice_points : 1024;
    if(st.ready.empty()) return;
    if(!helper.slice_points && os::uring::now_ms() - slice_start < interpreter::slice_ms) return;
    preempt_fiber = st.current;
  }

  process_machine(ast_db & db, node_id root, std::string name, std::string_view dump = {}, int dump_fd = -1)
      : db(db), root(root), name(name), helper(db, root, std::vector<std::string>{name}, mode::run), st(place_state(dump, dump_fd)) {
    try { // The strip is already open: there will be no destructor - close it here
      helper.owns_state = true;
      helper.capture_output(&elab_out); // output of ct modules at the beginning of the process output (as in the interpreter)
      helper.elaborate(); // [[state]] - the assistant opens the state of the program (cell - it)
      helper.capture_output(nullptr);
      if(!dump.empty()) helper.types.assign_names(st.type_names); // type numbers - as in a dump
      if(!dump.empty()) host_lost();
      if(!dump.empty() && !st.pressure.empty()) arm_pressure(); // Pressure handlers - and in the new VM
      if(!dump.empty()) services_fresh();
    } catch(...) {
      st.~m_state();
      close_band(band);
      machine_space = outer_space;
      throw;
    }
  }
  ~process_machine() {
    try {
      save_state_vars(true);
    } catch(...) {
    }
    if(psi_fd >= 0) ::close(psi_fd);
    for(auto & h: host) { // host descriptors - no further than the machine (the application will be removed by the ring)
      for(int fd: {h.listen_fd, h.file_fd})
        if(fd >= 0) ::close(fd);
      for(auto & pr: h.peers)
        if(pr.fd >= 0) ::close(pr.fd);
    }
    st.~m_state();
    close_band(band);
    machine_space = outer_space;
  }
  process_machine(process_machine const &) = delete;

  // The state is in the process lane: new or dump pages at the same addresses. dump_fd - dump file:
  // pages are displayed as a copy when written (file does not change)
  m_state & place_state(std::string_view dump, int dump_fd) {
    if(dump.empty()) {
      if(!open_band(band, 0)) throw diagnostic("машина процессов: нет свободной полосы адресов", {});
      machine_space = &band;
      return *new(space_alloc(band, sizeof(m_state))) m_state;
    }
    reader r(dump);
    dump_head h = r.get<dump_head>();
    if(!open_band(band, h.base)) throw diagnostic("дамп процесса: его адреса заняты", {});
    machine_space = &band;
    std::size_t at = h.pages_at;
    for(auto const & p: h.pieces) { // Pages - as they were, in the same addresses
      if(dump_fd >= 0) {
        if(mmap(band.lo + p.addr, p.len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, dump_fd, off_t(at)) == MAP_FAILED)
          throw diagnostic("дамп процесса: не отобразить страницы", {});
        band.pieces.push_back(p);
      } else {
        band.commit(band.lo + p.addr, p.len);
        std::memcpy(band.lo + p.addr, dump.data() + at, p.len);
      }
      at += p.len;
    }
    band.small_used = h.small_used;
    band.small_committed = h.small_committed;
    band.big_next = band.lo + h.big_next;
    for(uint32_t k = 0; k < address_space::classes; ++k)
      for(uint64_t o: h.small_free[k]) band.small_free[k].push_back(reinterpret_cast<uint64_t *>(band.lo + o));
    for(uint64_t o: h.big_free) band.big_free.push_back(reinterpret_cast<uint64_t *>(band.lo + o));
    uint64_t big_at = address_space::state_meta + address_space::state_small; // plug-in
    for(auto const & p: h.pieces)
      if(p.addr >= big_at) {
        std::byte * slice = band.lo + big_at + (p.addr - big_at) / address_space::state_big * address_space::state_big;
        size_t have = size_t(band.lo + p.addr + p.len - slice);
        auto it = std::ranges::find_if(band.big_have, [&](auto const & x) { return x.first == slice; });
        if(it == band.big_have.end()) band.big_have.push_back({slice, have});
        else it->second = std::max(it->second, have);
      }
    return *reinterpret_cast<m_state *>(band.lo + h.state_at);
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
    if(st.res_on) // In the reverse order of the announcement, alias and the transferred are not its own.
      for(uint32_t i = uint32_t(st.scopes[id].vars.size()); i-- > 0;) {
        m_var & v = st.scopes[id].vars[i];
        if(v.hidden || v.v.is<m_alias>() || v.v.is<m_thunk>() || v.v.is<m_fn>() || !has_res(v.v)) continue;
        mval x = std::move(v.v);
        v.v = mval{m_unit{}};
        drop_value(x, {});
      }
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
  place find_lexical(uint32_t scope, symbol name) const {
    for(uint32_t s = scope; s != m_none; s = st.scopes[s].parent) {
      auto const & vs = st.scopes[s].vars;
      for(uint32_t i = uint32_t(vs.size()); i-- > 0;)
        if(vs[i].name == name) return {s, i};
    }
    return {};
  }
  // Lexically, then (if it is not a function, type, or built-in name) - the environment
  // of the caller and further along the call chain, as in the interpreter (O4).
  place find(uint32_t scope, symbol name) const {
    place p = find_lexical(scope, name);
    if(p.scope != m_none) return p;
    std::string_view s = helper.str(name);
    if(helper.functions.contains(name) || helper.structs.contains(name) || helper.types.builtin(s) || s == "true" || s == "false") return {};
    for(uint32_t k = scope; k != m_none; k = st.scopes[k].parent) {
      if(st.scopes[k].caller == m_none) continue;
      p = find(st.scopes[k].caller, name);
      if(p.scope != m_none && st.scopes[k].fn != m_none && db.as<n_fn>(node_id(st.scopes[k].fn)).outline)
        throw diagnostic(cat(helper.describe_fn(node_id(st.scopes[k].fn)), " помечена outline, но берёт имя ", s, " из окружения вызывающего"), db[node_id(st.scopes[k].fn)].where);
      return p;
    }
    return {};
  }
  // Place cell - through alias to the present.
  mval & cell(place p) {
    mval * c = &st.scopes[p.scope].vars[p.slot].v;
    if(c->is<m_alias>()) c = &follow(c->as<m_alias>());
    return *c;
  }
  // alias Cell: A place is searched anew with each reference (base and index) like an
  // interpreter; no element is out_of_bounds.
  mval & follow(m_alias al) {
    mval * c = &st.scopes[al.scope].vars[al.slot].v;
    if(c->is<m_alias>()) c = &follow(c->as<m_alias>());
    if(al.path_scope != m_none) { // The path is step by step from the variable (place is sought again: there is nothing to hang)
      for(uint32_t k = 0; k < al.path_n; ++k) {
        m_step const & s = st.scopes[al.path_scope].steps[al.path_at + k];
        if(c->is<m_agg>() && is_shared_v(*c)) c = &slot_of(*c).v;
        if(s.field) {
          if(!c->is<m_agg>() || s.at >= c->as<m_agg>().fields.size()) {
            diagnostic d("ссылка на поле использована после замены агрегата", here(), diag_kind::runtime);
            d.type = "invalidated_alias";
            throw d;
          }
          c = &c->as<m_agg>().fields[s.at].v;
          continue;
        }
        if(!c->is<m_arr>()) throw diagnostic(cat("ссылка на элемент: ", describe(*c), " — не массив"), here());
        auto & xs = c->as<m_arr>().items;
        if(s.at >= xs.size()) out_of_bounds(s.at, xs.size(), here());
        c = &xs[s.at];
      }
      return *c;
    }
    if(al.index != m_none) { // element (then, if any, its field: cl[c].xs)
      if(!c->is<m_arr>()) throw diagnostic(cat("ссылка на элемент: ", describe(*c), " — не массив"), here());
      auto & xs = c->as<m_arr>().items;
      if(al.index >= xs.size()) out_of_bounds(al.index, xs.size(), here());
      c = &xs[al.index];
    }
    if(al.field != m_none) { // path by field number - allowed at each request, there is nothing to hang
      if(!c->is<m_agg>() || al.field >= c->as<m_agg>().fields.size()) {
        diagnostic d("ссылка на поле использована после замены агрегата", here(), diag_kind::runtime);
        d.type = "invalidated_alias";
        throw d;
      }
      return c->as<m_agg>().fields[al.field].v;
    }
    return *c;
  }
  span here() { return fib().tasks.empty() ? span{} : db[fib().tasks.back().node].where; }
  mval deref(mval const & x) {
    if(x.is<m_alias>()) return follow(x.as<m_alias>());
    return x;
  }
  // Reading without copy (index, size, field): alias is the cell itself. A copy of the array on
  // each xs[k] and is_index(k, xs) made the cycle quadratic (found in step 12.2).
  mval const & ref_of(mval const & x) { return x.is<m_alias>() ? follow(x.as<m_alias>()) : x; }
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
      else if constexpr(std::is_same_v<T, m_str>) return value(std::string_view(a));
      else return value(a);
    },
        x.v);
  }

  mval from_value(value const & x, span where) {
    return std::visit([&](auto const & a) -> mval {
      using T = std::remove_cvref_t<decltype(a)>;
      if constexpr(std::is_same_v<T, unit_v>) return mval{m_unit{}};
      else if constexpr(std::is_same_v<T, bool> || std::is_same_v<T, int64_t> || std::is_same_v<T, double>) return mval{a};
      else if constexpr(std::is_same_v<T, str_v>) return mval{a.str()};
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
      if constexpr(std::is_same_v<T, bool>) return std::string(helper.types.bool_name());
      else if constexpr(std::is_same_v<T, int64_t>) return std::string(helper.types.int_name());
      else if constexpr(std::is_same_v<T, double>) return std::string(helper.types.float_name());
      else if constexpr(std::is_same_v<T, m_str>) return std::string(helper.types.str_name());
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
    for(uint32_t i = 0; i < a.size(); ++i) line += (i ? " " : "") + helper.print_text(to_value(a[i]));
    line += '\n';
    if(captured) *captured += line;
    else {
      put(stdout, line);
      std::fflush(stdout);
    }
  }

  // --- host resources (step 12.1) ---

  enum : uint64_t { op_accept = 1, op_recv, op_send, op_connect, op_fread, op_fwrite, op_psi };
  static uint64_t io_tag(uint64_t op, uint64_t sid, uint64_t peer) { return (op << 56) | (sid << 24) | peer; }
  [[noreturn]] static void rt_fail(std::string_view type, std::string what, span where) {
    diagnostic d(std::move(what), where, diag_kind::runtime);
    d.type = std::string(type);
    throw d;
  }
  os::uring & io(span where) {
    if(!ring) ring.emplace();
    if(!ring->ok) rt_fail("io_error", "io_uring недоступен в этой системе", where);
    return *ring;
  }
  // host recording of the stream; no - the resource did not survive the process transfer (closed by the external side)
  host_rec * host_of(uint64_t sid) { return sid < host.size() && st.streams[sid].host ? &host[sid] : nullptr; }
  uint64_t host_stream(uint8_t kind) {
    st.streams.emplace_back();
    st.streams.back().host = kind;
    st.streams.back().cap = ~uint64_t{0} >> 1; // host events do not wait for the reader.
    if(host.size() < st.streams.size()) host.resize(st.streams.size());
    return st.streams.size() - 1;
  }
  // The process is restored in another VM: it has no host resources - the external side has closed the process.
  void host_lost() {
    for(auto & s: st.streams)
      if(s.host && !s.closed) {
        s.closed = true;
        for(uint32_t r: std::exchange(s.readers, {})) wake(r);
        for(uint32_t w: std::exchange(s.writers, {})) wake(w);
      }
  }
  mval event(int64_t peer, std::string_view kind, std::string_view data) {
    m_agg g{helper.types.named("event"), {}};
    g.fields.push_back({helper.sym("peer"), mval{peer}});
    g.fields.push_back({helper.sym("kind"), mval{kind}});
    g.fields.push_back({helper.sym("data"), mval{data}});
    return mval{std::move(g)};
  }
  void deliver(uint64_t sid, mval v) {
    m_stream & s = st.streams[sid];
    s.buf.push_back(std::move(v));
    if(!s.readers.empty()) {
      uint32_t r = s.readers.front();
      s.readers.erase(s.readers.begin());
      wake(r);
    }
  }
  void peer_closed(uint64_t sid, std::size_t p, std::string_view why = {}) {
    host_peer & pr = host[sid].peers[p];
    if(!pr.open) return;
    pr.open = false;
    if(pr.fd >= 0) {
      ::close(pr.fd);
      pr.fd = -1;
    }
    if(!st.streams[sid].closed) deliver(sid, event(int64_t(p), "close", why));
  }
  void send_next(uint64_t sid, std::size_t p) {
    host_peer & pr = host[sid].peers[p];
    if(pr.sending || pr.out.empty() || pr.fd < 0) return;
    pr.sending = true;
    ++io_pending;
    io({}).send(pr.fd, pr.out.front().data() + pr.sent, pr.out.front().size() - pr.sent, io_tag(op_send, sid, p));
  }
  void arm_recv(uint64_t sid, std::size_t p) {
    host_peer & pr = host[sid].peers[p];
    if(pr.fd < 0) return;
    ++io_pending;
    io({}).recv_all(pr.fd, io_tag(op_recv, sid, p));
  }
  void file_write_next(uint64_t sid) {
    host_rec & h = host[sid];
    host_peer & q = h.peers[0];
    if(h.io_busy || q.out.empty()) {
      if(q.out.empty())
        for(uint32_t w: std::exchange(st.streams[sid].writers, {})) wake(w);
      return;
    }
    h.io_busy = true;
    ++io_pending;
    io({}).write(h.file_fd, q.out.front().data() + q.sent, q.out.front().size() - q.sent, h.offset, io_tag(op_fwrite, sid, 0));
  }
  // The completion of the kernel application is an event in the stream, the awakening of those waiting.
  void io_complete(os::uring::completion const & c) {
    if(!c.tag) return; // cancellation
    uint64_t op = c.tag >> 56, sid = (c.tag >> 24) & 0xFFFFFFFF;
    if(op == op_psi) { // host lacks memory
      psi_armed = false;
      if(c.res >= 0) {
        pressure();
        arm_pressure();
      }
      return;
    }
    std::size_t p = c.tag & 0xFFFFFF;
    bool more = c.flags & IORING_CQE_F_MORE;
    if(!more) --io_pending;
    host_rec & h = host[sid];
    m_stream & s = st.streams[sid];
    switch(op) {
      case op_accept:
        if(c.res >= 0 && !s.closed) {
          h.peers.push_back({c.res});
          deliver(sid, event(int64_t(h.peers.size() - 1), "open", {}));
          arm_recv(sid, h.peers.size() - 1);
        } else if(c.res >= 0) ::close(c.res);
        if(!more && !s.closed && h.listen_fd >= 0) {
          ++io_pending;
          io({}).accept_all(h.listen_fd, io_tag(op_accept, sid, 0));
        }
        break;
      case op_recv: {
        host_peer & pr = h.peers[p];
        if(c.res > 0) {
          unsigned bid = c.flags >> IORING_CQE_BUFFER_SHIFT;
          std::string data(ring->buffer(bid), std::size_t(c.res));
          ring->give_back(bid);
          if(pr.open && !s.closed) deliver(sid, event(int64_t(p), "data", data));
          if(!more && pr.open) arm_recv(sid, p);
        } else if(c.res == -ENOBUFS) {
          if(!more && pr.open) arm_recv(sid, p);
        } else if(!more) peer_closed(sid, p, c.res < 0 ? std::string_view(std::strerror(-c.res)) : std::string_view());
        break;
      }
      case op_send: {
        host_peer & pr = h.peers[p];
        pr.sending = false;
        if(c.res < 0) {
          peer_closed(sid, p, std::strerror(-c.res));
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
          h.peers[0].open = false;
          ::close(h.peers[0].fd);
          h.peers[0].fd = -1;
          deliver(sid, event(0, "close", std::strerror(-c.res)));
          break;
        }
        h.connected = true;
        deliver(sid, event(0, "open", {}));
        arm_recv(sid, 0);
        send_next(sid, 0);
        break;
      case op_fread:
        h.io_busy = false;
        if(c.res > 0) {
          h.offset += uint64_t(c.res);
          deliver(sid, mval{std::string_view(h.io_buf.data(), std::size_t(c.res))});
        } else {
          s.closed = true; // end of file (or reading error)
          for(uint32_t r: std::exchange(s.readers, {})) wake(r);
        }
        break;
      case op_fwrite: {
        h.io_busy = false;
        host_peer & q = h.peers[0];
        if(c.res > 0) {
          h.offset += uint64_t(c.res);
          q.sent += std::size_t(c.res);
          if(q.sent >= q.out.front().size()) {
            q.out.pop_front();
            q.sent = 0;
          }
        } else if(!q.out.empty()) q.out.clear(); // record
        file_write_next(sid);
        break;
      }
    }
  }
  // host stream: Closing cancels orders and closes descriptors.
  void close_host(uint64_t sid) {
    host_rec * h = host_of(sid);
    if(!h) return;
    if(h->listen_fd >= 0) {
      io({}).cancel_fd(h->listen_fd);
      ::close(h->listen_fd);
      h->listen_fd = -1;
    }
    for(auto & pr: h->peers)
      if(pr.fd >= 0) {
        io({}).cancel_fd(pr.fd);
        ::close(pr.fd);
        pr.fd = -1;
        pr.open = false;
      }
    if(h->file_fd >= 0) {
      ::close(h->file_fd);
      h->file_fd = -1;
    }
  }
  // net *, file * - like an interpreter (io_op_call): creating and operating a host stream
  mval host_op(std::string_view n, std::vector<mval> const & a, span where) {
    auto str_arg = [&](std::size_t i) { return std::string(a[i].as<m_str>()); };
    // host resources are only given (step 12.4, like the interpreter): the background - nothing, the program
    // - what the author gave (grants; proc grant / revoke - on the go); not given - "0 devices"
    if((n == "net_listen" || n == "net_connect" || n == "file_open" || n == "file_create") && !machine_granted(n.starts_with("file_") ? "fs" : "net")) {
      std::string_view who = fib().proc ? "процессу не дана (ресурс даёт программа: передаёт поток)" : "программе не дана (даёт пользователь: vish run --grant fs)";
      if(n.starts_with("file_")) rt_fail("io_error", cat(a.empty() || !a[0].is<m_str>() ? std::string("?") : str_arg(0), ": нет файла — файловая система ", who), where);
      st.streams.emplace_back(); // Network without devices: no one to listen to, nowhere to connect
      uint64_t sid = st.streams.size() - 1;
      if(n == "net_connect") deliver(sid, event(0, "close", fib().proc ? "сеть процессу не дана" : "сеть программе не дана"));
      return stream_value(sid);
    }
    if(n == "net_listen") {
      if(a.size() != 1 || !a[0].is<int64_t>()) throw diagnostic("listen(порт)", where);
      int fd = os::listen_tcp(int(a[0].as<int64_t>()));
      if(fd < 0) rt_fail("io_error", cat("listen: ", std::strerror(errno)), where);
      uint64_t sid = host_stream(3);
      host[sid].listen_fd = fd;
      ++io_pending;
      io(where).accept_all(fd, io_tag(op_accept, sid, 0));
      return stream_value(sid);
    }
    if(n == "net_connect") {
      if(a.size() != 2 || !a[0].is<m_str>() || !a[1].is<int64_t>()) throw diagnostic("connect(адрес, порт)", where);
      uint64_t sid = host_stream(3);
      host_rec & h = host[sid];
      if(!os::ipv4(str_arg(0), int(a[1].as<int64_t>()), h.addr)) rt_fail("io_error", "connect: неверный адрес", where);
      int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if(fd < 0) rt_fail("io_error", cat("connect: ", std::strerror(errno)), where);
      h.peers.push_back({fd});
      ++io_pending;
      io(where).connect(fd, &h.addr, io_tag(op_connect, sid, 0));
      return stream_value(sid);
    }
    if(n == "file_open" || n == "file_create") {
      if(a.size() != 1 || !a[0].is<m_str>()) throw diagnostic(cat(n == "file_open" ? "open" : "create", "(путь)"), where);
      std::string path = str_arg(0);
      int fd = n == "file_open" ? ::open(path.c_str(), O_RDONLY | O_CLOEXEC) : ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
      if(fd < 0) rt_fail("io_error", cat(path, ": ", std::strerror(errno)), where);
      uint64_t sid = host_stream(n == "file_open" ? 1 : 2);
      st.streams[sid].cap = 16;
      host[sid].file_fd = fd;
      host[sid].peers.push_back({});
      return stream_value(sid);
    }
    uint64_t sid = stream_id(a[0], where);
    host_rec * h = host_of(sid);
    if(n == "net_port") return mval{int64_t(h && h->listen_fd >= 0 ? os::bound_port(h->listen_fd) : 0)};
    if(n == "net_send") { // In the outgoing stream: does not wait (the core will send)
      if(a.size() != 3 || !a[1].is<int64_t>() || !a[2].is<m_str>()) throw diagnostic("send(сеть, peer, байты)", where);
      if(!h) return mval{false}; // The connection did not survive the transfer of the process
      auto p = std::size_t(a[1].as<int64_t>());
      if(st.streams[sid].host != 3 || p >= h->peers.size()) rt_fail("io_error", cat("send: нет соединения ", p), where);
      if(!h->peers[p].open) return mval{false};
      h->peers[p].out.push_back(str_arg(2));
      if(h->connected || h->listen_fd >= 0) send_next(sid, p);
      return mval{true};
    }
    if(n == "net_drop") {
      auto p = std::size_t(a[1].as<int64_t>());
      if(h && p < h->peers.size() && h->peers[p].fd >= 0) {
        io(where).cancel_fd(h->peers[p].fd);
        h->peers[p].open = false;
        ::close(h->peers[p].fd);
        h->peers[p].fd = -1;
      }
      return mval{m_unit{}};
    }
    throw diagnostic(cat("неизвестная операция ", n), where);
  }

  // --- VM processes (§9.2, M3c; step 12.1) ---

  // root of the current process name domain: process args on top of general globals
  uint32_t proc_globals() { return fib().globals != m_none ? fib().globals : st.globals; }
  // process(f, аргументы…): process input fiber is f; arguments are data only
  mval start_proc(node_list const & args, std::vector<mval> & a, span where) {
    if(args.empty() || !db.is<n_ident>(args[0])) throw diagnostic("process(функция, аргументы…)", where);
    auto it = helper.functions.find(db.as<n_ident>(args[0]).name);
    if(it == helper.functions.end()) throw diagnostic(cat("process: нет функции ", helper.str(db.as<n_ident>(args[0]).name)), where);
    for(std::size_t i = 1; i < a.size(); ++i)
      if(!transferable(a[i])) throw diagnostic(cat("process: аргумент типа ", describe(a[i]), " не передаётся между процессами (только данные)"), db[args[i]].where);
    st.procs.emplace_back();
    uint32_t pid = uint32_t(st.procs.size());
    uint32_t root_scope = new_scope(st.globals);
    m_arr av;
    av.items.push_back(mval{std::string_view("process")});
    declare(root_scope, helper.sym("args"), mval{std::move(av)});
    uint32_t id = uint32_t(st.fibers.size());
    st.fibers.emplace_back();
    st.fibers[id].proc = pid;
    st.fibers[id].globals = root_scope;
    st.fibers[id].proc_entry = true;
    uint32_t me = std::exchange(st.current, id);
    for(std::size_t i = 1; i < a.size(); ++i) fib().vals.push_back(std::move(a[i]));
    invoke(it->second, uint32_t(a.size() - 1), 0, where);
    st.current = me;
    st.ready.push_back(id);
    m_agg g{helper.types.named("process"), {}};
    g.fields.push_back({helper.sym("id"), mval{int64_t(pid - 1)}});
    return mval{std::move(g)};
  }
  m_proc & proc_of(mval const & v, span where) {
    mval x = deref(v);
    if(x.is<m_agg>())
      for(auto const & f: x.as<m_agg>().fields)
        if(helper.str(f.name) == "id" && f.v.is<int64_t>() && uint64_t(f.v.as<int64_t>()) < st.procs.size()) return st.procs[std::size_t(f.v.as<int64_t>())];
    throw diagnostic("ожидался процесс", where);
  }
  // end of the process (input fiber finished or fell): waiters
  void proc_finished(uint32_t pid) {
    m_proc & m = st.procs[pid - 1];
    m.done = true;
    for(uint32_t w: std::exchange(m.waiters, {})) wake(w);
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
  // host waiting outside (editor process, F5): everyone is waiting for host - the machine does not
  // sleep in its io_uring, but gives control; the ring is waiting for the one who executes it (the
  // server), and calls the pump. .
  bool external_wait = false;
  bool host_waiting() { return external_wait && !st.done && fib().state != f_ready && st.ready.empty() && io_pending > 0; }
  int host_fd() const { return ring ? ring->ring.ring_fd : -1; }
  void pump() {
    if(!ring) return;
    for(auto const & c: ring->peek()) io_complete(c);
    if(fib().state != f_ready) schedule();
  }
  // The next one is ready; no - everyone is waiting: mutual lock, error - in the main.
  void schedule() {
    while(!st.ready.empty() && st.fibers[st.ready.front()].state != f_ready) st.ready.erase(st.ready.begin());
    while(st.ready.empty() && io_pending > 0 && fib().state != f_ready) { // We are all waiting for host: sleep in io_uring
      if(external_wait) return;
      for(auto const & c: ring->wait()) io_complete(c);
      while(!st.ready.empty() && st.fibers[st.ready.front()].state != f_ready) st.ready.erase(st.ready.begin());
    }
    if(!st.ready.empty()) {
      st.current = st.ready.front();
      st.ready.erase(st.ready.begin());
      if(fib().proc) { // quantum - from obtaining the nucleus
        preempt_left = helper.slice_points ? helper.slice_points : 1024;
        if(!helper.slice_points) slice_start = os::uring::now_ms();
      }
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
      for(auto & f: st.fibers)
        if(f.error.present) {
          f.error.present = false; // The rest of the step will add to it.
          throw restore_error(f.error);
        }
      st.done = true;
      if(!fib().vals.empty() && deref(fib().vals.back()).is<int64_t>()) st.exit_code = deref(fib().vals.back()).as<int64_t>();
      return;
    }
    fib().state = f_done;
    if(fib().proc_entry) proc_finished(fib().proc);
    for(uint32_t j: std::exchange(st.joiners, {})) wake(j);
    schedule();
  }
  static diagnostic restore_error(m_error const & e) {
    diagnostic d(std::string(e.what), e.where, diag_kind::runtime);
    d.type = std::string(e.type);
    for(auto const & t: e.trace) d.trace.push_back({std::string(t.what), t.where});
    return d;
  }
  // The uncaught error fiber (not main) is its outcome; the program lives to wait in the main.
  void fiber_error(diagnostic const & d) {
    auto & f = fib();
    if(f.proc) { // Error of the VM process - its outcome, the program lives
      m_proc & m = st.procs[f.proc - 1];
      if(!m.failed) m.error = m_str(d.what());
      m.failed = true;
      if(f.proc_entry) proc_finished(f.proc);
    } else {
      f.error = {true, m_str(d.what()), d.where, m_str(d.type), {}};
      for(auto const & t: d.trace) f.error.trace.push_back({m_str(t.what), t.where});
    }
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
    if(!elab_out.empty()) { // ct modules printed during elaboration
      if(captured) *captured += elab_out;
      else {
        put(stdout, elab_out);
        std::fflush(stdout);
      }
      elab_out.clear();
    }
    st.res_on = helper.res_any();
    if(input) {
      st.queued_input = true;
      for(auto const & l: *input) st.input.emplace_back(l);
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
    helper.check_before_run(); // parsing before execution: errors of hooks parsing - before the first step
    for(auto const & [k, v]: helper.ct_memo)
      if(portable(v)) st.ct_memo.push_back({node_id(k.first), k.second, from_value(v, {})});
    std::vector<node_id> modules = helper.module_order; // Variables of all modules of the program (the last one)
    if(modules.empty()) modules.push_back(root);
    for(node_id m: modules)
      for(node_id item: db.as<n_module>(m).items)
        if(db.is<n_var>(item)) {
          auto const & v = db.as<n_var>(item);
          if(!v.ct) continue; // Outside of non-ct functions is parsing error; it doesn't get there
          value * x = helper.globals->find(v.name);
          if(!x) throw diagnostic(cat("ct ", helper.str(v.name), ": нет значения"), db[item].where);
          st.ct_vars.push_back(v.name);
          // the result, depending on the forks (branches give different), is not a value: read it ct- node from the memo
          if(portable(*x)) declare(st.globals, v.name, from_value(*x, db[item].where));
        }
    for(node_id item: db.as<n_module>(root).items) // [[state]] - value from the state of the program
      if(db.is<n_var>(item) && !db.as<n_var>(item).attrs.empty()) {
        value * x = helper.globals->find(db.as<n_var>(item).name);
        if(x) declare(st.globals, db.as<n_var>(item).name, from_value(*x, db[item].where));
      }
    for(node_id m: modules) // VM services (step 12.4): their state is common to machine processes
      if(helper.is_service(m))
        for(node_id item: db.as<n_module>(m).items)
          if(symbol nm = service_var(item)) {
            value * x = helper.globals->find(nm);
            if(x) declare(st.globals, nm, from_value(*x, db[item].where));
            st.service_names.push_back(nm);
          }
    auto it = helper.functions.find(helper.sym("main"));
    if(it == helper.functions.end()) throw diagnostic("нет функции main", {});
    invoke(it->second, 0, 0, db[it->second.front()].where);
  }

  // Step: One phase of the upper task; false, the process is complete. Error main - with fiber
  // errors that it did not wait for (like the interpreter).
  bool step() {
    try {
      return step_one();
    } catch(diagnostic & d) {
      for(auto & f: st.fibers)
        if(f.error.present) {
          d.also.push_back(restore_error(f.error));
          f.error.present = false;
        }
      throw;
    }
  }
  bool step_one() {
    if(st.done) return false;
    if(external_wait && fib().state != f_ready) { // waiting for host: ready to take, otherwise there is no step
      pump();
      if(fib().state != f_ready) return true;
    }
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
        case m_path_ref: path_ref_step(); break;
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
    if(preempt_fiber != m_none) { // Displacement - between steps, if fiber is still its own and ready
      bool mine = preempt_fiber == st.current && fib().state == f_ready && !st.ready.empty();
      preempt_fiber = m_none;
      if(mine) {
        ++preemptions;
        yield();
      }
    }
    return true;
  }

  std::optional<diagnostic> pending_error; // Original error VM flying exception (not process state)

  void run(uint64_t max_steps = ~uint64_t{0}) {
    while(max_steps-- > 0 && step()) {}
    if(st.done) save_state_vars(true);
  }

  // [[state]] (§12.2): The value of the variable is in the program state cell (its level) at
  // completion and the dump when the interpreter resets the state to the disk.
  bool state_saved = false;
  void save_state_vars(bool close) {
    if(state_saved || !helper.state_level || st.globals == m_none) return;
    for(node_id item: db.as<n_module>(root).items)
      if(db.is<n_var>(item) && !db.as<n_var>(item).attrs.empty()) {
        symbol name = db.as<n_var>(item).name;
        place p = find(st.globals, name);
        value * cell = helper.globals->find(name);
        if(p.scope == m_none || !cell) continue;
        *cell = adopt_into(to_value(cell_value(p)), &*helper.state_level);
      }
    if(close) {
      helper.save_program_state();
      state_saved = true;
    } else save_state(*helper.state_level);
  }
  mval cell_value(place p) { return deref(st.scopes[p.scope].vars[p.slot].v); }

  // --- resources§12.2): rules of the interpreter
  // --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- destroy(T) и copy(T)
  // The program is performed by the assistant: a function over its parameter (there are no variables of the module during execution), the output is performed in the output of the process.
  value helper_call(node_id fn, mval const & v, span where, std::string what) {
    std::string * was = helper.captured;
    helper.capture_output(captured);
    struct back {
      interpreter & h;
      std::string * w;
      ~back() { h.capture_output(w); }
    } restore{helper, was};
    std::vector<value> vals{to_value(v)};
    auto r = helper.invoke(fn, vals, {}, helper.globals, {where, std::move(what), 0, 0});
    if(r.kind == interpreter::flow_kind::thr) throw diagnostic("исключение из destroy/copy", where);
    return r.v;
  }
  bool is_shared_v(mval const & v) const { return v.is<m_agg>() && v.as<m_agg>().rid && helper.types.name(v.as<m_agg>().type) == "shared"; }
  m_shared & slot_of(mval const & v) { return st.shared[std::size_t(v.as<m_agg>().fields[0].v.as<int64_t>())]; }
  bool has_res(mval const & v) const {
    if(v.is<m_agg>()) {
      if(v.as<m_agg>().rid) return true;
      for(auto const & f: v.as<m_agg>().fields)
        if(has_res(f.v)) return true;
    } else if(v.is<m_arr>())
      for(auto const & e: v.as<m_arr>().items)
        if(e.is<m_agg>() || e.is<m_arr>()) {
          if(has_res(e)) return true;
        } else return false; // scalar
    return false;
  }
  // new value of the type with destroy - resource (and attached, came from the assistant without numbers)
  void mark_deep(mval & v) {
    if(!st.res_on) return;
    if(v.is<m_arr>())
      for(auto & e: v.as<m_arr>().items) mark_deep(e);
    if(!v.is<m_agg>()) return;
    auto & g = v.as<m_agg>();
    for(auto & f: g.fields) mark_deep(f.v);
    if(!g.rid && helper.member_fn_of("destroy", g.type)) {
      g.rid = ++st.res_counter;
      st.res_live.push_back(g.rid);
    }
  }
  void drop_value(mval const & v, span where) {
    if(v.is<m_arr>()) {
      auto const & xs = v.as<m_arr>().items;
      for(std::size_t i = xs.size(); i-- > 0;) drop_value(xs[i], where);
      return;
    }
    if(!v.is<m_agg>()) return;
    auto const & g = v.as<m_agg>();
    if(g.rid) {
      auto it = std::ranges::find(st.res_live, g.rid);
      if(it == st.res_live.end()) throw diagnostic(cat("ресурс ", helper.types.name(g.type), " уже освобождён: копия прошла мимо правила"), where);
      st.res_live.erase(it);
      if(is_shared_v(v)) { // shared: dec, the latter releases the stored
        m_shared & sl = slot_of(v);
        if(sl.count > 0 && --sl.count == 0) {
          mval held = std::move(sl.v);
          sl.v = mval{m_unit{}};
          drop_value(held, where);
        }
        return;
      }
      if(node_id d = helper.member_fn_of("destroy", g.type)) helper_call(d, v, where, cat("destroy(", helper.types.name(g.type), ")"));
    }
    for(std::size_t i = g.fields.size(); i-- > 0;) drop_value(g.fields[i].v, where);
  }
  // Transfer from the name: no further read; parameter - alias - and the caller on the chain (call
  // node: used_after looks the entire instruction) as an interpreter. alias field or element is part
  // of someone else’s meaning: copy.
  bool movable(node_id src, symbol name, place p) {
    if(helper.used_after(src, name)) return false;
    for(m_var * v = &st.scopes[p.scope].vars[p.slot]; v->v.is<m_alias>();) {
      m_alias const & al = v->v.as<m_alias>();
      if(al.index != m_none || al.field != m_none || al.path_scope != m_none) return false;
      uint32_t site = st.scopes[p.scope].site;
      m_var & to = st.scopes[al.scope].vars[al.slot];
      if(!site || helper.used_after(node_id(site), to.name)) return false;
      p = {al.scope, al.slot};
      v = &to;
    }
    return true;
  }
  // the value of the name src goes to a new place: the name is no longer read - transfer (name
  // hidden), reads - copy(T) of the program (shared - inc), no it - error
  void take_from(node_id src, mval & v, uint32_t scope) {
    if(!st.res_on || !db.is<n_ident>(src) || !has_res(v)) return;
    symbol name = db.as<n_ident>(src).name;
    span where = db[src].where;
    if(place p = find(scope, name); p.scope != m_none && movable(src, name, p)) {
      var_of(p).hidden = src.value + 1;
      return;
    }
    if(is_shared_v(v)) {
      ++slot_of(v).count;
      v.as<m_agg>().rid = ++st.res_counter;
      st.res_live.push_back(v.as<m_agg>().rid);
      return;
    }
    node_id c = v.is<m_agg>() ? helper.member_fn_of("copy", v.as<m_agg>().type) : node_id{};
    if(!c) {
      std::string tn = describe(v);
      throw diagnostic(cat("значение ", tn, " — ресурс: ", helper.str(name), " читается дальше, а копировать его нечем — объявите copy(", tn, " x) или не используйте ", helper.str(name), " после"), where);
    }
    v = from_value(helper_call(c, v, where, cat("copy(", describe(v), ")")), where);
    mark_deep(v);
  }
  mval make_shared(mval v) {
    st.res_on = true;
    st.shared.push_back({std::move(v), 1});
    m_agg g{helper.types.named("shared"), {}};
    g.fields.push_back({helper.sym("id"), mval{int64_t(st.shared.size() - 1)}});
    g.rid = ++st.res_counter;
    st.res_live.push_back(g.rid);
    return mval{std::move(g)};
  }

  // --- live process (§9.3): clone test, hot replacement, migration ---

  // Sample:
//.
  // The expression is considered in the region and frame of the upper problem of the current fiber (local names
  // are visible); the result is the output and value. Call in the clone (fork session): the state changes.
  std::string probe_here(node_id expr, uint64_t max_steps) {
    uint32_t me = st.current;
    m_fiber & f = fib();
    m_task const * t = f.tasks.empty() ? nullptr : &f.tasks.back();
    uint32_t scope = t && t->scope != m_none ? t->scope : proc_globals();
    uint32_t frame = t ? t->frame : 0;
    std::size_t depth = f.tasks.size();
    std::string out;
    captured = &out;
    push_eval(expr, scope, frame);
    try {
      while(max_steps-- > 0 && st.fibers[me].tasks.size() > depth && step()) {}
    } catch(diagnostic const & d) {
      return out + render(d, name, db);
    }
    if(st.fibers[me].tasks.size() > depth) return out + "проба: не закончилась за отведённые шаги\n";
    if(st.fibers[me].vals.empty()) return out;
    return out + helper.print_text(to_value(st.fibers[me].vals.back())) + "\n";
  }

  // Where the process stands: step and frames of the current fiber (bottom up).
  std::string where_now() {
    std::string r = cat("шаг ", st.steps, st.done ? ", завершён" : "", "; кадры:");
    for(auto const & fr: fib().frames) r += cat(" ", helper.str(fr.name));
    return r + "\n";
  }

  // All values of the process: variables of living areas, stacks of values fiber, signals,
  // buffers of flows; f sees and nested (elements, fields) - first internal.
  void each_value(auto && f) {
    auto walk = [&](auto && self, mval & x) -> void {
      if(x.is<m_arr>())
        for(auto & e: x.as<m_arr>().items) self(self, e);
      else if(x.is<m_agg>())
        for(auto & fl: x.as<m_agg>().fields) self(self, fl.v);
      f(x);
    };
    for(auto & sc: st.scopes)
      if(sc.live)
        for(auto & v: sc.vars) walk(walk, v.v);
    for(auto & fb: st.fibers) {
      for(auto & v: fb.vals) walk(walk, v);
      walk(walk, fb.pending.v);
    }
    for(auto & sm: st.streams)
      for(auto & v: sm.buf) walk(walk, v);
  }
  // The layout is the names and types of fields (the tail of step 10: f64 instead of i64 for the
  // same name is also a different layout, the value must be transferred migrate).
  struct field_decl {
    symbol name;
    std::string type; // field-type
  };
  bool same_layout(m_agg const & g, std::vector<field_decl> const & fields) {
    if(g.fields.size() != fields.size()) return false;
    for(std::size_t i = 0; i < fields.size(); ++i)
      if(g.fields[i].name != fields[i].name || !field_fits(g.fields[i].v, fields[i].type)) return false;
    return true;
  }
  bool field_fits(mval const & v, std::string_view t) {
    if(t.empty()) return true;
    std::string d = describe(v);
    if(d == t || d.starts_with(cat(t, "<"))) return true;
    return v.is<m_agg>() && helper.types.is_a(helper.types.name(v.as<m_agg>().type), t); // heir
  }
  // After editing the living process (§9.3, tail of step 10): ct of the new code - parsing of the
  // program again in canonical order, the results - in memory of the process; the former node - the
  // former (started frames live in the old body). The new body read the ct-variable by the final
  // elaboration value rather than by place (§7.3).
  void refresh_ct() {
    interpreter a(db, root, std::vector<std::string>{name}, mode::run);
    a.check_before_run();
    for(auto const & [k, v]: a.ct_memo) {
      if(!portable(v)) continue;
      mval x = from_value(v, {});
      auto it = std::ranges::find_if(st.ct_memo, [&](m_ct const & m) { return m.node.value == k.first && m.ctx == k.second; });
      if(it != st.ct_memo.end()) it->v = std::move(x);
      else st.ct_memo.push_back({node_id(k.first), k.second, std::move(x)});
    }
  }
  // Living values of type t with old layout (fields not as fields)
  uint64_t stale_values(type_id t, std::vector<field_decl> const & fields) {
    uint64_t n = 0;
    each_value([&](mval & x) {
      if(x.is<m_agg>() && x.as<m_agg>().type == t && !same_layout(x.as<m_agg>(), fields)) ++n;
    });
    return n;
  }
  // 
// (§9.3): each value of the old layout is via migrate program (assistant)
  // It performs it as an ordinary call; the fields of the old meaning are read by name.
  uint64_t migrate_values(type_id t, std::vector<field_decl> const & fields, node_id fn) {
    uint64_t n = 0;
    each_value([&](mval & x) {
      if(!x.is<m_agg>() || x.as<m_agg>().type != t || same_layout(x.as<m_agg>(), fields)) return;
      std::vector<value> vals{to_value(x)};
      span where = db[fn].where;
      auto r = helper.invoke(fn, vals, {}, helper.globals, {where, "migrate", 0, 0});
      if(!r.normal()) throw diagnostic("migrate: не вернула значение", where);
      mval y = from_value(r.v, where);
      if(!y.is<m_agg>() || y.as<m_agg>().type != t || !same_layout(y.as<m_agg>(), fields))
        throw diagnostic(cat("migrate: ожидалось значение новой раскладки ", helper.types.name(t)), where);
      x = std::move(y);
      ++n;
    });
    return n;
  }

  // Autotransfer: without migrate, the value of the old layout becomes the new struct st as the {поле:
  // значение} entry - the “write → struct” (§5.8) rules: fields by name, field type by implicit
  // presentation, new field by default. The field is removed, the type is not led, there is no default
  // value, struct has a base - you can not (data would be lost or invented): you need migrate. The result
  // is "(you can; apply - transferred) or why not; transfer - only if you can do everything (first
  // sample)." My rule.
  //
  std::string auto_migrate(type_id t, node_id st, bool apply) {
    if(apply)
      if(std::string why = auto_migrate(t, st, false); !why.empty()) return why;
    auto const & sd = db.as<n_extend>(st);
    if(sd.base) return "у struct есть база — нужна migrate";
    auto fields = struct_layout(st);
    node_list vars;
    for(node_id x: sd.items)
      if(db.is<n_var>(x)) vars.push_back(x);
    std::string why;
    each_value([&](mval & x) {
      if(!why.empty() || !x.is<m_agg>() || x.as<m_agg>().type != t || same_layout(x.as<m_agg>(), fields)) return;
      value old = to_value(x);
      auto const & have = old.as<agg_v>().fields;
      for(auto const & [k, _]: have)
        if(std::ranges::none_of(vars, [&](node_id f) { return db.as<n_var>(f).name == k; })) {
          why = cat("поле ", db.strings.text(k), " убрано — нужна migrate");
          return;
        }
      agg_v g{t, {}};
      for(node_id f: vars) {
        auto const & d = db.as<n_var>(f);
        value const * got = nullptr;
        for(auto const & [k, fv]: have)
          if(k == d.name) got = &fv;
        value fv;
        if(got && d.type && db.is<n_ident>(d.type)) {
          auto q = helper.type_fit(*got, db.strings.text(db.as<n_ident>(d.type).name), false, fv);
          if(q == interpreter::fit::no || q == interpreter::fit::maybe) {
            why = cat("поле ", db.strings.text(d.name), ": ", helper.types.describe(*got), " не приводится к ", db.strings.text(db.as<n_ident>(d.type).name), " — нужна migrate");
            return;
          }
        } else if(got) fv = *got;
        else if(d.init) {
          auto r = helper.eval(d.init, helper.globals, 0);
          if(!r.normal()) {
            why = cat("значение по умолчанию поля ", db.strings.text(d.name), " не вычислилось");
            return;
          }
          fv = helper.field_typed(d, std::move(r.v), db[d.init].where);
        } else {
          why = cat("у нового поля ", db.strings.text(d.name), " нет значения по умолчанию — нужна migrate");
          return;
        }
        g.fields.push_back({d.name, std::move(fv)});
      }
      if(apply) x = from_value(value(std::move(g)), db[st].where);
    });
    return why;
  }
  std::vector<field_decl> struct_layout(node_id st) {
    std::vector<field_decl> fields;
    for(node_id x: db.as<n_extend>(st).items)
      if(db.is<n_var>(x)) {
        node_id ty = db.as<n_var>(x).type;
        fields.push_back({db.as<n_var>(x).name, ty && db.is<n_ident>(ty) ? std::string(db.strings.text(db.as<n_ident>(ty).name)) : std::string()});
      }
    return fields;
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
      if(t.kind == m_call_body && s.kind == sig_tail && s.target == t.a) { // transition: a new frame instead
        node_id fn = t.node;
        uint32_t caller = !f.frames.empty() && f.frames.back().id == t.a ? f.frames.back().caller : 0;
        mval packed = std::move(s.v);
        s.kind = sig_none;
        end_frame(t);
        release_scope(t.owned);
        f.vals.resize(t.base);
        f.tasks.pop_back();
        uint32_t n = uint32_t(packed.as<m_arr>().items.size());
        for(auto & v: packed.as<m_arr>().items) fib().vals.push_back(std::move(v));
        safe_point(true); // rib-back
        return invoke(std::vector<node_id>{fn}, n, caller, db[fn].where);
      }
      if(t.kind == m_call_body && s.kind == sig_return && s.target == t.a) {
        mval v = result_in_type(t.node, std::move(s.v));
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
    auto at = std::ranges::find_if(f.tasks.rbegin(), f.tasks.rend(), [&](m_task const & x) { return x.kind == m_eval && x.phase == 1 && db.try_compile_body(x.node); });
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
  uint32_t access_scope = m_none; // unpacking area - for expressions in sample appeals
  void invoke(std::vector<node_id> const & fns, uint32_t nargs, uint32_t caller_frame, span where, bool repacked = false) {
    safe_point(false); // challenge
    auto & f = fib();
    std::vector<mval> args(f.vals.end() - nargs, f.vals.end());
    node_id chosen = override_for(fns, args); // Operation in context (checked) - its redefinition
    bool plain = bool(chosen);
    bool tie = false;
    uint32_t parent = std::exchange(invoke_parent, m_none); // local set: environment of the set
    uint32_t rejected = 0, by_value = 0; // All are cut off by meaning - an exception, like the interpreter.
    std::vector<std::pair<uint32_t, mval>> chosen_conv;
    // Appropriate Candidates and Their Features - Rule of Specificity in the Interpreter (overload_pick)
    struct good_one {
      node_id fn;
      std::vector<int64_t> feat;
      std::vector<std::pair<uint32_t, mval>> conv;
    };
    std::vector<good_one> good;
    for(node_id fnode: fns) {
      auto const & fn = db.as<n_fn>(fnode);
      if(plain) continue;
      bool tail = helper.rest_param(fnode);
      if(tail ? nargs + 1 < fn.params.size() : fn.params.size() != nargs) {
        ++rejected;
        continue;
      }
      std::vector<int64_t> feat{tail ? 1 : 0, 0}; // interpreter-like signs
      bool fits = true;
      std::vector<std::pair<uint32_t, mval>> conv; // candidate's converted arguments
      for(uint32_t i = 0; i < fn.params.size() && fits; ++i) {
        auto const & p = db.as<n_param>(fn.params[i]);
        if(tail && i + 1 == fn.params.size()) { // tail: T... xs - each element is represented in T
          if(p.type && db.is<n_ident>(p.type))
            for(uint32_t j = i; j < nargs && fits; ++j) {
              mval got;
              int r = type_fit(deref(args[j]), helper.str(db.as<n_ident>(p.type).name), p.strict, got);
              if(!r) fits = false;
              else if(r != 2) conv.push_back({j, got});
            }
          break;
        }
        if(p.type && db.is<n_pat>(p.type)) { // Applicant’s condition (as required)
          uint32_t probe = new_scope(parent == m_none ? proc_globals() : parent);
          bool ok = unpack_into(p.type, args[i], probe, where);
          release_scope(probe);
          if(!ok) {
            fits = false;
            ++by_value;
          } else feat.push_back(100 + int64_t(helper.pattern_checks(p.type)));
          continue;
        }
        if(p.mode == param_mode::out) { // location of the result: the value is given by the function
          feat.push_back(2);
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
            feat.push_back(r); // 2 - type equals, 3 - the same object under the general type, 1 - imagine
            if(r != 2) conv.push_back({i, got});
          }
        }
      }
      // Primitive operator - only "type equal", without i64 → f64 (§5.8, like an interpreter)
      if(fits && !conv.empty() && helper.intrinsic_operator(fnode)) fits = false;
      if(!fits) {
        ++rejected;
        continue;
      }
      if(has_leading_require(fn)) { // require at the beginning of the body - Candidate restriction (§5.2)
        int n = requires_hold(fnode, args, where, parent);
        if(n < 0) {
          ++rejected;
          ++by_value;
          continue;
        }
        feat[1] = n;
      }
      good.push_back({fnode, std::move(feat), std::move(conv)});
    }
    if(!plain && !good.empty()) {
      std::vector<std::vector<int64_t> const *> feats;
      std::vector<bool> maybes;
      for(auto const & g: good) {
        feats.push_back(&g.feat);
        maybes.push_back(false); // machine
      }
      auto const & pick = helper.choose_overload(feats, maybes, where);
      chosen = good[pick.order[0]].fn;
      chosen_conv = std::move(good[pick.order[0]].conv);
      tie = pick.ambiguous;
    }
    // cortege, like the interpreter: no one directly approached
    if(!chosen && !repacked) {
      bool pack = std::ranges::any_of(fns, [&](node_id fn) {
        auto const & ps = db.as<n_fn>(fn).params;
        return ps.size() == 1 && db.as<n_param>(ps[0]).type && db.is<n_pat>(db.as<n_param>(ps[0]).type) && db.as<n_pat>(db.as<n_param>(ps[0]).type).type &&
            helper.str(db.as<n_ident>(db.as<n_pat>(db.as<n_param>(ps[0]).type).type).name) == "array";
      });
      if(pack && nargs != 1) { // f(1, 2) → f(array{1, 2})
        m_arr packed;
        for(auto & a: args) packed.items.push_back(deref(a));
        f.vals.resize(f.vals.size() - nargs);
        f.vals.push_back(mval{std::move(packed)});
        invoke_parent = parent;
        return invoke(fns, 1, caller_frame, where, true);
      }
      if(nargs == 1 && deref(args[0]).is<m_arr>()) { // g(t) → g(t[0], t[1], …)
        auto items = deref(args[0]).as<m_arr>().items;
        if(std::ranges::any_of(fns, [&](node_id fn) { return db.as<n_fn>(fn).params.size() == items.size() || (helper.rest_param(fn) && items.size() + 1 >= db.as<n_fn>(fn).params.size()); })) {
          f.vals.pop_back();
          for(auto & e: items) f.vals.push_back(std::move(e));
          invoke_parent = parent;
          return invoke(fns, uint32_t(items.size()), caller_frame, where, true);
        }
      }
    }
    if(!chosen && by_value && by_value == rejected) {
      diagnostic d(cat("нет перегрузки ", helper.str(db.as<n_fn>(fns.front()).name), " для этих значений"), where, diag_kind::runtime);
      d.type = "unpack_failed";
      throw d;
    }
    if(!chosen) {
      std::string_view name = helper.str(db.as<n_fn>(fns.front()).name);
      if((name.starts_with("infix") || name.starts_with("prefix")) && nargs >= 1 && nargs <= 2) // interpreter-like
        throw diagnostic(cat("оператор ", name.substr(name.starts_with("infix") ? 5 : 6), " не определён для ", describe(deref(args[0])), nargs == 2 ? cat(" и ", describe(deref(args[1]))) : std::string()), where);
      throw diagnostic(cat("нет подходящей перегрузки ", name), where);
    }
    if(tie) throw diagnostic("неоднозначный вызов", where);
    for(auto & [i, v]: chosen_conv) args[i] = std::move(v); // Values - in the types of parameters selected
    auto const & fn = db.as<n_fn>(chosen);
    if(fn.outline && helper.inline_required(chosen))
      throw diagnostic(cat(helper.str(fn.name), " помечена outline, но использует семантику инлайна"), db[chosen].where);
    // function-operator from one primitive - VM operation
    if(helper.intrinsic_operator(chosen)) {
      std::string prim(helper.str(db.as<n_ident>(db.as<n_call>(fn.body).callee).name));
      f.vals.resize(f.vals.size() - nargs);
      mval r = primitive(prim, args, where);
      f.vals.push_back(std::move(r));
      return;
    }
    // tail call itself (O5, like the interpreter): transition - the body of the same call again, the
    // frame does not grow (parameter values - copies: the caller frame leaves)
    if(site_node && helper.tail_self(chosen, site_node.value))
      for(auto it = f.tasks.rbegin(); it != f.tasks.rend(); ++it) {
        if(it->kind == m_lambda_body || it->kind == m_thunk_eval || it->kind == m_intercept) break; // Structure block, try not tail
        if(it->kind != m_call_body) continue;
        if(it->node != chosen) break;
        m_arr vs;
        for(auto & a: args) vs.items.push_back(deref(a));
        f.vals.resize(f.vals.size() - nargs);
        raise({sig_tail, mval{std::move(vs)}, it->a});
        return;
      }
    uint32_t id = ++st.frame_counter;
    f.frames.push_back({id, fn.name, fn.outline, caller_frame, plain});
    uint32_t sc = new_scope(parent == m_none ? proc_globals() : parent); // Local - on top of the surroundings of your block
    st.scopes[sc].caller = f.tasks.empty() ? m_none : f.tasks.back().scope; // call environment (O4)
    st.scopes[sc].fn = chosen.value;
    st.scopes[sc].site = site_node.value;
    for(uint32_t i = 0; i < fn.params.size(); ++i) {
      auto const & p = db.as<n_param>(fn.params[i]);
      if(p.pack && p.mode == param_mode::let && i + 1 == fn.params.size()) { // ...xs is the remainder of the argument
        m_arr rest;
        for(uint32_t j = i; j < nargs; ++j) rest.items.push_back(deref(args[j]));
        declare(sc, p.name, mval{std::move(rest)});
        break;
      }
      mval a = std::move(args[i]);
      if(a.is<m_alias>() && (a.as<m_alias>().index != m_none || a.as<m_alias>().field != m_none || a.as<m_alias>().path_scope != m_none) && p.mode == param_mode::own) a = deref(a);
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
  int requires_hold(node_id f, std::vector<mval> const & args, span where, uint32_t parent = m_none) {
    auto const & fn = db.as<n_fn>(f);
    // environment: captured names of the local function (parent) - transferred values
    node_list conds;
    for(node_id s: db.as<n_block>(fn.body).items) {
      if(!db.require_cond(s)) break;
      conds.push_back(db.require_cond(s));
    }
    scope * outer = helper_probe(conds, parent == m_none ? proc_globals() : parent);
    struct drop {
      interpreter & h;
      scope * s;
      ~drop() { h.drop_scope(s); }
    } hold_outer{helper, outer};
    scope * probe = helper.new_scope(outer);
    drop hold{helper, probe};
    std::vector<value> vals;
    for(auto const & a: args) {
      if(a.is<m_thunk>()) unsupported("require с expr-параметром", f);
      vals.push_back(to_value(a));
    }
    helper.bind_values(fn.params, vals, {}, probe, probe); // And the samples and the tail. xx
    int n = 0;
    for(node_id s: db.as<n_block>(fn.body).items) {
      if(!db.require_cond(s)) break;
      ++n;
      interpreter::flow c;
      try {
        c = helper.eval(db.require_cond(s), probe, 0);
      } catch(diagnostic const &) {
        return -1; // predicate not instance - no candidate
      }
      if(!c.v.is<bool>()) throw diagnostic("require не bool", where);
      if(!c.v.as<bool>()) return -1;
    }
    return n;
  }

  bool has_leading_require(n_fn const & fn) const {
    if(!fn.body || !db.is<n_block>(fn.body)) return false;
    auto const & items = db.as<n_block>(fn.body).items;
    return !items.empty() && db.require_cond(items[0]) && !db.is_assume(items[0]);
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
    mval r = result_in_type(t.node, deref(pop_val())); // a value, not a alias, on an area that is now freed up
    if(st.res_on) {
      if(!db.is<n_block>(body)) take_from(body, r, t.scope); // => x - transfer
      if(r.is<m_agg>() && r.as<m_agg>().rid && t.owned != m_none) // parameter returned by value - transfer
        for(auto & v: st.scopes[t.owned].vars)
          if(v.v.is<m_agg>() && v.v.as<m_agg>().rid == r.as<m_agg>().rid) v.hidden = 1;
    }
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
        {"rem", "%"}, {"band", "&"}, {"bor", "|"}, {"bxor", "^"}, {"shl", "<<"}, {"shr", ">>"}, {"eq", "=="}, {"ne", "!="}, {"lt", "<"}, {"gt", ">"}, {"le", "<="}, {"ge", ">="}};
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
    if(foreign_module_var(p, x.name, n)) p = {}; // Visibility hygiene and reading like an interpreter
    if(p.scope == m_none) {
      std::string_view s = helper.str(x.name);
      if(s == "true" || s == "false") return finish(mval{s == "true"});
      if(auto st = helper.structs.find(x.name); st != helper.structs.end()) { // entity struct - value (previously registry: there and others)
        if(node_id home = helper.module_of(st->second), m = helper.module_of(n); helper.module_order.size() > 1 && m && !helper.sees_directly(m, home, x.name))
          throw diagnostic(helper.not_visible("тип", x.name, home), db[n].where);
        return finish(mval{m_type{helper.types.named(s)}});
      }
      if(auto ty = helper.types.builtin(s)) return finish(mval{m_type{ty}});
      if(auto it = helper.functions.find(x.name); it != helper.functions.end()) {
        for(node_id f: it->second)
          if(helper.inline_required(f))
            throw diagnostic(cat("функция ", s, " использует семантику инлайна (expr-параметр или return(…)) и не может быть значением; вызывайте её напрямую"), db[n].where);
        return finish(mval{m_fn{x.name, {}, m_none}});
      }
      if(node_id home = module_var_home(x.name); home && find(t.scope, x.name).scope != m_none) throw diagnostic(helper.not_visible("переменная", x.name, home), db[n].where);
      throw diagnostic(cat("неизвестное имя ", s), db[n].where);
    }
    if(p.scope == st.globals && std::ranges::contains(st.ct_vars, x.name) && !in_ct()) // ct-variable - in place (§7.3)
      for(auto const & m: st.ct_memo)
        if(m.node == n && m.ctx == t.ctx) return finish(mval(m.v));
    not_hidden(p, x.name, db[n].where);
    mval & c = cell(p);
    if(c.is<m_thunk>()) { // expr-parameter - the expression of the caller in its area and frame
      m_thunk th = c.as<m_thunk>();
      if(db.is<n_lambda>(th.expr) && db.as<n_lambda>(th.expr).block)
        throw diagnostic("блок конструкции вводит имена — его вызывают с аргументами: body(x)", db[th.expr].where);
      t.kind = m_lambda_body; // The result of the expression is the result of this node
      t.extra = th.expr;
      t.scope = ruled_scope(th);
      if(t.scope != th.env) t.owned = t.scope;
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
      if(st.res_on && (db.is<n_call>(b.items[t.phase - 1]) || db.is<n_construct>(b.items[t.phase - 1]))) // Temporary discarded - released
        for(std::size_t i = t.base; i < fib().vals.size(); ++i)
          if(!fib().vals[i].is<m_alias>() && has_res(fib().vals[i])) drop_value(mval(fib().vals[i]), db[b.items[t.phase - 1]].where);
      fib().vals.resize(t.base); // The meaning of the instruction is not
    }
    if(t.phase < b.items.size()) {
      node_id item = b.items[t.phase++];
      push_eval(item, t.scope, t.frame);
      return;
    }
    if(b.has_value && !b.items.empty()) {
      mval v = deref(pop_val());
      take_from(b.items.back(), v, t.scope); // the name of the block - transfer
      return finish(std::move(v));
    }
    (void)n;
    finish(mval{m_unit{}});
  }

  // --- unpacking (P12), as in the interpreter; value-element - literal ---

  // The expression without the state of the process is to help: free names are
  // transferred from the scope (as for the chain tree and require).
  scope * helper_probe(node_list const & exprs, uint32_t scope_id) {
    scope * probe = helper.new_scope(helper.globals);
    std::vector<symbol> names;
    for(node_id e: exprs) helper.free_names(e, names);
    for(symbol s: names) {
      place p = find(scope_id, s);
      if(p.scope == m_none) continue;
      mval v = cell(p);
      if(v.is<m_thunk>() || v.is<m_fn>()) continue; // functions - the assistant has its own
      probe->declare(s, to_value(v));
    }
    return probe;
  }
  // nullopt - the calculation failed (VM error or exception): for the circulation of the sample - "not unpacked"
  std::optional<mval> helper_eval(node_id e, uint32_t scope_id, std::optional<mval> subject, span where) {
    scope * probe = helper_probe({e}, scope_id);
    struct drop {
      interpreter & h;
      scope * s;
      ~drop() { h.drop_scope(s); }
    } hold{helper, probe};
    if(subject) helper.subjects.push_back(to_value(*subject));
    struct pop {
      interpreter & h;
      bool on;
      ~pop() {
        if(on) h.subjects.pop_back();
      }
    } unsub{helper, bool(subject)};
    auto f = helper.eval(e, probe, 0);
    if(f.kind != interpreter::flow_kind::normal) return std::nullopt;
    return from_value(f.v, where);
  }

  // Levels of coincidence of type, as in the interpreter: 2 - type equals, 3 - the same object
  // under the common type (heir, record → struct), 1 - the value is conceivable (out - in the
  // type of parameter), 0 - no.
  int type_fit(mval const & v, std::string_view want, bool strict, mval & out) {
    out = v;
    if(is_shared_v(v) && want != "shared") { // Shared is suitable where stored (transparent)
      mval in;
      return type_fit(slot_of(v).v, want, strict, in) ? 2 : 0;
    }
    std::string have = describe(v);
    if(want == "auto" || have == want || have.substr(0, have.find('<')) == want) return 2; // identity is here
    // The ratio of unequal types is the same rule as that of the interpreter (fit_rule prelude, M6)
    value got;
    auto r = helper.type_fit(to_value(v), want, strict, got);
    if(r == interpreter::fit::no) return 0;
    if(r == interpreter::fit::exact) return 2;
    out = from_value(got, {});
    return r == interpreter::fit::subsumes ? 3 : 1;
  }
  // Anonymous literature in a place of a known type (as in the interpreter, §4.3).
  bool is_record(mval const & v) const { return v.is<m_agg>() && helper.types.anonymous(v.as<m_agg>().type); }
  mval to_type(mval v, std::string_view want, span where) {
    if(want.empty() || !is_record(v)) return v;
    return from_value(helper.to_type(to_value(v), want, where), where);
  }
  std::string place_type(mval const & cur) const {
    if(cur.is<int64_t>()) return std::string(helper.types.int_name()); // x = {} у i64 — 0
    if(cur.is<double>()) return std::string(helper.types.float_name());
    if(!cur.is<m_agg>() || helper.types.anonymous(cur.as<m_agg>().type)) return {};
    return std::string(helper.types.name(cur.as<m_agg>().type));
  }
  // point f() => {x: …} is the result of a function with type
  mval result_in_type(node_id f, mval v) {
    if(!db.is<n_fn>(f)) return v;
    node_id rt = db.as<n_fn>(f).result_type;
    if(!rt || !db.is<n_ident>(rt)) return v;
    return to_type(std::move(v), helper.str(db.as<n_ident>(rt).name), db[rt].where);
  }
  bool numeric_pair(mval const & v, std::string_view want) const { return (want == helper.types.float_name() && v.is<int64_t>()) || (want == helper.types.int_name() && v.is<double>()); }

  // Reference to the interface from the unpackable value: .field, .size, [literal]; failed
  // (no field, border) - nullopt: not unpacked.
  std::optional<mval> access(node_id e, mval const & subject) {
    if(db.is<n_subject>(e)) return subject;
    if(db.is_member(e)) {
      auto const & m = db.member_of(e);
      auto o = access(m.object, subject);
      if(!o) return std::nullopt;
      mval x = deref(*o);
      if(x.is<m_arr>() && helper.str(m.name) == "size") return mval{int64_t(x.as<m_arr>().items.size())};
      if(x.is<m_agg>())
        for(auto const & f: x.as<m_agg>().fields)
          if(f.name == m.name) return f.v;
      return std::nullopt;
    }
    if(db.is_index(e) && db.is<n_int>(db.index_of(e).index)) {
      auto o = access(db.index_of(e).object, subject);
      if(!o) return std::nullopt;
      mval x = deref(*o);
      int64_t k = db.as<n_int>(db.index_of(e).index).value;
      if(!x.is<m_arr>() || k < 0 || uint64_t(k) >= x.as<m_arr>().items.size()) return std::nullopt;
      return x.as<m_arr>().items[std::size_t(k)];
    }
    // .f() call, index expression - to the assistant over the value (error - not unpacked)
    try {
      return helper_eval(e, access_scope, subject, db[e].where);
    } catch(diagnostic const & d) {
      if(d.kind != diag_kind::runtime) throw;
      return std::nullopt;
    }
  }

  bool unpack_into(node_id pat, mval const & v0, uint32_t into, span where) {
    auto const & p = db.as<n_pat>(pat);
    std::string_view kind = p.type ? helper.str(db.as<n_ident>(p.type).name) : std::string_view();
    mval v = deref(v0);
    if(helper.node_kind_named(kind) || (v.is<m_agg>() && helper.types.name(v.as<m_agg>().type) == "code")) {
      // sample above the code (ast.if {c, …} = node) - unpacks the assistant, names - here
      scope * probe = helper_probe({pat}, into);
      scope * got = helper.new_scope(probe);
      struct drop {
        interpreter & h;
        scope * a;
        scope * b;
        ~drop() {
          h.drop_scope(a);
          h.drop_scope(b);
        }
      } hold{helper, got, probe};
      if(helper.unpack_into(pat, to_value(v), got, probe, 0, where) != interpreter::unpacked::yes) return false;
      for(auto const & [name, cellp]: got->vars) declare(into, name, from_value(*cellp, where));
      return true;
    }
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
      else { // expression - assistant (names - from the unpacking area)
        auto got = helper_eval(item, into, std::nullopt, db[item].where);
        if(!got) return false;
        lit = *got;
      }
      mval eq = primitive("eq", {e, lit}, db[item].where);
      return eq.is<bool>() && eq.as<bool>();
    };
    node_list pos, fields;
    for(node_id it: p.items) (db.is<n_pat_field>(it) ? fields : pos).push_back(it);
    auto interface = [&] {
      access_scope = into;
      for(node_id it: fields) {
        auto got = access(db.as<n_pat_field>(it).access, v);
        if(!got || !elem(db.as<n_pat_field>(it).sub, deref(*got))) return false;
      }
      return true;
    };
    if(!p.type) return v.is<m_agg>() && interface(); // {x: a, y: b} = v - addresses .x to the aggregate
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
      if(!v.is<m_agg>() || !helper.types.is_a(helper.types.name(v.as<m_agg>().type), kind)) return false; // heir
      auto const & fs = v.as<m_agg>().fields;
      std::size_t own = helper.fields_of_struct(db.as<n_ident>(p.type).name).size(); // basefields
      if(!pos.empty() && own != pos.size()) throw diagnostic(cat("образец ", kind, "{…}: полей ", own, ", элементов ", pos.size()), where);
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

  // Tag "text {x} text" ст tag ("text", x, "text")
  void eval_node(node_id n, n_native const &) {
    native_entry const * e = native_of(db, n);
    std::vector<value> a;
    for(node_id pn: db.as<n_fn>(native_owner(db, n)).params) {
      place p = find_lexical(top().scope, db.as<n_param>(pn).name);
      a.push_back(p.scope == m_none ? value() : to_value(deref(st.scopes[p.scope].vars[p.slot].v)));
    }
    finish(from_value(e->fn(helper, a, db[n].where), db[n].where));
  }

  // Raw block (§4.4): node parsing - in the assistant (ct code without a process state), the
  // result is calculated in the same area as the expression of the block
  void eval_node(node_id n, n_raw const & r) {
    if(!r.rule) throw diagnostic("подблок сырого блока не исполняется: его разбирает правило конструкции", db[n].where);
    m_task & t = top();
    t.kind = m_lambda_body;
    t.extra = helper.raw_result(n);
    t.phase = 0;
  }

  // ...t in arguments: meaning in its entirety; in arguments it unfolds call_args_step
  void eval_node(node_id, n_spread const & s) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(s.value, t.scope, t.frame);
    }
    finish(deref(pop_val()));
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
    safe_point(true); // rib-back
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
    if(r.value) take_from(r.value, v, t.scope); // return x - transfer
    uint32_t target = r.target ? return_target(db.as<n_ident>(r.target).name, t.frame, db[n].where) : t.frame;
    raise({sig_return, std::move(v), target});
  }

  void eval_member(node_id n, member_view const & m) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(m.object, t.scope, t.frame);
    }
    mval ov = pop_val();
    mval const * op = &ref_of(ov);
    if(is_shared_v(*op)) op = &slot_of(*op).v; // shared - transparently
    mval const & o = *op;
    if(o.is<m_agg>() && !helper.service_types.empty()) helper.service_guard(helper.types.name(o.as<m_agg>().type), n, "читать поля");
    if(is_code(o)) { // c.cond - node field of code: reads the assistant
      auto v = helper_eval(n, t.scope, std::nullopt, db[n].where);
      if(!v) throw diagnostic(cat("у узла кода нет поля ", helper.str(m.name)), db[n].where);
      return finish(std::move(*v));
    }
    if(helper.str(m.name) == "size") {
      if(o.is<m_arr>()) return finish(mval{int64_t(o.as<m_arr>().items.size())});
      if(o.is<m_str>()) return finish(mval{int64_t(o.as<m_str>().size())});
    }
    if(o.is<m_agg>())
      for(auto const & f: o.as<m_agg>().fields)
        if(f.name == m.name) return finish(f.v);
    // Tag above representation: value is the machine value itself (like an interpreter)
    if(helper.str(m.name) == "value" && (o.is<int64_t>() || o.is<double>() || o.is<bool>())) return finish(o);
    throw diagnostic(cat("у значения типа ", describe(o), " нет поля ", helper.str(m.name)), db[n].where);
  }

  // v.[f] is a field by descriptor (from fields(T)) or by line name (§7.4).
  void eval_field(node_id n, field_view const & fl) {
    m_task & t = top();
    if(t.phase < 2) {
      node_id next = t.phase == 0 ? fl.key : fl.object;
      ++t.phase;
      return push_eval(next, t.scope, t.frame);
    }
    mval o = deref(pop_val()), key = deref(pop_val());
    std::string name;
    if(key.is<m_str>()) name = key.as<m_str>();
    else if(key.is<m_agg>() && !key.as<m_agg>().fields.empty() && key.as<m_agg>().fields[0].v.is<m_str>()) name = key.as<m_agg>().fields[0].v.as<m_str>();
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

  void eval_index(node_id n, index_view const & ix) {
    m_task & t = top();
    if(t.phase < 2) {
      node_id next = t.phase == 0 ? ix.object : ix.index;
      ++t.phase;
      return push_eval(next, t.scope, t.frame);
    }
    mval iv = pop_val(), ov = pop_val();
    mval const & i = ref_of(iv);
    mval const & o = ref_of(ov);
    if(!i.is<int64_t>()) throw diagnostic("индекс должен быть i64", db[n].where);
    if(o.is<m_str>()) { // bytes: s[k] - i64
      auto const & str = o.as<m_str>();
      int64_t k = i.as<int64_t>();
      if(k < 0 || uint64_t(k) >= str.size()) out_of_bounds(k, str.size(), db[n].where);
      return finish(mval{int64_t(uint8_t(str[std::size_t(k)]))});
    }
    if(!o.is<m_arr>()) throw diagnostic(cat("индексация значения типа ", describe(o)), db[n].where);
    auto const & xs = o.as<m_arr>().items;
    int64_t k = i.as<int64_t>();
    if(k < 0 || uint64_t(k) >= xs.size()) out_of_bounds(k, xs.size(), db[n].where);
    finish(xs[uint64_t(k)]);
  }

  void eval_node(node_id n, n_op const & o) {
    if(db.is_member(n)) return eval_member(n, db.member_of(n));
    if(db.is_field(n)) return eval_field(n, db.field_of(n));
    if(db.is_index(n) && (!helper.user_ops.contains(helper.sym("infix[]").value) || db.in_prelude(n))) return eval_index(n, db.index_of(n));
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
    if(symbol e = helper.sym(entity); helper.local_ops.contains(e.value)) // operator declared in the block
      if(place p = find_lexical(t.scope, e); p.scope != m_none && cell(p).is<m_fn>() && !cell(p).as<m_fn>().set.empty()) {
        std::vector<node_id> fns(cell(p).as<m_fn>().set.begin(), cell(p).as<m_fn>().set.end());
        return eval_call_args(n, fns, o.args, true);
      }
    auto it = helper.functions.find(helper.sym(entity));
    if(it == helper.functions.end()) throw diagnostic(cat("оператор ", op, " не определён"), db[n].where);
    return eval_call_args(n, it->second, o.args, true);
  }

  bool is_code(mval const & v) const { return v.is<m_agg>() && helper.types.name(v.as<m_agg>().type) == "code"; }
  void eval_assign(node_id n, n_op const & o, std::string_view op) {
    m_task & t = top();
    node_id lhs = o.args[0];
    if(t.phase == 0 && db.is_member(lhs)) { // f.body = ... - editing live node code: assistant (node - common, in the database)
      node_id x = lhs;
      while(db.is_member(x)) x = db.member_of(x).object;
      if(db.is<n_ident>(x))
        if(place p = find(t.scope, db.as<n_ident>(x).name); p.scope != m_none && is_code(cell(p))) {
          if(!helper_eval(n, t.scope, std::nullopt, db[n].where)) throw diagnostic("правка узла кода не удалась", db[n].where);
          return finish(mval{m_unit{}});
        }
    }
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(o.args[1], t.scope, t.frame);
    }
    if(db.is<n_ident>(lhs)) {
      mval rhs = deref(pop_val());
      symbol name = db.as<n_ident>(lhs).name;
      place p = op == "=" ? find_lexical(t.scope, name) : find(t.scope, name); // x = e is a name that does not cause
      // Visibility hygiene (as interpreter::foreign_global): the variable module of the program is not
      // visible to the code of the module that does not see it (the body of the construction prelude).
      if(op == "=" && foreign_module_var(p, name, n)) p = {};
      if(op == "=") take_from(o.args[1], rhs, t.scope);
      if(p.scope == m_none) {
        if(op != "=") throw diagnostic(cat("неизвестное имя ", helper.str(name)), db[lhs].where);
        declare(t.scope, name, std::move(rhs)); // x = e is an announcement if there is no name.
        return finish(mval{m_unit{}});
      }
      // expr parameter - the code of the caller: the record goes to its place (x in swap(x) <->(y))
      while(p.scope != m_none && st.scopes[p.scope].vars[p.slot].v.is<m_thunk>()) {
        m_thunk const & th = st.scopes[p.scope].vars[p.slot].v.as<m_thunk>();
        if(!db.is<n_ident>(th.expr)) unsupported("запись в expr-параметр, чьё выражение — не имя", n);
        p = find(th.env, db.as<n_ident>(th.expr).name);
        if(p.scope == m_none) throw diagnostic("присваивание не в переменную", db[lhs].where);
      }
      if(p.scope == st.globals && std::ranges::contains(st.ct_vars, name) && !in_ct())
        throw diagnostic("ct-переменная меняется только кодом при компиляции (ct { … })", db[n].where);
      mval & c = cell(p);
      if(op == "=" && is_record(rhs)) rhs = to_type(std::move(rhs), place_type(c), db[n].where); // Struct holds it, {x: …} becomes it.
      if(op == "=" && st.res_on && !var_of(p).hidden && has_res(c)) { // Overwrite - the old resource is released
        mval old = std::move(c);
        c = mval{m_unit{}};
        drop_value(old, db[n].where);
      }
      if(op == "=") var_of(p).hidden = 0;
      mval & c2 = cell(p);
      c2 = op == "=" ? std::move(rhs) : primitive(prim_of(op), {c2, rhs}, db[n].where);
      return finish(mval{m_unit{}});
    }
    if(db.is_index(lhs) && db.is<n_ident>(db.index_of(lhs).object)) { // xs[i] = e
      auto const & ix = db.index_of(lhs);
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
      if(op == "=" && is_record(rhs)) rhs = to_type(std::move(rhs), place_type(xs[uint64_t(k)]), db[n].where);
      xs[uint64_t(k)] = op == "=" ? std::move(rhs) : primitive(prim_of(op), {xs[uint64_t(k)], rhs}, db[n].where);
      return finish(mval{m_unit{}});
    }
    // a.b.c = e is the aggregate field in the variable (chain of fields on behalf)
    if(db.is_member(lhs)) {
      std::vector<symbol> path;
      node_id x = lhs;
      while(db.is_member(x)) {
        path.push_back(db.member_of(x).name);
        x = db.member_of(x).object;
      }
      if(db.is<n_ident>(x)) {
        mval rhs = deref(pop_val());
        place p = find(t.scope, db.as<n_ident>(x).name);
        if(p.scope == m_none) throw diagnostic("присваивание не в переменную", db[lhs].where);
        not_hidden(p, db.as<n_ident>(x).name, db[lhs].where);
        mval * target = &cell(p);
        for(auto it = path.rbegin(); it != path.rend(); ++it) {
          if(is_shared_v(*target)) target = &slot_of(*target).v; // shared - transparently
          if(!target->is<m_agg>()) throw diagnostic(cat("у значения типа ", describe(*target), " нет поля ", helper.str(*it)), db[lhs].where);
          mval * next = nullptr;
          for(auto & f: target->as<m_agg>().fields)
            if(f.name == *it) next = &f.v;
          if(!next) throw diagnostic(cat("у значения типа ", describe(*target), " нет поля ", helper.str(*it)), db[lhs].where);
          target = next;
        }
        if(op == "=" && is_record(rhs)) rhs = to_type(std::move(rhs), place_type(*target), db[n].where);
        *target = op == "=" ? std::move(rhs) : primitive(prim_of(op), {*target, rhs}, db[n].where);
        return finish(mval{m_unit{}});
      }
    }
    unsupported("присваивание в это место", lhs);
  }

  static std::string_view prim_of(std::string_view op) {
    return op == "+=" ? "add" : op == "-=" ? "sub" : op == "*=" ? "mul" : "div";
  }

  // Associativity of the operator in the context of the process - as in the interpreter
  // (op_associative): elements of modules and prelude, operators of blocks in the field using
  // the operator of active functions-contexts; the code of the sealed module elements of
  // modules - only its definition context (at - node of the chain).
  bool op_associative(symbol op, uint32_t scope, node_id at) {
    if(helper.op_associative(op, at)) return true;
    symbol f = helper.infix_entity(op);
    if(helper.local_ops.contains(f.value))
      if(place p = find_lexical(scope, f); p.scope != m_none && cell(p).is<m_fn>())
        for(node_id fn: cell(p).as<m_fn>().set)
          if(helper.assoc_attr(fn)) return true;
    for(auto const & r: fib().overrides)
      if(r.entity == f && helper.assoc_attr(r.fn)) return true;
    return false;
  }

  void eval_node(node_id n, n_chain const & c) {
    m_task & t = top();
    if(t.phase == 0 && std::ranges::all_of(c.ops, [&](symbol o) { return o == c.ops[0]; }) && op_associative(c.ops[0], t.scope, n)) {
      t.phase = 1; // the same associative operator - one tree, without brute force
      return push_eval(helper.assoc_tree(n, c), t.scope, t.frame);
    }
    if(t.phase == 0) { // chain tree - according to the types of operands (solution parsing, "combination only")
      std::string assoc;
      for(symbol o: c.ops) assoc += op_associative(o, t.scope, n) ? '1' : '0';
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
      node_id tree = helper.resolve_chain(n, c, probe, assoc);
      if(!tree) throw diagnostic("цепочка: дерево не выбрано", db[n].where);
      t.phase = 1;
      return push_eval(tree, t.scope, t.frame);
    }
    mval v = deref(pop_val());
    finish(std::move(v));
  }

  // Challenge: arguments (expr-parameter - expression of the caller), then overload.
  void eval_node(node_id n, n_call const & c) {
    if(node_id rc = db.require_cond(n)) return eval_require(n, rc);
    if(symbol h = db.hide_name(n)) return eval_hide(n, h);
    if(node_id b = db.try_compile_body(n)) return eval_try_compile(b);
    m_task & t = top();
    if(t.phase == 0) {
      if(auto set = entity_members(n, t.scope); !set.empty()) return eval_call_args(n, set, c.args, false); // essence. f(...)
      if(db.is_member(c.callee)) { // a.f(b) ≡ f(a, b) (§4.5)
        auto const & m = db.member_of(c.callee);
        node_list args{m.object};
        args.insert(args.end(), c.args.begin(), c.args.end());
        auto it = helper.functions.find(m.name);
        if(it == helper.functions.end()) return builtin_call(n, m.name, args);
        return eval_call_args(n, it->second, args, false);
      }
      if(db.is<n_ref>(c.callee) && db.is<n_fn>(node_id(db.as<n_ref>(c.callee).target))) { // Reference to function - its current name
        auto it = helper.functions.find(db.as<n_fn>(node_id(db.as<n_ref>(c.callee).target)).name);
        if(it != helper.functions.end()) return eval_call_args(n, it->second, c.args, false);
      }
      if(!db.is<n_ident>(c.callee)) unsupported("вызов значения-выражения", n);
      symbol name = db.as<n_ident>(c.callee).name;
      place p = find(t.scope, name);
      if(p.scope != m_none) {
        mval & cv = cell(p);
        if(cv.is<m_thunk>() && db.is<n_lambda>(cv.as<m_thunk>().expr) && db.as<n_lambda>(cv.as<m_thunk>().expr).block)
          return call_block(n, cv.as<m_thunk>(), c.args);
        if(cv.is<m_fn>() && !cv.as<m_fn>().set.empty()) { // localization
          std::vector<node_id> fns(cv.as<m_fn>().set.begin(), cv.as<m_fn>().set.end());
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
      if(it == helper.functions.end() && helper.str(name) == "code" && c.args.empty()) return finish(from_value(helper.code_value(node_id()), db[n].where)); // knot
      if(it == helper.functions.end() && helper.str(name) == "code" && c.args.size() == 1) { // AST
        node_id e = c.args[0];
        uint32_t sc = t.scope;
        while(db.is<n_ident>(e)) {
          place q = find_lexical(sc, db.as<n_ident>(e).name);
          if(q.scope == m_none || !st.scopes[q.scope].vars[q.slot].v.is<m_thunk>()) break;
          m_thunk const & th = st.scopes[q.scope].vars[q.slot].v.as<m_thunk>();
          e = th.expr;
          sc = th.env;
        }
        if(db.is<n_raw>(e) && db.as<n_raw>(e).rule) e = helper.raw_result(e); // The block disassembled by the code - its result
        if(db.is<n_ident>(e) && find(sc, db.as<n_ident>(e).name).scope == m_none && e == c.args[0]) { // code(f) - entity Advertisement: The Assistant
          auto v = helper_eval(n, t.scope, std::nullopt, db[n].where);
          if(v) return finish(std::move(*v));
        }
        return finish(from_value(helper.code_value(e), db[n].where));
      }
      if(it == helper.functions.end() && helper.str(name) == "alloc_in" && c.args.size() == 2) {
        // with_alloc(r) { … } - Body at the call site: machine memory - process pages (dump writes them
        // as is), your region level inside the process - not separate addresses (mine)
        t.node = c.args[1];
        t.phase = 0;
        return;
      }
      if(it == helper.functions.end() && helper.str(name) == "intercept") return begin_intercept(n, c.args);
      if(it == helper.functions.end() && helper.str(name) == "fiber_start") return begin_spawn(n, c.args);
      if(it == helper.functions.end() && helper.str(name) == "pressure_handler") return begin_pressure(n, c.args);
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

  // Program type node: call eval with fields (like interpreter::eval_node(n_node)).
  std::vector<node_id> kind_evals(n_node const & u) {
    std::vector<node_id> evals;
    if(auto it = helper.kind_members.find(helper.sym(u.kind)); it != helper.kind_members.end())
      for(node_id f: it->second)
        if(helper.str(db.as<n_fn>(f).name) == "eval") evals.push_back(f);
    return evals;
  }
  void eval_node(node_id n, n_node const & u) {
    if(top().phase == 0) {
      std::vector<node_id> evals = kind_evals(u);
      if(evals.empty())
        throw diagnostic(cat("узел ", u.kind, " не раскрыт: смысл вида программы — раскрытие ([[transform]] f(", u.kind, " x) возвращает код из известных узлов) или ", u.kind, " { eval(…) => …; }"), db[n].where);
      for(node_id x: u.items)
        if(!x) throw diagnostic(cat("узел ", u.kind, ": поле пусто — eval получает все поля"), db[n].where);
      return eval_call_args(n, evals, u.items, false);
    }
    resume_call(n);
  }

  // Arguments for congestion fns: t.extra - overload by the name of the first, t.a - number of arguments.
  void eval_call_args(node_id n, std::vector<node_id> const & fns, node_list const & args, bool op) {
    m_task & t = top();
    (void)op;
    t.extra = fns.front();
    t.a = uint32_t(db.is<n_call>(n) ? call_args(n).size() : args.size()); // with implicit (O9) - call_args
    t.b = 0; // How many arguments are computed
    t.phase = 1;
    call_args_step(n);
  }

  // entity .f(…): the object is the name of the function - entity (not variable), f - its local
  // functions; neighbors are visible to each other (as in the interpreter - entity_scope).
  std::vector<node_id> entity_members(node_id n, uint32_t scope) {
    if(!db.is<n_call>(n) || !db.is_member(db.as<n_call>(n).callee)) return {};
    auto const & m = db.member_of(db.as<n_call>(n).callee);
    node_id head = m.object;
    while(db.is_member(head)) head = db.member_of(head).object;
    if(!db.is<n_ident>(head) || find(scope, db.as<n_ident>(head).name).scope != m_none) return {};
    if(node_id qm = helper.module_by_path(m.object)) { // geo.vec.norm(…) - overload declared in the module
      std::vector<node_id> set;
      if(auto it = helper.functions.find(m.name); it != helper.functions.end())
        for(node_id f: it->second)
          if(helper.module_of(f) == qm) set.push_back(f);
      return set;
    }
    if(!db.is<n_ident>(m.object)) return {};
    auto it = helper.functions.find(db.as<n_ident>(m.object).name);
    if(it == helper.functions.end()) return {};
    std::vector<node_id> set;
    for(node_id f: it->second)
      if(db.is<n_block>(db.as<n_fn>(f).body))
        for(node_id s: db.as<n_block>(db.as<n_fn>(f).body).items)
          if(db.is<n_fn>(s) && !db.as<n_fn>(s).scoped && db.as<n_fn>(s).name == m.name) set.push_back(s);
    return set;
  }
  uint32_t entity_scope(symbol owner) {
    for(auto const & e: st.entities)
      if(e.owner == owner) return e.scope;
    uint32_t sc = new_scope(proc_globals());
    for(node_id f: helper.functions.at(owner))
      if(db.is<n_block>(db.as<n_fn>(f).body))
        for(node_id item: db.as<n_block>(db.as<n_fn>(f).body).items)
          if(db.is<n_fn>(item) && !db.as<n_fn>(item).scoped) {
            symbol name = db.as<n_fn>(item).name;
            bool added = false;
            for(auto & v: st.scopes[sc].vars)
              if(v.name == name && v.v.is<m_fn>()) {
                v.v.as<m_fn>().set.push_back(item);
                added = true;
              }
            if(!added) declare(sc, name, mval{m_fn{{}, {}, sc, {item}}});
          }
    st.entities.push_back({owner, sc});
    return sc;
  }

  node_list call_args(node_id n) {
    if(db.is<n_op>(n)) return db.as<n_op>(n).args;
    if(db.is<n_node>(n)) return db.as<n_node>(n).items; // Type of program: fields - arguments eval
    auto const & c = db.as<n_call>(n);
    if(db.is<n_ident>(c.callee) && !helper.implicit_fns.empty()) // Implicit parameters (O9) - like the interpreter
      if(auto it = helper.functions.find(db.as<n_ident>(c.callee).name); it != helper.functions.end()) return helper.with_implicit(it->second, c.args, n.value);
    if(db.is_member(c.callee) && !entity_members(n, top().scope).empty()) return c.args; // entity .f(…) without an object
    if(db.is_member(c.callee)) {
      node_list args{db.member_of(c.callee).object};
      args.insert(args.end(), c.args.begin(), c.args.end());
      return args;
    }
    return c.args;
  }

  // overload of the call n: local set (from the caller area) or module function; env is the
  // local set environment (m_none is the global area).
  std::vector<node_id> overloads_of(node_id n, node_id any, uint32_t scope, uint32_t * env = nullptr) {
    if(db.is<n_node>(n)) return kind_evals(db.as<n_node>(n));
    if(auto set = entity_members(n, scope); !set.empty()) {
      node_id obj = db.member_of(db.as<n_call>(n).callee).object;
      if(env) *env = helper.module_by_path(obj) ? m_none : entity_scope(db.as<n_ident>(obj).name); // module - global field
      return set;
    }
    symbol local; // Local dial: call by name or operator declared in the block
    if(db.is<n_call>(n) && db.is<n_ident>(db.as<n_call>(n).callee)) local = db.as<n_ident>(db.as<n_call>(n).callee).name;
    else if(db.is<n_op>(n) && helper.local_ops.contains(db.as<n_fn>(any).name.value)) local = db.as<n_fn>(any).name;
    if(local)
      if(place p = db.is<n_op>(n) ? find_lexical(scope, local) : find(scope, local); p.scope != m_none)
        if(mval & cv = cell(p); cv.is<m_fn>() && !cv.as<m_fn>().set.empty()) {
          if(env) *env = cv.as<m_fn>().env;
          return std::vector<node_id>(cv.as<m_fn>().set.begin(), cv.as<m_fn>().set.end());
        }
    return helper.functions.at(db.as<n_fn>(any).name);
  }

  void call_args_step(node_id n) {
    m_task & t = top();
    node_list args = call_args(n);
    uint32_t local = m_none;
    std::vector<node_id> fns = overloads_of(n, t.extra, t.scope, &local);
    if(t.phase == 2) { // challenge
      mval r = deref(pop_val());
      if(t.b > t.a) st.scopes[t.scope].steps.resize(t.b - t.a - 1);
      return finish(std::move(r));
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
      // out-параметр: имя — место результата; нет у вызывающего — объявляется (строгость —
      // Read before writing, all paths - parsing as an interpreter
      bool out_everywhere = !fns.empty() && db.is<n_ident>(args[i]);
      for(node_id f: fns) {
        auto const & ps = db.as<n_fn>(f).params;
        if(i >= ps.size() || db.as<n_param>(ps[i]).mode != param_mode::out) out_everywhere = false;
      }
      if(out_everywhere && find(t.scope, db.as<n_ident>(args[i]).name).scope == m_none) declare(t.scope, db.as<n_ident>(args[i]).name, mval{m_unit{}});
      // an array element in a variable parameter - a reference to the element, not a
      // copy of the place - alias and in an immutable parameter:
// 
      bool aliased = !fns.empty();
      for(node_id f: fns) {
        auto const & ps = db.as<n_fn>(f).params;
        aliased = aliased && i < ps.size() && db.as<n_param>(ps[i]).mode != param_mode::expr && db.as<n_param>(ps[i]).mode != param_mode::own;
      }
      if(aliased && elem_place(db, args[i])) return push_elem_ref(args[i], t.scope, t.frame, true);
      if(aliased && field_alias(args[i], t.scope)) continue; // variable field - alias by field number
      // path deeper (a.b.c, cl[c].xs.ys) - link-path, like &e = ...; steps - in the area of calling to the
      // end; not place (xs.size, line element) - value n
      if(aliased && (db.is_member(args[i]) || db.is_index(args[i])) && path_arg(args[i], t.scope)) return push_path_ref(args[i], t.scope, t.frame, true);
      return push_eval(args[i], t.scope, t.frame); // the value (or alias of the place) on the stack
    }
    if(t.phase != 2) { // link-path steps of arguments are removed after the call (the cycle does not accumulate)
      uint32_t mark = m_none;
      for(uint32_t k = t.base; k < fib().vals.size(); ++k)
        if(mval const & v = fib().vals[k]; v.is<m_alias>() && v.as<m_alias>().path_scope == t.scope) mark = std::min(mark, uint32_t(v.as<m_alias>().path_at));
      if(mark != m_none) t.b = t.a + 1 + mark;
    }
    t.phase = 2;
    uint32_t frame = t.frame;
    site_node = n;
    invoke_parent = local;
    uint32_t nargs = t.a;
    if(std::ranges::any_of(args, [&](node_id a) { return db.is<n_spread>(a); })) { // ...t - in separate arguments
      auto & vals = fib().vals;
      std::vector<mval> ex;
      for(uint32_t k = 0; k < args.size(); ++k) {
        mval v = vals[t.base + k];
        if(!db.is<n_spread>(args[k])) {
          ex.push_back(std::move(v));
          continue;
        }
        mval x = deref(v);
        if(!x.is<m_arr>()) throw diagnostic(cat("...: раскрывается массив или кортеж, получено ", describe(x)), db[args[k]].where);
        for(auto const & e: x.as<m_arr>().items) ex.push_back(e);
      }
      vals.resize(t.base);
      for(auto & v: ex) vals.push_back(std::move(v));
      nargs = uint32_t(ex.size());
      if(!std::ranges::any_of(fns, [&](node_id f) { return helper.arity_fits(f, nargs); })) {
        diagnostic d(cat("...: ", nargs, " аргументов — нет перегрузки ", helper.str(db.as<n_fn>(fns.front()).name), " такой длины"), db[n].where, diag_kind::runtime);
        d.type = "unpack_failed";
        throw d;
      }
    }
    invoke(fns, nargs, frame, db[n].where);
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
    if(helper.str(name) == "process_class" && t.a == 0) return finish(mval{std::string_view(fib().proc ? "фоновый" : "монопольный")});
    if(helper.str(name) == "granted" && t.a == 1)
      if(mval const & k = ref_of(fib().vals[t.base]); k.is<m_str>()) return finish(mval{machine_granted(std::string_view(k.as<m_str>()))});
    if(helper.str(name) == "is_index" && t.a == 2) { // massifless
      mval const & i = ref_of(fib().vals[t.base]);
      mval const & xs = ref_of(fib().vals[t.base + 1]);
      if(i.is<int64_t>() && xs.is<m_arr>()) return finish(mval{i.as<int64_t>() >= 0 && uint64_t(i.as<int64_t>()) < xs.as<m_arr>().items.size()});
    }
    if(helper.str(name) == "pin_storage" && t.a == 1 && fib().vals[t.base].is<m_alias>()) { // pin: the machine does not have host addresses - links and so base and index
      if(!follow(fib().vals[t.base].as<m_alias>()).is<m_arr>()) throw diagnostic("pin: не массив", db[n].where);
      return finish(mval{m_unit{}});
    }
    if(helper.str(name) == "append" && t.a == 2 && fib().vals[t.base].is<m_alias>()) { // push: the place of the caller grows (before the name of the arguments - without a copy of the array)
      m_alias al = fib().vals[t.base].as<m_alias>();
      mval & xs = follow(al);
      if(!xs.is<m_arr>()) throw diagnostic("append: не массив", db[n].where);
      mval x = deref(fib().vals[t.base + 1]);
      if(st.res_on) take_from(call_args(n)[1], x, t.scope); // resource in an array - transfer or copy, as in the assignment
      xs.as<m_arr>().items.push_back(std::move(x)); // data transfer links are not visible
      return finish(mval{m_unit{}});
    }
    std::vector<mval> a;
    for(uint32_t i = 0; i < t.a; ++i) a.push_back(deref(fib().vals[t.base + i]));
    if(helper.str(name) == "shared" && t.a == 1 && !helper.functions.contains(name)) { // vault
      mval v = a[0];
      take_from(args[0], v, t.scope);
      return finish(make_shared(std::move(v)));
    }
    // ct-state of value (§5.6): one semantics with an interpreter - set_ct_state(x, s) sets the
    // state to place x (variable, via alias), ct_state(x, иначе) reads it
    if(helper.str(name) == "set_ct_state" && t.a == 2 && !helper.functions.contains(name)) {
      mval * place = nullptr;
      if(fib().vals[t.base].is<m_alias>()) place = &follow(fib().vals[t.base].as<m_alias>());
      else if(db.is<n_ident>(args[0])) {
        auto p = find(t.scope, db.as<n_ident>(args[0]).name);
        if(p.scope != m_none) place = &cell(p);
      }
      if(!place) throw diagnostic("set_ct_state: первый аргумент — переменная", db[n].where);
      st.ct_states.push_back(a[1]);
      place->ct = uint32_t(st.ct_states.size());
      return finish(mval{m_unit{}});
    }
    if(helper.str(name) == "ct_state" && t.a == 2 && !helper.functions.contains(name)) return finish(a[0].ct ? mval(st.ct_states[a[0].ct - 1]) : a[1]);
    if(helper.str(name) == "shared_count" && t.a == 1 && !helper.functions.contains(name)) {
      if(!is_shared_v(a[0])) throw diagnostic("shared_count: ожидался shared", db[n].where);
      return finish(mval{int64_t(slot_of(a[0]).count)});
    }
    if(helper.str(name).starts_with("stream_")) { // Can block: the task will remain and repeat
      mval out;
      if(!stream_op(helper.str(name), a, db[n].where, out)) return park();
      return finish(std::move(out));
    }
    if(helper.str(name).starts_with("net_") || helper.str(name).starts_with("file_")) return finish(host_op(helper.str(name), a, db[n].where));
    if(helper.str(name) == "process" && !helper.functions.contains(name)) return finish(start_proc(call_args(n), a, db[n].where));
    if((helper.str(name) == "process_wait" || helper.str(name) == "process_error") && t.a == 1) { // waiting for the end of the process
      m_proc & m = proc_of(a[0], db[n].where);
      if(!m.done) {
        m.waiters.push_back(st.current);
        return park();
      }
      return finish(helper.str(name) == "process_wait" ? mval{!m.failed} : mval{std::string_view(m.error)});
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
    t.a = ruled_scope(th); // rule layer is released with block
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
      if(st.scopes[t.a].layer) release_scope(t.a);
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
    t.a = ruled_scope(body);
    if(t.a != body.env) t.owned = t.a;
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
    node_list handlers = db.handlers_of(hs.expr);
    std::string type = exception_type(f.pending.v);
    for(node_id h: handlers) {
      if(!db.is<n_lambda>(h) || db.as<n_lambda>(h).params.size() != 1) throw diagnostic("catch: блок с одним параметром — catch(E e) { … }", db[h].where);
      auto const & l = db.as<n_lambda>(h);
      auto const & p = db.as<n_param>(l.params[0]);
      if(p.type && db.is<n_ident>(p.type) && !helper.types.is_a(type, helper.str(db.as<n_ident>(p.type).name))) continue; // and heirs
      uint32_t sc = new_scope(hs.env);
      if(helper.str(p.name) != "_") declare(sc, p.name, std::move(f.pending.v));
      f.pending = {};
      pending_error.reset();
      f.vals.resize(t.base + 1);
      t.phase = 2;
      release_scope(t.owned); // layer of body rules (if there was) - the body is over
      t.owned = sc;
      f.tasks.push_back(m_task{m_lambda_body, h, 0, sc, hs.frame, uint32_t(f.vals.size()), m_none, 0, 0, l.body, hs.ctx});
      return true;
    }
    return false;
  }

  // on_pressure { … } → pressure_handler(тело): process pressure handler (like an
  // interpreter); runs it pressure() - external command or PSI host
  void begin_pressure(node_id n, node_list const & args) {
    if(args.size() != 1) throw diagnostic("on_pressure { … }", db[n].where);
    m_thunk th = thunk_named(args[0], top().scope);
    uint32_t env = ruled_scope(th);
    capture(env);
    st.pressure.push_back({th, env, fib().proc, fib().globals});
    arm_pressure();
    finish(mval{m_unit{}});
  }
  int psi_fd = -1;
  bool psi_armed = false;
  void arm_pressure() {
    if(psi_fd == -1) {
      if(!ring) ring.emplace();
      psi_fd = ring->ok ? os::memory_pressure_fd() : -1;
      if(psi_fd < 0) psi_fd = -2; // host does not have a PSI - only an external command (proc pressure)
    }
    if(psi_fd >= 0 && !psi_armed) {
      psi_armed = true;
      ring->poll(psi_fd, POLLPRI, io_tag(op_psi, 0, 0)); // No application: io_pending is not growing
    }
  }
  // Pressure: - Background handlers first
  void pressure() {
    for(int pass = 0; pass < 2; ++pass)
      for(std::size_t i = 0; i < st.pressure.size(); ++i) {
        m_pressure h = st.pressure[i];
        if((h.proc != 0) != (pass == 0)) continue;
        uint32_t id = uint32_t(st.fibers.size());
        st.fibers.emplace_back();
        st.fibers.back().proc = h.proc;
        st.fibers.back().globals = h.globals;
        st.fibers.back().tasks.push_back(m_task{m_lambda_body, h.body.expr, 0, h.env, h.body.frame, 0, m_none, 0, 0, h.body.expr, h.body.ctx});
        st.ready.push_back(id);
      }
  }

  // spawn { … } prelude → fiber_start(тело): the new fiber at the end of the queue;
  // the body is closed (its area lives beyond the call).
  void begin_spawn(node_id n, node_list const & args) {
    if(args.size() != 1) throw diagnostic("spawn(тело)", db[n].where);
    m_thunk th = thunk_named(args[0], top().scope);
    uint32_t env = ruled_scope(th); // Rule layer lives with fiber
    capture(env);
    uint32_t id = uint32_t(st.fibers.size()), pid = fib().proc, groot = fib().globals;
    st.fibers.emplace_back();
    st.fibers.back().proc = pid; // fiber - the same process
    st.fibers.back().globals = groot;
    st.fibers.back().tasks.push_back(m_task{m_lambda_body, th.expr, 0, env, th.frame, 0, m_none, 0, 0, th.expr, th.ctx});
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
    if(n == "stream_write" && s.host) { // stream host : file to write - request
      if(s.host == 3) throw diagnostic("в сеть — send(сеть, peer, байты)", where);
      if(s.host == 1) throw diagnostic("файл открыт на чтение", where);
      host_rec * h = host_of(sid);
      if(!h || s.closed) rt_fail("closed_stream", "запись в закрытый поток", where);
      if(!a[1].is<m_str>()) throw diagnostic(cat("в файл — байты (str), получено ", describe(a[1])), where);
      h->peers[0].out.push_back(std::string(a[1].as<m_str>()));
      file_write_next(sid);
      return true;
    }
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
        if(host_rec * h = s.host == 1 ? host_of(sid) : nullptr; h && !h->io_busy) { // File: The next piece is the application
          h->io_busy = true;
          h->io_buf.resize(65536);
          ++io_pending;
          io(where).read(h->file_fd, h->io_buf.data(), h->io_buf.size(), h->offset, io_tag(op_fread, sid, 0));
        }
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
      if(host_rec * h = s.host == 2 ? host_of(sid) : nullptr; h && !h->peers[0].out.empty()) { // queue
        s.writers.push_back(me);
        return false;
      }
      close_host(sid);
      s.closed = true;
      for(uint32_t r: std::exchange(s.readers, {})) wake(r);
      for(uint32_t w: std::exchange(s.writers, {})) wake(w);
      return true;
    }
    throw diagnostic(cat("неизвестная операция потока ", n), where);
  }

  // Hide x: Name no longer available (§5.6).
  void eval_hide(node_id n, symbol name) {
    place p = find(top().scope, name);
    if(p.scope == m_none || cell(p).is<m_thunk>()) throw diagnostic(cat("hide: нет переменной ", helper.str(name)), db[n].where);
    not_hidden(p, name, db[n].where);
    var_of(p).hidden = n.value + 1;
    finish(mval{m_unit{}});
  }

  // require in the body - check in runtime (§5.4): violated - program error.
  void eval_require(node_id n, node_id cond) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(cond, t.scope, t.frame);
    }
    if(!truthy(pop_val(), db[cond].where)) { // require(c, "сообщение", …): Notes only at parsing
      auto const & args = db.as<n_call>(n).args;
      diagnostic d(args.size() > 1 && db.is<n_str>(args[1]) ? db.as<n_str>(args[1]).value : std::string(db.is_assume(n) ? "нарушено предположение assume" : "нарушено требование require"), db[n].where, diag_kind::runtime);
      d.type = "require_failed"; // Excluding a program like an interpreter: catch it
      throw d;
    }
    finish(mval{m_unit{}});
  }

  // v[i] as location: reference to a variable array element (alias with buffer generation).
  void push_elem_ref(node_id n, uint32_t scope, uint32_t frame, bool arg = false) {
    auto & f = fib();
    f.tasks.push_back(m_task{m_elem_ref, n, 0, scope, frame, uint32_t(f.vals.size())});
    f.tasks.back().a = arg; // Argument: not place, meaning n
    if(f.tasks.size() > 1) f.tasks.back().ctx = f.tasks[f.tasks.size() - 2].ctx;
  }
  void elem_ref_step() {
    m_task & t = top();
    auto const & ix = db.index_of(t.node);
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
    if(t.a && (!arr.is<m_arr>() || !i.is<int64_t>() || al.index != m_none)) return as_value(t); // Argument Not Place (Element of a Line) n
    if(!arr.is<m_arr>() || !i.is<int64_t>()) throw diagnostic("auto& — ссылка на переменную или элемент массива переменной", db[t.node].where);
    int64_t k = i.as<int64_t>();
    auto size = arr.as<m_arr>().items.size();
    if(k < 0 || uint64_t(k) >= size) out_of_bounds(k, size, db[t.node].where);
    if(al.index != m_none) throw diagnostic("auto& — ссылка на переменную или элемент массива переменной", db[t.node].where); // interpreter-like
    finish(mval{m_alias{al.scope, al.slot, uint32_t(k)}});
  }
  // The root of the place is the variable that the link itself (r[k] from &r = ...): then this is the path.
  bool ref_root(node_id x, uint32_t scope) {
    while(db.is_member(x) || db.is_index(x)) x = db.is_member(x) ? db.member_of(x).object : db.index_of(x).object;
    if(!db.is<n_ident>(x)) return false;
    place p = find(scope, db.as<n_ident>(x).name);
    if(p.scope == m_none) return false;
    mval const & v = st.scopes[p.scope].vars[p.slot].v;
    return v.is<m_alias>() && (v.as<m_alias>().index != m_none || v.as<m_alias>().field != m_none || v.as<m_alias>().path_scope != m_none);
  }
  // Link-path (§12.1, tail of step 10): chain indexes - in turn (phases), then steps - to the area where
  // the link is declared; depth - any (the interpreter - a chain of three steps).
  void push_path_ref(node_id n, uint32_t scope, uint32_t frame, bool arg = false) {
    auto & f = fib();
    f.tasks.push_back(m_task{m_path_ref, n, 0, scope, frame, uint32_t(f.vals.size())});
    f.tasks.back().a = arg; // Argument: not place, meaning n
    if(f.tasks.size() > 1) f.tasks.back().ctx = f.tasks[f.tasks.size() - 2].ctx;
  }
  // A link-argument whose path does not belong: the problem becomes the calculation of the value of node
  void as_value(m_task & t) {
    fib().vals.resize(t.base);
    t.kind = m_eval;
    t.phase = 0;
    t.a = 0;
  }
  void path_ref_step() {
    m_task & t = top();
    std::vector<node_id> chain; // root-out
    node_id x = t.node;
    while(db.is_member(x) || db.is_index(x)) {
      chain.push_back(x);
      x = db.is_member(x) ? db.member_of(x).object : db.index_of(x).object;
    }
    std::ranges::reverse(chain);
    std::vector<node_id> indices;
    for(node_id c: chain)
      if(db.is_index(c)) indices.push_back(db.index_of(c).index);
    if(t.phase < indices.size()) return push_eval(indices[t.phase++], t.scope, t.frame);
    if(!db.is<n_ident>(x)) throw diagnostic("auto& — ссылка на переменную, поле или элемент массива", db[t.node].where);
    symbol name = db.as<n_ident>(x).name;
    place p = find(t.scope, name);
    if(p.scope == m_none) throw diagnostic(cat("неизвестное имя ", helper.str(name)), db[x].where);
    not_hidden(p, name, db[t.node].where);
    std::vector<m_step> steps;
    m_alias al{p.scope, p.slot};
    if(mval const & root = st.scopes[p.scope].vars[p.slot].v; root.is<m_alias>()) { // link from the link: its path, then its steps
      m_alias const & r = root.as<m_alias>();
      al = m_alias{r.scope, r.slot};
      if(r.path_scope != m_none)
        for(uint32_t k = 0; k < r.path_n; ++k) steps.push_back(st.scopes[r.path_scope].steps[r.path_at + k]);
      else {
        if(r.index != m_none) steps.push_back({r.index, false});
        if(r.field != m_none) steps.push_back({r.field, true}); // field
      }
    }
    uint32_t next = 0;
    for(node_id c: chain) {
      m_alias prefix = al;
      uint32_t at = uint32_t(st.scopes[t.scope].steps.size());
      for(auto const & s: steps) st.scopes[t.scope].steps.push_back(s);
      prefix.path_scope = t.scope;
      prefix.path_at = uint16_t(at);
      prefix.path_n = uint8_t(steps.size());
      mval & box = steps.empty() ? follow(al) : follow(prefix); // step-up
      st.scopes[t.scope].steps.resize(at);
      if(db.is_member(c)) {
        symbol f = db.member_of(c).name;
        mval * agg = &box;
        if(agg->is<m_agg>() && is_shared_v(*agg)) agg = &slot_of(*agg).v;
        if(t.a && (!agg->is<m_agg>() || std::ranges::none_of(agg->as<m_agg>().fields, [&](auto const & x) { return x.name == f; }))) return as_value(t); // xs.size - value n
        if(!agg->is<m_agg>()) throw diagnostic(cat("у значения ", describe(*agg), " нет поля ", helper.str(f)), db[c].where);
        auto const & fs = agg->as<m_agg>().fields;
        uint32_t k = 0;
        while(k < fs.size() && fs[k].name != f) ++k;
        if(k == fs.size()) throw diagnostic(cat("у значения ", describe(*agg), " нет поля ", helper.str(f)), db[c].where);
        steps.push_back({k, true});
      } else {
        mval i = deref(fib().vals[t.base + next++]);
        if(t.a && (!box.is<m_arr>() || !i.is<int64_t>())) return as_value(t); // line element n
        if(!box.is<m_arr>() || !i.is<int64_t>()) throw diagnostic(cat("ссылка на элемент: ", describe(box), " — не массив"), db[c].where);
        int64_t k = i.as<int64_t>();
        auto size = box.as<m_arr>().items.size();
        if(k < 0 || uint64_t(k) >= size) out_of_bounds(k, size, db[c].where);
        steps.push_back({uint32_t(k), false});
      }
      if(steps.size() > 255) throw diagnostic("ссылка — не глубже 255 шагов (поле, элемент)", db[t.node].where);
    }
    fib().vals.resize(t.base);
    uint32_t at = uint32_t(st.scopes[t.scope].steps.size());
    for(auto const & s: steps) st.scopes[t.scope].steps.push_back(s);
    al.path_scope = t.scope;
    al.path_at = uint16_t(at);
    al.path_n = uint8_t(steps.size());
    finish(mval{al});
  }
  // b.f in the variable parameter - alias on the field (as an array element): "parameter - alias"
  // (§6.2). A variable field or reference to an element (e.f at &e = v[i]); deeper is the path.
  bool field_alias(node_id x, uint32_t scope) {
    if(!db.is_member(x) || !db.is<n_ident>(db.member_of(x).object)) return false;
    place p = find(scope, db.as<n_ident>(db.member_of(x).object).name);
    if(p.scope == m_none) return false;
    m_alias al{p.scope, p.slot};
    if(st.scopes[p.scope].vars[p.slot].v.is<m_alias>()) {
      al = st.scopes[p.scope].vars[p.slot].v.as<m_alias>();
      if(al.field != m_none || al.path_scope != m_none) return false; // field
    }
    mval & agg = follow(al);
    if(!agg.is<m_agg>()) return false;
    symbol name = db.member_of(x).name;
    auto const & fs = agg.as<m_agg>().fields;
    for(uint32_t k = 0; k < fs.size(); ++k)
      if(fs[k].name == name) {
        al.field = k;
        fib().vals.push_back(mval{al});
        return true;
      }
    return false;
  }
  // The path of the argument is on behalf of the variable: its place will become a link-path (steps
  // - in the area of the call to the end, depth - any).
  bool path_arg(node_id x, uint32_t scope) {
    while(db.is_member(x) || db.is_index(x)) x = db.is_member(x) ? db.member_of(x).object : db.index_of(x).object;
    return db.is<n_ident>(x) && find(scope, db.as<n_ident>(x).name).scope != m_none;
  }
  // The program module variable found in the module domain is foreign to the module code that the
  // program does not see (the structure body prelude): like interpreter::foreign_global.
  bool foreign_module_var(place p, symbol name, node_id at) {
    if(p.scope != st.globals) return false;
    node_id home = module_var_home(name);
    node_id m = helper.module_of(at);
    if(home && m && m != home && helper.service_modules.contains(home.value)) return true; // Service status - only its code
    return home && m && !helper.sees_directly(m, home, name); // use is non-transitive (tail of step 10)
  }
  // The program module that declared the variable name (the last in the order of modules).
  node_id module_var_home(symbol name) const {
    std::vector<node_id> modules = helper.module_order;
    if(modules.empty() && db.is<n_module>(root)) modules.push_back(root);
    for(auto it = modules.rbegin(); it != modules.rend(); ++it)
      if(std::ranges::any_of(db.as<n_module>(*it).items, [&](node_id i) { return (db.is<n_var>(i) && db.as<n_var>(i).name == name) || service_var(i) == name; })) return *it;
    return {};
  }
  // service variable: x = e at the service module level (other modules do not).
  symbol service_var(node_id item) const {
    if(db.is<n_var>(item) && !db.as<n_var>(item).ct && db.as<n_var>(item).attrs.empty()) return db.as<n_var>(item).name;
    if(db.is<n_op>(item) && helper.str(db.as<n_op>(item).op) == "=" && db.is<n_ident>(db.as<n_op>(item).args[0])) return db.as<n_ident>(db.as<n_op>(item).args[0]).name;
    return {};
  }
  // The process is restored to another VM: the state of services is its (renewed), not the process
// 
  void services_fresh() {
    for(symbol nm: st.service_names)
      if(place p = find(st.globals, nm); p.scope != m_none)
        if(value * x = helper.globals->find(nm)) cell(p) = from_value(*x, {});
  }
  static bool elem_place(ast_db const & db, node_id x) { return db.is_index(x) && db.is<n_ident>(db.index_of(x).object); }

  void eval_node(node_id n, n_var const & v) {
    m_task & t = top();
    if(v.ref) { // &a = b is the second name of the same cell; &e = v[i] is a reference to an element
      if(t.phase == 0) {
        t.phase = 1;
        if(elem_place(db, v.init) && !ref_root(v.init, t.scope)) return push_elem_ref(v.init, t.scope, t.frame);
        if(db.is_member(v.init) || db.is_index(v.init)) return push_path_ref(v.init, t.scope, t.frame); // path: field, field element, from reference
        if(!db.is<n_ident>(v.init)) throw diagnostic("auto& — ссылка на переменную, поле или элемент массива", db[v.init].where);
        return push_eval(v.init, t.scope, t.frame);
      }
      mval x = pop_val();
      if(!x.is<m_alias>()) throw diagnostic("auto& — ссылка на переменную, поле или элемент массива", db[v.init].where);
      declare(t.scope, v.name, std::move(x));
      return finish(mval{m_unit{}});
    }
    if(v.init && t.phase == 0) {
      t.phase = 1;
      return push_eval(v.init, t.scope, t.frame);
    }
    mval x = v.init ? deref(pop_val()) : mval{m_unit{}};
    if(v.type && db.is<n_ident>(v.type) && numeric_pair(x, helper.str(db.as<n_ident>(v.type).name))) { // T x = e is imaginable (§5.8)
      auto want = helper.str(db.as<n_ident>(v.type).name);
      mval got;
      if(v.strict || !type_fit(x, want, v.strict, got)) {
        diagnostic d(cat("значение ", describe(x), " не представимо в ", want), db[v.init].where, diag_kind::runtime);
        d.type = "unpack_failed";
        throw d;
      }
      x = std::move(got);
    } else if(v.type && db.is<n_ident>(v.type) && !v.strict) { // Transformation by rule (f64 d = m - implicit programs)
      mval got;
      if(int q = type_fit(x, helper.str(db.as<n_ident>(v.type).name), false, got); q == 1 || q == 3) x = std::move(got);
    }
    if(v.type && db.is<n_ident>(v.type) && !v.strict) x = to_type(std::move(x), helper.str(db.as<n_ident>(v.type).name), db[v.init].where); // point p = {x: …}
    if(v.init) take_from(v.init, x, t.scope);
    declare(t.scope, v.name, std::move(x));
    finish(mval{m_unit{}});
  }

  // The name {a, b} is an aggregate by field positions (missing - default values
  // calculated in the global domain); array {…} / [...] - array (different type -
  // tuple, author's decision).
  void eval_node(node_id n, n_construct const & c) {
    m_task & t = top();
    if(!c.type) { // {x: e, …} - anonymous unit: type by structure - the assistant
      if(t.phase < c.items.size()) return push_eval(c.items[t.phase++], t.scope, t.frame);
      field_list fs;
      for(uint32_t i = 0; i < c.items.size(); ++i) {
        for(auto const & [name, _]: fs)
          if(name == c.names[i]) throw diagnostic(cat("{…}: поле ", helper.str(name), " дважды"), db[c.items[i]].where);
        fs.push_back({c.names[i], to_value(deref(fib().vals[t.base + i]))});
      }
      return finish(from_value(helper.record_value(std::move(fs)), db[n].where));
    }
    if(db.is<n_ident>(c.type) && t.phase == 0) helper.service_guard(helper.str(db.as<n_ident>(c.type).name), n, "создать"); // serviceman
    if(!db.is<n_ident>(c.type) || helper.node_kind_named(helper.str(db.as<n_ident>(c.type).name))) { // ast.call {…} - node type of program: builds an assistant
      auto v = helper_eval(n, t.scope, std::nullopt, db[n].where);
      if(!v) throw diagnostic("узел кода: не удалось построить", db[n].where);
      return finish(std::move(*v));
    }
    symbol tname = db.as<n_ident>(c.type).name;
    auto st_it = helper.structs.find(tname);
    node_list all;
    if(st_it != helper.structs.end()) all = helper.fields_of_struct(st_it->first);
    if(t.phase < c.items.size()) {
      node_id item = c.items[t.phase++];
      if(db.is<n_spread>(item)) { // ...a are elements of the array
        if(st_it != helper.structs.end()) throw diagnostic("...: раскрытие — в array{…} или аргументах вызова", db[item].where);
        item = db.as<n_spread>(item).value;
      }
      return push_eval(item, t.scope, t.frame);
    }
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
      for(uint32_t i = 0; i < all.size(); ++i) { // Type field: {x: …} and {} (and by default) become
        auto const & f = db.as<n_var>(all[i]);
        mval v = deref(fib().vals[t.base + i]);
        if(f.type && db.is<n_ident>(f.type)) v = to_type(std::move(v), helper.str(db.as<n_ident>(f.type).name), db[i < c.items.size() ? c.items[i] : f.init].where);
        g.fields.push_back({f.name, std::move(v)});
      }
      mval r{std::move(g)};
      if(st.res_on) {
        for(uint32_t i = 0; i < c.items.size(); ++i) take_from(c.items[i], r.as<m_agg>().fields[i].v, t.scope); // name-field
        mark_deep(r);
      }
      return finish(std::move(r));
    }
    if(helper.str(tname) != "array" && helper.types.builtin(helper.str(tname)) && c.items.size() == 1) { // Tag: i64 {x} - as i64 y = x
      std::string_view want = helper.str(tname);
      mval x = deref(fib().vals[t.base]), got;
      int r = type_fit(x, want, false, got);
      if(r == 0 && numeric_pair(x, want)) {
        diagnostic d(cat("значение ", describe(x), " не представимо в ", want), db[c.items[0]].where, diag_kind::runtime);
        d.type = "unpack_failed";
        throw d;
      }
      if(r == 0) throw diagnostic(cat("ожидался тип ", want, ", получен ", describe(x)), db[c.items[0]].where);
      return finish(r == 1 || r == 3 ? std::move(got) : std::move(x));
    }
    if(helper.str(tname) != "array") throw diagnostic(cat("конструктор ", helper.str(tname), "{} пока не поддержан"), db[n].where);
    m_arr a;
    for(uint32_t i = 0; i < c.items.size(); ++i) {
      mval v = deref(fib().vals[t.base + i]);
      if(!db.is<n_spread>(c.items[i])) {
        if(st.res_on) take_from(c.items[i], v, t.scope); // name element - transfer or copy
        a.items.push_back(std::move(v));
        continue;
      }
      if(!v.is<m_arr>()) throw diagnostic(cat("...: раскрывается массив или кортеж, получено ", describe(v)), db[c.items[i]].where);
      for(auto & e: v.as<m_arr>().items) a.items.push_back(e);
    }
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
    rules_for_blocks(n, t.scope);
    finish(mval{m_unit{}});
  }

  // The rule in the body of a structure is for its blocks (as in the interpreter,
  // §4.4): the parameters-blocks remember the area of the body; the block is executed
  // in a layer of rules over the area of the caller.
  void rules_for_blocks(node_id n, uint32_t scope) {
    node_id b = db[n].parent;
    uint32_t params = st.scopes[scope].parent;
    if(params == m_none || !helper.rule_decl(n) || !db.is<n_block>(b) || !db[b].parent || !db.is<n_fn>(db[b].parent) || db.as<n_fn>(db[b].parent).body != b) return;
    for(auto & v: st.scopes[params].vars)
      if(v.v.is<m_thunk>() && helper.block_arg(v.v.as<m_thunk>().expr)) v.v.as<m_thunk>().rules = scope;
  }
  uint32_t ruled_scope(m_thunk const & th) {
    if(th.rules == m_none) return th.env;
    uint32_t layer = new_scope(th.env);
    st.scopes[layer].layer = true;
    std::vector<m_var> rules;
    for(auto const & v: st.scopes[th.rules].vars)
      if(v.v.is<m_fn>() && !v.v.as<m_fn>().set.empty() && helper.rule_decl(v.v.as<m_fn>().set.front())) rules.push_back(v);
    for(auto & v: rules) declare(layer, v.name, std::move(v.v));
    return layer;
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
  void eval_try_compile(node_id body) {
    m_task & t = top();
    if(t.phase == 0) {
      t.phase = 1;
      return push_eval(body, t.scope, t.frame);
    }
    finish(deref(pop_val()));
  }

  // link to the ad (ast.reference): function - value by current name, type, ct-variable
  void eval_node(node_id n, n_ref const & r) {
    node_id d(r.target);
    if(db.is<n_fn>(d)) return finish(mval{m_fn{db.as<n_fn>(d).name, {}, m_none}});
    if(db.is_struct(d)) return finish(mval{m_type{helper.types.named(helper.str(db.as<n_extend>(d).name))}});
    if(db.is<n_var>(d))
      if(place p = find(st.globals, db.as<n_var>(d).name); p.scope != m_none) return finish(cell_value(p));
    throw diagnostic("ref: цель ссылки не объявлена", db[n].where);
  }

  // { … } - code value: considers assistant (free names - domain values), code value - node of
  // the base, common to the process and assistant
  void eval_node(node_id n, n_quote const &) {
    auto v = helper_eval(n, top().scope, std::nullopt, db[n].where);
    if(!v) throw diagnostic("quote: не удалось построить код", db[n].where);
    finish(std::move(*v));
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
  //
  // Dump is the header (base journal, process level metadata, state location) and the level
  // page as it is: values are not serialized.
  std::string dump() {
    save_state_vars(false); // Program state to disk: the resumed process has the same
    st.type_names.clear();
    for(uint32_t i = 0; i < helper.types.names.size(); ++i) st.type_names.emplace_back(helper.types.nm(i));
    dump_head h;
    h.name = name;
    h.root = root.value;
    h.journal = db.journal();
    h.base = uint64_t(band.lo);
    h.small_used = band.small_used;
    h.small_committed = band.small_committed;
    h.big_next = uint64_t(band.big_next - band.lo);
    for(uint32_t k = 0; k < address_space::classes; ++k)
      for(uint64_t * p: band.small_free[k]) h.small_free[k].push_back(uint64_t(reinterpret_cast<std::byte *>(p) - band.lo));
    for(uint64_t * p: band.big_free) h.big_free.push_back(uint64_t(reinterpret_cast<std::byte *>(p) - band.lo));
    h.state_at = uint64_t(reinterpret_cast<std::byte *>(&st) - band.lo);
    h.pieces = band.pieces;
    writer w0;
    w0.put(h);
    h.pages_at = (w0.bytes.size() + address_space::page - 1) / address_space::page * address_space::page; // Pages - from the boundary of the page (display)
    writer w;
    w.put(h);
    w.bytes.resize(h.pages_at, 0);
    for(auto const & p: h.pieces) w.bytes.append(reinterpret_cast<char const *>(band.lo + p.addr), p.len);
    return std::move(w.bytes);
  }
};

// Recovery in another VM: base - from journal dump, state - as it was.
struct restored_process {
  ast_db db;
  std::optional<process_machine> m;

  explicit restored_process(std::string_view bytes, int fd = -1) {
    reader r(bytes);
    dump_head h = r.get<dump_head>();
    if(h.magic != "VISHPROC10")
      throw diagnostic("не дамп процесса vish", {});
    db.replay(h.journal);
    m.emplace(db, node_id(h.root), h.name, bytes, fd);
  }
};

} // namespace vish
