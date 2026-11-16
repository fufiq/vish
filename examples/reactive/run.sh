#!/bin/sh
# An example of the reactive query engine: modules are taken next to the program (spec §9.1).
set -eu
b=$(realpath "${1:-out/vish}")
root=$(pwd)
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
for m in reactive tools; do ln -s "$root/lib/$m.vish" "$d/$m.vish"; done
cp "$root/examples/reactive/live.vish" "$d/live.vish"
"$b" run "$d/live.vish"
