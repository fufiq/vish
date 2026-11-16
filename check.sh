#!/bin/sh
# Complete check before the committ: three assemblies - simultaneously (code generation each -
# on the cores, -flto=auto), tests of the mode - immediately upon the readiness of its assembly.
# ./check.sh - release, debug (with VISH_SPLICE_CHECK sticker reconciliation), asan + --a64, VISH_JIT_SELF, projection, editor
# ./check.sh Quick - Release only and its tests
# ./check.sh full - as without a key, plus VISH_JIT_SELF under asan (≈7.5 min) Exit code
# 0 - all green. Logs - out/check/. Workout/vish - release.
cd "$(dirname "$0")"
L=out/check
mkdir -p $L
rm -f $L/r_*

suite() { # имя лог бинарь аргументы…
  # Each run is from its own directory: tests write files relative to it (58 files - out/ io_test
  # .txt), simultaneous runs otherwise interfere with each other
  n=$1 l=$2 b=$3; shift 3
  d=$L/run_$n
  rm -rf "$d"; mkdir -p "$d/out"; ln -s "$PWD/tests" "$d/tests"
  # IPC channels (test 380) - also its own directory: otherwise simultaneous runs share one channel name
  (cd "$d" && VISH_IPC_DIR="$PWD/ipc" "$OLDPWD/$b" "$@") > "$l" 2>&1; c=$?
  echo "$n: $(tail -1 "$l") (код $c)" > $L/r_$n
}
mode() { # режим: сборка → тесты; release — ещё --a64 и самокомпиляция (full: и asan)
  m=$1 b=out/vish-$1
  if ! OUT=$b ./build.sh "$m" > $L/b_$m.log 2>&1; then
    echo "$m: сборка упала (код $?, $L/b_$m.log)" > $L/r_$m
    return
  fi
  if [ "$m" = asan ] && [ "$FULL" = 1 ]; then # full: самокомпиляция и под asan (≈5 мин) — стек VM, не хоста
    VISH_JIT_SELF=1 suite self_asan $L/t_self_asan.log $b test tests &
  fi
  if [ "$m" = debug ]; then # вклейка черновика редактора сверяется с полной печатью на каждой клавише
    VISH_SPLICE_CHECK=1 suite "$m" $L/t_$m.log $b test tests
  else
    suite "$m" $L/t_$m.log $b test tests
  fi
  wait
  if [ "$m" = release ]; then
    cp $b out/vish
    suite a64 $L/t_a64.log $b test tests --a64 &
    VISH_JIT_SELF=1 suite self $L/t_self.log $b test tests &
    suite projection $L/t_projection.log $b test tests --projection geo.words & # второй язык: тот же АСТ (§4.5)
    # node pool invariants (S5.0): list and text by offsets - a separate small executable file
    CXX=${CXX:-g++-16}
    ( $CXX -std=c++26 -freflection -fcontracts -Iinclude tests/pool_list.cpp -o $L/pool_list && $L/pool_list ) > $L/t_pool.log 2>&1
    echo "pool: код $?" > $L/r_pool
    # base area in the storage (S5.1): close / open, another version, cropping, copy for samples, compaction
    ( rm -rf $L/area; mkdir -p $L/area; $CXX -std=c++26 -freflection -fcontracts -Iinclude tests/pool_area.cpp -o $L/pool_area && $L/pool_area $L/area ) > $L/t_area.log 2>&1
    echo "area: код $?" > $L/r_area
    # IPC between VM (shared memory + futex): two processes, ring rotation, end closure, occupied roles
    ( rm -rf $L/ipc; mkdir -p $L/ipc; $CXX -std=c++26 -freflection -fcontracts -Iinclude tests/ipc_ring.cpp -o $L/ipc_ring && $L/ipc_ring $L/ipc ) > $L/t_ipc.log 2>&1
    echo "ipc: код $?" > $L/r_ipc
    # Two true VMs: Writer and reader by IPC streams (futex via io_uring in shared memory), the total of the reader to the writer
    ( d=$L/ipc_two; rm -rf $d; mkdir -p $d
      export VISH_GRANT=ipc VISH_IPC_DIR=$PWD/$d/ch
      timeout 60 $b run tests/ipc/reader.vish > $d/reader.out 2>&1 &
      timeout 60 $b run tests/ipc/writer.vish > $d/writer.out 2>&1
      wait
      [ "$(cat $d/writer.out)" = "ok 2000 6890" ] ) > $L/t_ipc_two.log 2>&1
    echo "ipc_two: код $?" > $L/r_ipc_two
    # Ed25519 (RFC 8032): Slow (~4s staircase with JIT), so once, not in five dial modes
    ( timeout 300 $b run --jit tests/slow/ed25519.vish > $L/ed25519.out 2> $L/t_ed25519.log; cmp -s $L/ed25519.out tests/slow/ed25519.out || { echo "ed25519: вывод отличается от tests/slow/ed25519.out" >> $L/t_ed25519.log; diff $L/ed25519.out tests/slow/ed25519.out >> $L/t_ed25519.log; false; } )
    echo "ed25519: код $?" > $L/r_ed25519
    # Built-in modules of the base - from the assembly: a copy of another build is replaced, the same - remains (a separate executable file)
    ( $CXX -std=c++26 -freflection -fcontracts -O0 -Iinclude -Iout tests/builtin_modules.cpp -o $L/builtin_modules -luring && $L/builtin_modules ) > $L/t_builtin.log 2>&1
    echo "builtin: код $?" > $L/r_builtin
    # exchange between repositories: pull/push, repeat, your pictures on the branch, discrepancy journal
    ( sh tests/exchange.sh $PWD/$b $L/exchange ) > $L/t_exchange.log 2>&1
    echo "exchange: код $?" > $L/r_exchange
    # Text export of the repository (vish repo export) and return via vish repo init
    ( sh tests/export.sh $PWD/$b $L/export ) > $L/t_export.log 2>&1
    echo "export: код $?" > $L/r_export
    # host itself : lib/*.vish through the repository back and forth (vish lib import/export), second circle - the same text
    ( sh tests/selfhost.sh $PWD/$b $L/selfhost ) > $L/t_selfhost.log 2>&1
    echo "selfhost: код $?" > $L/r_selfhost
    # Transfer of C++ to vish: Lexer in language (lib/lexer.vish) vs C++ stage0 - dumps of tokens, comments and causes of holes coincide
    ( $b lexcheck tests/data/lexer_edge*.txt lib/cli.vish lib/lexer.vish $(ls tests/*.vish | head -60) ) > $L/t_lexer.log 2>&1
    echo "lexer: код $?" > $L/r_lexer
    # transfer to vish, step 2: phase 1 parser (brackets, indentation repairs, names ast.*) in language vs. C++ stage0
    ( $b parsecheck tests/data/*.txt lib/lexer.vish lib/parser.vish $(ls tests/*.vish | head -30) ) > $L/t_parser.log 2>&1
    echo "parser: код $?" > $L/r_parser
    # Transfer to vish, step 3a: Phase 2 expressions (literals, names, calls, postfixes, operator series) in language vs. C++ stage0
    ( $b exprcheck tests/data/expr_corpus.txt tests/data/expr_random.txt ) > $L/t_expr.log 2>&1
    echo "expr: код $?" > $L/r_expr
    # epic vsh-u4e: the environment object journal (lib/runlog.vish): write, blobs, truncation, corruption, cause chain; each run uses its own directory
    ( rm -rf $L/runlog && mkdir -p $L/runlog && $b runlogcheck $PWD/$L/runlog ) > $L/t_runlog.log 2>&1
    echo "runlog: код $?" > $L/r_runlog
    # environment object journal in the editor: an acceptance scenario -> cause chain; the second process runs after a restart
    ( rm -rf $L/runlog_wire $L/runlog_wire_stale && mkdir -p $L/runlog_wire $L/runlog_wire_stale && VISH_RUNLOG_DIR=$PWD/$L/runlog_wire $b test tests/runlog/obj.wire && VISH_RUNLOG_DIR=$PWD/$L/runlog_wire $b test tests/runlog/obj_after.wire && VISH_RUNLOG_DIR=$PWD/$L/runlog_wire_stale $b test tests/runlog/obj_stale.wire ) > $L/t_runlog_wire.log 2>&1
    echo "runlog_wire: код $?" > $L/r_runlog_wire
    # retention policy: bodies switched off (metadata stays) and the blob limit (host variables, not the agent)
    ( rm -rf $L/runlog_off $L/runlog_cap && mkdir -p $L/runlog_off $L/runlog_cap \
      && VISH_RUNLOG_BODIES=off VISH_RUNLOG_DIR=$PWD/$L/runlog_off $b test tests/runlog/obj.wire \
      && [ -z "$(find $L/runlog_off -name 'blob.*')" ] \
      && VISH_RUNLOG_MAX_BLOB=10 VISH_RUNLOG_DIR=$PWD/$L/runlog_cap $b test tests/runlog/obj.wire \
      && [ -n "$(find $L/runlog_cap -name 'blob.*')" ] && [ -z "$(find $L/runlog_cap -name 'blob.*' -size +18c)" ] ) > $L/t_runlog_policy.log 2>&1
    echo "runlog_policy: код $?" > $L/r_runlog_policy
    # close/open: a database saved later than the text is restored on `vish serve X.vish`
    ( sh tests/restore.sh $PWD/$b $L/restore ) > $L/t_restore.log 2>&1
    echo "restore: код $?" > $L/r_restore
    # JIT to random program interpreter (tools/jit_fuzz .py): three code generation errors per day were random
    if command -v python3 > /dev/null; then
      ( python3 tools/jit_fuzz.py 24 1 $PWD/$b ) > $L/t_jitfuzz.log 2>&1
      echo "jitfuzz: код $?" > $L/r_jitfuzz
    fi
    if [ -d editor/node_modules ]; then
      $b schema > editor/src/protocol/schema.json
      (cd editor && npx tsc --noEmit && npx vitest run) > $L/t_editor.log 2>&1
      echo "editor: код $?" > $L/r_editor
    fi
    wait
  fi
}

start=$(date +%s)
FULL=0
[ "$1" = full ] && FULL=1
if [ "$1" = quick ]; then
  mode release
else
  mode release & mode debug & mode asan &
  wait
fi
ok=0
for r in $L/r_*; do
  cat "$r"
  grep -q "(код 0)\|: код 0" "$r" || ok=1
done
echo "итого: $(( $(date +%s) - start )) с, $([ $ok = 0 ] && echo зелёное || echo ЕСТЬ ПАДЕНИЯ)"
exit $ok
