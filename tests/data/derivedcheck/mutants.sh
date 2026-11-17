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
('rollback','db_s.active = db_i;','db_s.active = db_s.active;'),
('generator','if((dv_r.generator == dv_name) && (dv_r.version != dv_version) && (dv_r.status != "отвергнут")) {','if(false) {'),
('metric','dp_s.stale = dp_s.stale + 1;','dp_s.stale = dp_s.stale;'),
('status','de_s.records[de_i].reason = de_reason;','de_s.records[de_i].reason = "lost";'),
('noeval','for(dc_d : dc_reads) { try { rx_eval(dc_db,dc_d[0]); } catch(dc_e) { } }',''),
('onlyfirst','for(dc_d : dc_reads) if(dc_db.revs[dc_d[0]] != dc_d[1]) return false;','if(dc_db.revs[dc_reads[0][0]] != dc_reads[0][1]) return false;'),
('rbgen','for(db_g : db_s.generators) if(db_g[0] == db_r.generator) if(db_g[1] != db_r.version) return false;',''),
('pubgen','for(dp_g : dp_s.generators) if(dp_g[0] == dp_r.generator) if(dp_g[1] != dp_r.version) dp_valid = false;',''),
('zerocmp','if((dp_c[2] <= 0) || (dp_c[3] != 0))','if(dp_c[3] != 0)'),
('othergen','(dv_r.generator == dv_name) && ','true && '),

]
for name,a,b in mutants:
    assert s.count(a)==1,(name,s.count(a))
    (d/(name+'.vish')).write_text(s.replace(a,b))
PY
for name in readset working rollback generator metric status noeval onlyfirst rbgen pubgen zerocmp othergen; do
  cp "$d/$name.vish" "$d/reactive.vish"
  "$b" run "$d/main.vish" > "$d/log" 2>&1 || true
  if ! grep -q 'oracle return=1' "$d/log"; then cat "$d/log"; echo "$name: не поймана поведенчески"; exit 1; fi
  echo "$name: поймана"
done
