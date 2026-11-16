#!/bin/sh
# host itself: lib/*.vish → repository (vish lib import) → text (vish lib export) → again; the second circle gives the same text, each
# module is disassembled. Use: tests/selfhost.sh <vish> <catalogue >>
set -e
b=$1; d=$2
rm -rf "$d"; mkdir -p "$d/p1" "$d/p2"; cd "$d"
lib=$(dirname "$b")/../lib
$b lib import "$lib" r1.vishrepo > /dev/null
$b lib export r1.vishrepo p1 > /dev/null
$b lib import p1 r2.vishrepo > /dev/null
$b lib export r2.vishrepo p2 > /dev/null
for f in prelude tools versions tests cli editor lexer parser runlog modules views complete session proto server; do
  cmp -s "p1/$f.vish" "p2/$f.vish" || { echo "второй круг изменил $f.vish"; diff "p1/$f.vish" "p2/$f.vish" | head -5; exit 1; }
  $b fmt "p1/$f.vish" > /dev/null 2> err.txt || { echo "$f.vish не разбирается"; head -3 err.txt; exit 1; }
done
grep -q "^\[\[service\]\] module editor;" p1/editor.vish || { echo "потерян [[service]] у editor"; exit 1; }
echo ok
