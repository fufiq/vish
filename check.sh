#!/bin/sh
# Complete check before the committ: three assemblies - simultaneously (code generation each -
# on the cores, -flto=auto), tests of the mode - immediately upon the readiness of its assembly.
# ./check.sh - release, debug, asan + --a64, TZ_JIT_SELF, editor
# ./check.sh quick is release only and its output code 0 is all green. Logs -
# out/check/. Working out/tz - release.
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
  (cd "$d" && "$OLDPWD/$b" "$@") > "$l" 2>&1; c=$?
  echo "$n: $(tail -1 "$l") (код $c)" > $L/r_$n
}
mode() { # режим: сборка → тесты; release — ещё --a64 и самокомпиляция
  m=$1 b=out/tz-$1
  if ! OUT=$b ./build.sh "$m" > $L/b_$m.log 2>&1; then
    echo "$m: сборка упала (код $?, $L/b_$m.log)" > $L/r_$m
    return
  fi
  suite "$m" $L/t_$m.log $b test tests
  if [ "$m" = release ]; then
    cp $b out/tz
    suite a64 $L/t_a64.log $b test tests --a64 &
    TZ_JIT_SELF=1 suite self $L/t_self.log $b test tests &
    if [ -d editor/node_modules ]; then
      $b schema > editor/src/protocol/schema.json
      (cd editor && npx tsc --noEmit && npx vitest run) > $L/t_editor.log 2>&1
      echo "editor: код $?" > $L/r_editor
    fi
    wait
  fi
}

start=$(date +%s)
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
