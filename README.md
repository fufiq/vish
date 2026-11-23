# vish

An experimental programming language with its own VM and JIT, written in C++26.

The program is not text. It lives inside the VM as a semantic database (an AST with stable node identity), and the text you read and edit is only a projection of it. Compile time and run time share one semantics, polymorphism comes from instantiation, inlining is part of the language semantics, and the backend generates machine code directly (x86-64, AArch64) without LLVM.

> Status: research prototype. The language specification (v0.3 draft, `docs/spec.md`) and the design notes in `docs/` are written in Russian. Source comments are in English. The VM prints diagnostics in Russian; the English message catalog is not done yet.

## Design principles

1. **One source of truth.** The semantic AST database is authoritative, text is a projection. Edits are local and reversible.
2. **Small core, extensible runtime.** A minimal base language; syntax, operators and constructs are defined by code in the language itself.
3. **One semantics for every stage.** Compile-time and run-time code behave the same, the stage is inferred, there is no `constexpr`.
4. **Polymorphism by instantiation.** Code is a template specialised by actual types and values; types are nominal tags.
5. **Inlining is semantics, not an optimisation.** Behaviour never depends on compiler heuristics.
6. **No undefined behaviour.** Behaviour is defined positively ("what certainly exists"), not by a list of prohibitions.
7. **Static exceptions and lexical contexts.** No runtime cost for the normal path; concurrency without function colouring (stackful fibers).
8. **Reactive and incremental.** A change invalidates only what depends on it, including inside a running VM (live patching).
9. **The VM owns every program.** Closed type system, no ABI between programs, static isolation, resources are granted explicitly.

## What is in the repository

| Path | Contents |
|---|---|
| `include/vish/` | The VM: AST database, lexer and parser with error recovery, interpreter, static analysis, JIT (`jit/`), fibers, processes, IPC on shared memory, storage services |
| `lib/` | The prelude and tooling written in the language itself: command line, parser, editor model, versions and merge |
| `src/vish.cpp` | Entry point (single translation unit build) |
| `editor/` | Electron editor with a typed protocol to the VM: completion, diagnostics, debugger with stepping back and forth, semantic diff and three-way merge |
| `tests/` | 416 regression scenarios (`.vish`, `.wire` editor sessions, shell and C++ drivers) |
| `bench/`, `tools/` | Benchmarks, JIT-versus-interpreter fuzzer, self-hosting scripts |
| `docs/` | Specification and design notes (Russian) |

## A taste of the language

```
module demo;

dot(a, b) {
  require(a.size == b.size);
  s = zero(typeof(a[0]));
  for(i : range(a.size)) s = s + (a[i] * b[i]);
  s
}

try(expr e) {
  r = e;
  if(r.is_err()) return(caller) r;
  r.value
}

unless(c, expr body) { if(!c) body; }
```

`dot` is instantiated per argument types and compiled to machine code by the JIT; `try` and `unless` are ordinary library functions that extend the language (a caller-return and a lazily evaluated body).

## Build and run

Requirements: Linux, a C++26 compiler with reflection and contracts (`g++-16` is what the script uses), `liburing`.

```sh
./build.sh            # release build to out/vish
./build.sh debug      # contracts checked
./build.sh asan       # AddressSanitizer + UBSan

out/vish test tests   # run the regression suite
./check.sh quick      # release build and tests; ./check.sh runs all modes
```

Commands include `run`, `check`, `fmt`, `jit`, `slice`, `residual`, `stats`, `diff`, `merge`, `serve` (editor protocol over a Unix socket) and `vm` (reactive session).

## Notes

- The file extension `.tzdb` and the `tz` name in early history are from the project's former name.
- The project is developed solo; the commit history is a curated set of milestones.

## License

GNU General Public License v3.0 or (at your option) any later version; see [`LICENSE`](LICENSE).
SPDX-License-Identifier: GPL-3.0-or-later
