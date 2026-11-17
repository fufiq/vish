// Frozen RA-1b model at 79ed134: differential test reference only.
namespace vish::rdeps_oracle {
struct impact_reference {
  ast_db const & db;
  std::vector<entry> uses;
  std::unordered_map<std::string, std::vector<uint32_t>> by_text; // a name text -> the use nodes
  std::unordered_map<uint32_t, std::vector<uint32_t>> by_target;
  std::vector<uint32_t> fns;
  explicit impact_reference(ast_db const & d) : db(d), uses(expected_entries(d)) {
    for(auto const & e: uses) {
      if(e[0]) by_target[e[1]].push_back(e[2]);
      else by_text[std::string(db.strings.text(symbol(e[1])))].push_back(e[2]);
    }
    for(uint32_t i = 0; i < db.size(); ++i) if(std::holds_alternative<n_fn>(db[node_id(i)].kind)) fns.push_back(i);
  }
  static node_list sorted(node_list xs) {
    std::ranges::sort(xs, {}, &node_id::value);
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    return xs;
  }
  node_id fn_of(node_id n) const { // the nearest function up the parent chain
    for(; n; n = db[n].parent) if(std::holds_alternative<n_fn>(db[n].kind)) return n;
    return {};
  }
  bool inside(node_id n, node_id top) const {
    for(; n; n = db[n].parent) if(n == top) return true;
    return false;
  }
  std::string name_of(node_id d) const { // the name of a declaration entity: the name field of these kinds
    auto const & k = db[d].kind;
    symbol s;
    if(auto p = std::get_if<n_fn>(&k)) s = p->name;
    else if(auto p = std::get_if<n_module>(&k)) s = p->name;
    else if(auto p = std::get_if<n_extend>(&k)) s = p->name;
    else if(auto p = std::get_if<n_ident>(&k)) s = p->name;
    else if(auto p = std::get_if<n_var>(&k)) s = p->name;
    return s ? std::string(db.strings.text(s)) : std::string();
  }
  void add_text(std::string const & t, node_list & out) const {
    if(auto it = by_text.find(t); it != by_text.end()) for(auto i: it->second) out.push_back(node_id(i));
  }
  node_list users(node_id d) const {
    node_list out;
    auto name = name_of(d);
    if(!name.empty()) {
      add_text(name, out);
      if(name[0] != '.') add_text("." + name, out); // a.name is the postfix operator `.name`
    }
    if(auto it = by_target.find(d.value); it != by_target.end()) for(auto i: it->second) out.push_back(node_id(i)); // ref(f)
    if(std::holds_alternative<n_fn>(db[d].kind)) // an operator function: infix+ - the uses of the operator `+` (any role and arity)
      for(std::string_view role: {"infix", "prefix", "postfix"})
        if(name.size() > role.size() && name.starts_with(role)) { add_text(name.substr(role.size()), out); break; }
    return sorted(std::move(out));
  }
  bool in_fn_or_extend_parent(node_id n, node_id & ext, node_id & mod) const { // the nearest extension / module up the tree for a node outside functions
    ext = {}; mod = {};
    for(; n; n = db[n].parent) {
      if(std::holds_alternative<n_fn>(db[n].kind)) return false;
      if(std::holds_alternative<n_extend>(db[n].kind)) { ext = n; return true; }
      if(std::holds_alternative<n_module>(db[n].kind)) { mod = n; return true; }
    }
    return true;
  }
  // A function called implicitly by the core (ast_db::implicit_hooks is the common list of names; the mark by the [[transform]] attribute has its own parse).
  bool is_hook(node_id d) const {
    auto const * f = std::get_if<n_fn>(&db[d].kind);
    if(!f) return false;
    auto text = db.strings.text(f->name);
    for(auto const & h: ast_db::implicit_hooks) {
      if(h.name.empty() || h.name != text) continue;
      if(text == "eval") {
        auto p = db[d].parent;
        if(p && !(std::holds_alternative<n_extend>(db[p].kind) && db.strings.text(std::get<n_extend>(db[p].kind).name).starts_with("ast."))) continue;
      }
      return true;
    }
    for(auto a: f->attrs)
      if(a)
        if(auto const * id = std::get_if<n_ident>(&db[a].kind); id && db.strings.text(id->name) == "transform") return true;
    return false;
  }
  node_list dependents(node_id d) const {
    node_list out;
    for(auto u: users(d)) {
      auto f = fn_of(u);
      if(f) out.push_back(f);
      bool extend = std::holds_alternative<n_extend>(db[u].kind), use = std::holds_alternative<n_use>(db[u].kind);
      if(extend || use) { // an extension or use changes any function of its scope: an extension inside itself, use - the module
        node_id top = u;
        if(use) while(db[top].parent && !std::holds_alternative<n_module>(db[top].kind)) top = db[top].parent;
        for(auto g: fns) if(inside(node_id(g), top)) out.push_back(node_id(g));
      }
      if(!f && !extend && !use) { // outside functions: a field type/value inside an extension - its members; global variables, ct, module attributes - the module's functions
        node_id ext, mod;
        in_fn_or_extend_parent(u, ext, mod);
        node_id top = ext ? ext : mod;
        if(top && !in_prelude(top))
          for(auto g: fns) if(inside(node_id(g), top)) out.push_back(node_id(g));
      }
    }
    // the core's implicit calls (the table ast_db::implicit_hooks, the [[transform]] attribute): any function of the program (not of the prelude) is dependent
    if(is_hook(d))
      for(auto f: fns) if(!in_prelude(node_id(f))) out.push_back(node_id(f));
    return sorted(std::move(out));
  }
  bool in_prelude(node_id n) const {
    for(; n && !std::holds_alternative<n_module>(db[n].kind); n = db[n].parent) {}
    return n && n == db.prelude;
  }
  // impact: functions and modules - up the parent chain, the dependents, and the descendants of an entity as its dependents.
  node_list impact(node_list const & changed) const {
    // An independent fixed point over a reflective snapshot; the live index is not read.
    std::set<uint32_t> reached;
    for(auto n: changed) if(n && n.value < db.size()) reached.insert(n.value);
    bool reflection = false;
    for(uint32_t j = 1; j < db.size(); ++j)
      if(std::holds_alternative<n_ct>(db[node_id(j)].kind) && !in_prelude(node_id(j))) reflection = true;
    if(reflection && !reached.empty()) for(auto f: fns) reached.insert(f);
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    for(auto f: fns) for(auto caller: dependents(node_id(f))) edges.emplace_back(caller.value, f);
    std::set<uint32_t> suffix_items, ordered, descended;
    // A structural snapshot through field reflection, without the live index's walk/rules.
    std::vector<node_list> children(db.size());
    for(uint32_t j = 1; j < db.size(); ++j) std::visit([&](auto const & k) {
      template for(constexpr auto field: fields_of<std::remove_cvref_t<decltype(k)>>()) {
        using F = [:std::meta::type_of(field):];
        if constexpr(std::is_same_v<F, node_id>) { if(k.[:field:]) children[j].push_back(k.[:field:]); }
        else if constexpr(std::is_same_v<F, list<node_id>>) for(auto c: k.[:field:]) if(c) children[j].push_back(c);
      }
    }, db[node_id(j)].kind);
    bool more = true;
    while(more) {
      more = false;
      auto put = [&](node_id n) { if(n && n.value < db.size()) more |= reached.insert(n.value).second; };
      auto current = reached;
      for(auto id: current) {
        node_id n(id);
        put(db[n].parent);
        for(auto d: dependents(n)) put(d);
        if(ordered.insert(id).second) {
          auto const & k = db[n].kind;
          bool rule = std::holds_alternative<n_fn>(k) || std::holds_alternative<n_use>(k)
            || std::holds_alternative<n_extend>(k) || std::holds_alternative<n_ct>(k);
          if(auto v = std::get_if<n_var>(&k)) rule |= v->ct;
          auto parent = db[n].parent;
          if(rule && parent) std::visit([&](auto const & container) {
            using K = std::remove_cvref_t<decltype(container)>;
            if constexpr(std::is_same_v<K, n_module> || std::is_same_v<K, n_block>
              || std::is_same_v<K, n_quote> || std::is_same_v<K, n_extend>) {
              bool after = false;
              for(auto item: container.items) {
                if(after && suffix_items.insert(item.value).second)
                {
                  auto visit = [&](auto && self, node_id child) -> void {
                    if(!descended.insert(child.value).second) return;
                    put(child);
                    for(auto c: children[child.value]) self(self, c);
                  };
                  visit(visit, item);
                }
                after |= item == n;
              }
            }
          }, db[parent].kind);
        }
        auto name = name_of(n);
        for(auto u: users(n)) {
          if(!name.empty())
            if(auto const * e = std::get_if<n_extend>(&db[u].kind); e && e->base && std::string(db.strings.text(e->base)) == name) put(u);
          node_id ext, mod;
          if(!fn_of(u) && !std::holds_alternative<n_extend>(db[u].kind) && in_fn_or_extend_parent(u, ext, mod) && ext) put(ext);
        }
      }
      for(auto [caller, callee]: edges) if(reached.contains(caller)) put(node_id(callee));
    }
    node_list out;
    for(auto id: reached)
      if(suffix_items.contains(id) || std::holds_alternative<n_fn>(db[node_id(id)].kind) || std::holds_alternative<n_module>(db[node_id(id)].kind)) out.push_back(node_id(id));
    return sorted(std::move(out));
  }

};
}
