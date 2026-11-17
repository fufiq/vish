#!/bin/sh
set -eu
b=$(realpath "${1:-out/vish}")
root=$PWD
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
cp tests/data/derivedcheck/main.vish "$d/main.vish"
# Each mutant must load successfully and return exactly an oracle violation.
python3 - "$root/lib/reactive.vish" "$d" <<'PY'
import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text(); d=Path(sys.argv[2])
mutants=[
('readset','dp_valid = derived_current(dp_db,dp_r.dependencies);','dp_valid = true;'),
('working','dp_s.active = dp_i; dp_s.working = dp_i;','dp_s.active = dp_i; dp_s.working = -1;'),
('rollback','db_s.active = db_i; db_s.working = db_i;','db_s.active = db_s.active; db_s.working = db_i;'),
('generator','if((dv_r.generator == dv_name) && (dv_r.version != dv_version)) {','if(false) {'),
('metric','dp_s.stale = dp_s.stale + 1;','dp_s.stale = dp_s.stale;'),
('status','de_s.records[de_i].reason = de_reason;','de_s.records[de_i].reason = "lost";'),
]
for name,a,b in mutants:
    assert s.count(a)==1,(name,s.count(a))
    (d/(name+'.vish')).write_text(s.replace(a,b))
PY
for name in readset working rollback generator metric status; do
  cp "$d/$name.vish" "$d/reactive.vish"
  "$b" run "$d/main.vish" > "$d/log" 2>&1 || true
  if ! grep -q 'oracle return=1' "$d/log"; then cat "$d/log"; echo "$name: не поймана поведенчески"; exit 1; fi
  echo "$name: поймана"
done
