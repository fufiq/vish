#!/bin/sh
# The examples require modules next to the program (§9.1).
set -eu
b=$(realpath "${1:-out/vish}")
root=$(pwd)
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
for m in reactive parser lexer tools; do ln -s "$root/lib/$m.vish" "$d/$m.vish"; done
for f in graphs modcheck_demo; do
  cp "$root/tests/data/reactcheck/$f.vish" "$d/$f.vish"
  "$b" run "$d/$f.vish"
done
