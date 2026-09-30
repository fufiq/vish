#!/bin/sh
# HO-Build: one translation unit, one compiler command.
#   ./build.sh            — release
# ./build.sh debug - debugging, contracts are checked
# ./build.sh asan - debugging + AddressSanitizer/UBSan
#
# The primitives of the native "cpp" { … } from libraries (§3.3) VM compiles into
# itself: the collected tz takes them out in out/native.hpp (`tz native`); if the
# title changes, the second build.
set -e
cd "$(dirname "$0")"

CXX=${CXX:-g++-16}
FLAGS="-std=c++26 -freflection -fcontracts -Iinclude -Iout -Wall -Wextra -Wno-unused-parameter ${EXTRA:-}"
mkdir -p out
[ -f out/native.hpp ] || printf '#pragma once\nnamespace tz {\nstd::span<native_entry const> native_table() { return {}; }\n}\n' > out/native.hpp

build() {
    case "$1" in
        release) $CXX $FLAGS -O2 -DNDEBUG -fcontract-evaluation-semantic=ignore src/tz.cpp -o out/tz -luring ;;
        debug)   $CXX $FLAGS -O0 -g src/tz.cpp -o out/tz -luring ;;
        asan)    $CXX $FLAGS -O1 -g -fsanitize=address,undefined src/tz.cpp -o out/tz -luring ;;
        *) echo "неизвестный режим: $1" >&2; exit 2 ;;
    esac
}

mode=${1:-release}
build "$mode"
rc=0
out/tz native out/native.hpp || rc=$?
if [ $rc = 3 ]; then
    echo "out/native.hpp обновлён — пересборка"
    build "$mode"
elif [ $rc != 0 ]; then
    exit $rc
fi
echo "out/tz ($mode)"
