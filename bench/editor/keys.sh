#!/bin/sh
# Editor key response: .wire scripts over ~150 lines (tests/79 generator loop.tz) through a real server socket (tz test).
# The key time is (scenario - opening only) / key, the best of five; on a hybrid processor - on a production kernel
# (taskset). ./bench/editor/keys.sh [Road to tz]
set -e
cd "$(dirname "$0")"
TZ=${1:-../../out/tz}
PIN=
[ -r /sys/devices/cpu_core/cpus ] && command -v taskset > /dev/null && PIN="taskset -c $(cut -d- -f1 /sys/devices/cpu_core/cpus | cut -d, -f1)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
prog=$(realpath ../../tests/79_generator_loop.tz)
head() { printf '// program: %s\nopen_view module\npointer 1 @gen_squares 0 0\n' "$prog"; }
head > "$tmp/open.wire"
{ head; i=0; while [ $i -lt 10 ]; do echo "key 1 End"; echo "key 1 Enter"; for c in s u m m a r i z e _ a l l _ v a l u e s; do echo "key 1 $c"; done; i=$((i+1)); done; } > "$tmp/type.wire"
{ head; i=0; while [ $i -lt 50 ]; do echo "key 1 ArrowDown"; echo "key 1 ArrowRight"; echo "key 1 ArrowRight"; echo "key 1 ArrowUp"; i=$((i+1)); done; } > "$tmp/move.wire"
best() {
  b=
  for r in 1 2 3 4 5; do
    s=$(date +%s%N); $PIN "$TZ" test "$1" > /dev/null; e=$(date +%s%N)
    t=$(( (e - s) / 1000 ))
    [ -z "$b" ] || [ $t -lt $b ] && b=$t
  done
  echo $b
}
o=$(best "$tmp/open.wire")
t=$(best "$tmp/type.wire")
m=$(best "$tmp/move.wire")
echo "открытие: $((o / 1000)) мс"
echo "набор (дополнение, подсказка): $(( (t - o) / 220 )) мкс на клавишу"
echo "движение: $(( (m - o) / 200 )) мкс на клавишу"
