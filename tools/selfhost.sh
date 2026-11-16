#!/bin/sh
# host itself (2026-11-10): the source lib/*.vish is stored in the Vish repository (lib.vishrepo, not in git), edited through the
# editor (`vish serve lib.vishrepo <сокет>`); git stores the canonical projection - lib/ text. Modules in the repository are keyed
# src.<name> (tools, versions, ... are occupied by built-in copies of the build).
# tools/selfhost.sh import lib/ → lib.vishrepo (recreated; out/vish needed)
#   tools/selfhost.sh export   lib.vishrepo → lib/
# tools/selfhost.sh build export, assembly (two passes: native.hpp), vish test; if lib/ and out/vish fail, two binaries return: a
# working out/vish.stable (they keep a repository and export) and a collected out/vish.
set -e
cd "$(dirname "$0")/.."
repo=lib.vishrepo
case "$1" in
  import)
    rm -f "$repo" "$repo.lock"
    out/vish lib import lib "$repo"
    ;;
  export)
    [ -f "$repo" ] || { echo "нет $repo — tools/selfhost.sh import"; exit 1; }
    out/vish lib export "$repo" lib
    ;;
  build)
    [ -f "$repo" ] || { echo "нет $repo — tools/selfhost.sh import"; exit 1; }
    cp out/vish out/vish.stable
    rm -rf out/lib.bak; mkdir -p out/lib.bak; cp lib/*.vish out/lib.bak/
    restore() { cp out/lib.bak/*.vish lib/; cp out/vish.stable out/vish; echo "сборка не удалась: lib/ и out/vish возвращены"; exit 1; }
    out/vish.stable lib export "$repo" lib || restore
    out/vish.stable native out/native.hpp lib/tools.vish lib/versions.vish lib/tests.vish lib/cli.vish lib/lexer.vish lib/parser.vish lib/runlog.vish lib/editor.vish || true
    ./build.sh release > out/selfhost.log 2>&1 || true
    ./build.sh release >> out/selfhost.log 2>&1 || restore
    out/vish test tests > out/selfhost.tests 2>&1 || { tail -5 out/selfhost.tests; restore; }
    tail -1 out/selfhost.tests
    ;;
  *) echo "использование: tools/selfhost.sh import|export|build"; exit 2 ;;
esac
