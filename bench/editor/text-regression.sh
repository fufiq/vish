#!/bin/sh
# Verification of the present tools from the assembly (tooling modules are not imported by runner tests/).
set -eu
cd "$(dirname "$0")/../.."
vish=${1:-./out/vish}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed -n 's|^// out: ||p' bench/editor/text-regression.vish > "$tmp/expected"
"$vish" run bench/editor/text-regression.vish > "$tmp/actual"
diff -u "$tmp/expected" "$tmp/actual"
