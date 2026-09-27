// Long-lived VM session: AST base + jet cache parsing (§10.1 steak, §9.3).
//
// function editing - operation on the base (new import + replacement of node in journal);
// the next check recalculates only instance, depending on the changed, the next run goes to
// the new version.
//
// Commands (by line):
// check static parsing; prints what is recalculated and what from the cache
// Full report from the last check
// stdin <line> string stdin for the next run (queue; session stdin - not process stdin)
// run [arguments] to perform main()
// Residual [arguments] program for given arguments
// fmt text projection
// edit <function> then the lines of the new function, the end is a line from one point
// Save the way. tzdb> write down journal
#pragma once

#include "interp.hpp"
#include "parser.hpp"
#include "printer.hpp"
#include "store.hpp"
#include "views.hpp"

#include <sstream>

namespace tz {

class vm_session {
public:
    vm_session(ast_db& db, node_id root, std::string name) : db_(db), root_(root), name_(std::move(name)) {}

    // Execute one command; to edit transfer the body to `body`.
    std::string exec(const std::string& line, const std::string& body = {}) {
        std::istringstream in(line);
        std::string        cmd;
        in >> cmd;
        std::vector<std::string> args;
        for (std::string a; in >> a;) args.push_back(a);
        try {
            if (cmd == "check") return check();
            if (cmd == "stdin") {
                stdin_.push_back(line.size() > 6 ? line.substr(6) : "");
                return {};
            }
            if (cmd == "report") return last_report_;
            if (cmd == "run") return run(args);
            if (cmd == "residual") return residual(args);
            if (cmd == "fmt") return printer(db_).print(root_);
            if (cmd == "edit" && args.size() == 1) return edit(args[0], body);
            if (cmd == "save" && args.size() == 1) {
                save_db(db_, root_, args[0]);
                return std::format("сохранено: {}\n", args[0]);
            }
        } catch (const diagnostic& d) {
            return render(d, name_, db_);
        }
        return std::format("неизвестная команда: {}\n", line);
    }

private:
    std::string check() {
        cache_.hits = cache_.recomputed = 0;
        cache_.recomputed_names.clear();
        interpreter vm(db_, root_, {name_}, mode::check, std::nullopt, &cache_);
        std::string err;
        try {
            vm.run_main();
        } catch (const diagnostic& d) {
            err = render(d, name_, db_);
        }
        last_report_ = vm.report(name_, db_.source(0));
        std::string names;
        for (auto& n : cache_.recomputed_names) names += (names.empty() ? "" : ", ") + n;
        return err + std::format("check: пересчитано {} экземпл. ({}), из кэша {}\n", cache_.recomputed,
                                 names.empty() ? "—" : names, cache_.hits);
    }

    std::string run(const std::vector<std::string>& args) {
        std::vector<std::string> a{name_};
        a.insert(a.end(), args.begin(), args.end());
        interpreter vm(db_, root_, a, mode::run, interpreter::known_inputs{stdin_});
        std::string out;
        vm.capture_output(&out);
        try {
            vm.run_main();
        } catch (const diagnostic& d) {
            out += render(d, name_, db_);
        }
        return out;
    }

    std::string residual(const std::vector<std::string>& args) {
        std::vector<std::string> a{name_};
        a.insert(a.end(), args.begin(), args.end());
        interpreter vm(db_, root_, a, mode::check, interpreter::known_inputs{});
        std::string err;
        try {
            vm.run_main();
        } catch (const diagnostic& d) {
            err = render(d, name_, db_);
        }
        return err + residual_view(db_, vm).print(root_);
    }

    // Function correction: the text is disassembled into the base (new import), the node of the old
    // function is replaced with new content - id and all references to the function are saved.
    std::string edit(const std::string& fn_name, const std::string& text) {
        parser  p(db_, text, std::format("<правка {} #{}>", fn_name, ++edits_));
        node_id snippet = p.parse_module();
        auto    errs    = p.errors();
        if (!errs.empty()) {
            std::string out;
            for (auto& e : errs) out += render(e, std::format("правка {}", fn_name), db_);
            return out + "правка отклонена\n";
        }
        node_id fresh;
        for (node_id item : db_.as<n_module>(snippet).items)
            if (db_.is<n_fn>(item) && db_.strings.text(db_.as<n_fn>(item).name) == fn_name) fresh = item;
        if (!fresh) return std::format("в тексте правки нет функции {}\n", fn_name);

        for (node_id item : db_.as<n_module>(root_).items)
            if (db_.is<n_fn>(item) && db_.strings.text(db_.as<n_fn>(item).name) == fn_name) {
                db_.replace(item, db_[fresh].kind);
                db_.link_parents(root_);
                return std::format("функция {} заменена (узел #{}, ревизия {})\n", fn_name, item.value(), db_.revision());
            }
        // No new function was added to the module (changes the set of overloads)
        auto m = db_.as<n_module>(root_);
        m.items.push_back(fresh);
        db_.replace(root_, m);
        db_.link_parents(root_);
        return std::format("функция {} добавлена\n", fn_name);
    }

    ast_db&     db_;
    node_id     root_;
    std::string name_;
    check_cache              cache_;
    std::vector<std::string> stdin_;
    u32                      edits_ = 0;
    std::string last_report_;
};

} // namespace tz
