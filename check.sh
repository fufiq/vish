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
