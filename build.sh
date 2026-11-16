#!/bin/sh
# HO-Build: one translation unit, one compiler command.
#   ./build.sh            — release
# ./build.sh debug - debugging, contracts are checked
# ./build.sh asan - debugging + AddressSanitizer/UBSan
#
# Primitives native "cpp" { … } from libraries (§3.3) VM compiles: assembled vish takes
# them out in out/native.hpp (`vish native`); if the title has changed - a second
# build.
set -e
cd "$(dirname "$0")"

CXX=${CXX:-g++-16}
OUT=${OUT:-out/vish} # свой выход — сборки режимов параллельно (full.sh)
# -flto=auto: the same TU and one command, but code generation (80 - 90% of the time) GCC
# cuts to pieces and runs through all cores: release 89 → 39 s, asan 190 → 63 s.
FLAGS="-std=c++26 -freflection -fcontracts -flto=auto -Iinclude -Iout -Wall -Wextra -Wno-unused-parameter ${EXTRA:-}"
mkdir -p out
[ -f out/native.hpp ] || printf '#pragma once\nnamespace vish {\nstd::span<native_entry const> native_table() { return {}; }\n}\n' > out/native.hpp

build() {
    case "$1" in
        release) $CXX $FLAGS -O2 -DNDEBUG -fcontract-evaluation-semantic=ignore src/vish.cpp -o "$OUT" -luring ;;
        debug)   $CXX $FLAGS -O0 -g src/vish.cpp -o "$OUT" -luring ;;
        # -fno-var-tracking: local places at -O1 for gdb are not built - 113 → 63 with build (GCC
        # is quadratic on huge functions); lines in ASan reports remain, debug mode
        #
# 
        asan)    $CXX $FLAGS -O1 -g -fno-var-tracking -fsanitize=address,undefined src/vish.cpp -o "$OUT" -luring ;;
        *) echo "неизвестный режим: $1" >&2; exit 2 ;;
    esac
}

# Assemblies of modes go in parallel (check.sh): the header could update the neighboring mode
# while this assembly was going, reassembly, if it is not the one with which it was collected.
mode=${1:-release}
before=$(cksum < out/native.hpp)
build "$mode"
rc=0
tmp=out/native.hpp.$$ # замена переименованием — соседняя сборка не прочтёт половину
cp out/native.hpp "$tmp"
"$OUT" native "$tmp" lib/tools.vish lib/versions.vish lib/tests.vish lib/cli.vish lib/lexer.vish lib/parser.vish lib/printer.vish lib/runlog.vish lib/editor.vish || rc=$?
if [ $rc = 3 ]; then
    mv "$tmp" out/native.hpp
else
    rm -f "$tmp"
    [ $rc = 0 ] || exit $rc
fi
if [ "$(cksum < out/native.hpp)" != "$before" ]; then
    echo "out/native.hpp обновлён — пересборка"
    build "$mode"
fi
echo "$OUT ($mode)"
