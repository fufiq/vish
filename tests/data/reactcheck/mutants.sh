#!/bin/sh
# A mutation check of the reactcheck oracle (RA-2 M1): an error is introduced into a copy of lib/reactive.vish,
# the oracle must return a non-zero code (violations in the random part). The module is next to the program.
#  a: skipping the read-set for key "0";  b: no transitive push invalidation;
#  c: ignoring a repeated definition;   d: losing the dependency on a failed read.
set -eu
b=$(realpath "${1:-out/vish}")
root=$(pwd)
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
cat > "$d/main.vish" <<'EOM'
use reactive;
main() { r = react_check_cli(array{"1","300"}); print("oracle return=" + text(r)); }
EOM
mutate() { # name, the source fragment (a sed regex), the replacement
  n=$(grep -c -F -- "$2" "$root/lib/reactive.vish" || true)
  [ "$n" = 1 ] || { echo "мутация $1: фрагмент найден $n раз"; exit 1; }
  awk -v from="$2" -v to="$3" 'BEGIN{} { i=index($0,from); if(i>0) $0=substr($0,1,i-1) to substr($0,i+length(from)); print }' "$root/lib/reactive.vish" > "$d/reactive.vish"
  out=$("$b" run "$d/main.vish" 2>&1) || true
  case "$out" in *"oracle return=1"*) ;; *) echo "мутация $1 НЕ ПОЙМАНА"; echo "$out" | tail -2; exit 1;; esac
  echo "мутация $1 поймана: $(echo "$out" | grep 'шагов=' | sed 's/.*случайных_нарушений=/случайных_нарушений=/')"
}
mutate a '  if(nt_db.top == 0) return;' '  if((nt_db.top == 0) || (nt_db.keys[nt_i] == "0")) return;'
mutate b 'rx_db.marks[rx_u] = rx_ep; rx_todo.push(rx_u); rx_db.dirty[rx_u] = true;' 'rx_db.marks[rx_u] = rx_ep; rx_db.dirty[rx_u] = true;'
mutate c 'qd_db.checked[qd_i] = -1; rx_dirty' 'rx_dirty'
mutate d 'catch(gt_error) { rx_note(gt_db,gt_i,-1,text(gt_error)); throw gt_error; }' 'catch(gt_error) { throw gt_error; }'
