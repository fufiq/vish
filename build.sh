#!/bin/sh
# HO-Build: one translation unit, one compiler command.
#   ./build.sh            — release
# ./build.sh debug - debugging, contracts are checked
# ./build.sh asan - debugging + AddressSanitizer/UBSan
set -e
cd "$(dirname "$0")"

CXX=${CXX:-g++-16}
FLAGS="-std=c++26 -freflection -fcontracts -Iinclude -Wall -Wextra -Wno-unused-parameter ${EXTRA:-}"
mkdir -p out

case "${1:-release}" in
    release) $CXX $FLAGS -O2 -DNDEBUG -fcontract-evaluation-semantic=ignore src/tz.cpp -o out/tz -luring ;;
    debug)   $CXX $FLAGS -O0 -g src/tz.cpp -o out/tz -luring ;;
    asan)    $CXX $FLAGS -O1 -g -fsanitize=address,undefined src/tz.cpp -o out/tz -luring ;;
    *) echo "неизвестный режим: $1" >&2; exit 2 ;;
esac
echo "out/tz ($1)"
