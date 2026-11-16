#!/bin/sh
# A mutation check of the reactcheck oracle (RA-2 M1): an error is introduced into a copy of lib/reactive.vish,
# the oracle must not return 0 (violations of the random part/of the fixed cases or an abnormal termination). The module is next to the program.
#  a: skipping the read-set for key "0";  b: no transitive push invalidation;
#  c: ignoring a repeated definition;   d: losing the dependency on a failed read;
#  e: distorting the hit counter (hits +7) - caught by the oracle's statistics check (accesses == computations + hits + refusals);
#  f: losing the removal of old users edges when the read-set changes - caught by the structural audit;
#  g: exceptions are compared by the top-level typeof + text (regression H1 of the third cycle: `throw array{1}` -> `throw array{"1"}`) -
#     caught by the generator's 22 exception kinds and the fixed pairs;
#  h: returning a "subscription cycle" refusal on delivery (regression B2) - caught by the fixed cases;
#  The third review cycle (the reviewer's mutations 9-12 and two new ones):
#  i: set_max_depth changes nothing - caught by the fixed depth cases;
#  j: on a matching result the cutoff is not counted (counts[2] does not grow) - caught by the exact independent cutoff count;
#  k: the early cutoff of an array result is lost (like a comparison by reference) - caught by the exact cutoff count and the array cutoff;
#  l: resetting delivery on leaving EVERY (including a nested) batch - caught by the exact notification count and the fixed nested batches;
#  m: deep_equal compares nested elements by text() - caught by nested arrays/tuples/structs with one text;
#  n: deep_equal does not compare the fields of a struct - caught by pairs of struct exceptions.
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
  # caught if the oracle did not return 0: a non-zero code or an abnormal termination (an exception past the oracle)
  case "$out" in *"oracle return=0"*) echo "мутация $1 НЕ ПОЙМАНА"; echo "$out" | tail -2; exit 1;; esac
  case "$out" in *"oracle return=1"*) how=$(echo "$out" | grep 'шагов=' | sed 's/.*случайных_нарушений=/случайных_нарушений=/');; *) how="аварийное завершение: $(echo "$out" | grep -v '^reactcheck: случай' | tail -1)";; esac
  echo "мутация $1 поймана: $how"
}
mutate a '  if(nt_db.top == 0) return;' '  if((nt_db.top == 0) || (nt_db.keys[nt_i] == "0")) return;'
mutate b 'rx_db.marks[rx_u] = rx_ep; rx_todo.push(rx_u); rx_db.dirty[rx_u] = true;' 'rx_db.marks[rx_u] = rx_ep; rx_db.dirty[rx_u] = true;'
mutate c 'qd_db.checked[qd_i] = -1; rx_dirty' 'rx_dirty'
mutate d 'catch(gt_error) { rx_note(gt_db,gt_i,gt_db.revs[gt_i]); throw gt_error; }' 'catch(gt_error) { throw gt_error; }'
mutate e 're_db.counts[1] = re_db.counts[1] + 1; return rx_result(re_db,re_i);' 're_db.counts[1] = re_db.counts[1] + 7; return rx_result(re_db,re_i);'
mutate f 'for(rl_d : rl_old) if(!rx_reads(rl_new, rl_d[0])) rx_unuse(rl_db, rl_d[0], rl_i);' 'for(rl_d : rl_old) if(false) rx_unuse(rl_db, rl_d[0], rl_i);'
mutate g 'rx_same_exc(se_a, se_b) => deep_equal(se_a,se_b);' 'rx_same_exc(se_a, se_b) => (typeof(se_a) == typeof(se_b)) && (text(se_a) == text(se_b));'
mutate h 'rf_db.aux[rf_best] = 0; rf_ready.push(rf_best);' 'throw "reactive: subscription cycle";'
mutate i 'set_max_depth(mut sd_db, i64 sd_n) { sd_db.maxdepth = sd_n; }' 'set_max_depth(mut sd_db, i64 sd_n) { }'
mutate j 'if(re_same) re_db.counts[2] = re_db.counts[2] + 1;' 'if(re_same) re_db.counts[2] = re_db.counts[2];'
mutate k 'if(re_db.vals[re_i].size > 0) re_same = re_eq(re_db.vals[re_i][0],re_value);' 'if(re_db.vals[re_i].size > 0) re_same = re_eq(re_db.vals[re_i][0],re_value) && (typeof(re_value) != array);'
mutate l 'bt_db.depth = bt_db.depth - 1; if(bt_db.depth == 0) rx_flush(bt_db);' 'bt_db.depth = bt_db.depth - 1; rx_flush(bt_db);'
mutate m 'for(de_i : range(de_a.size)) if(!deep_equal(de_a[de_i],de_b[de_i])) return false;' 'for(de_i : range(de_a.size)) if(text(de_a[de_i]) != text(de_b[de_i])) return false;'
mutate n 'for(de_f : ct fields(de_t)) if(!deep_equal(de_a.[de_f],de_b.[de_f])) return false;' 'for(de_f : ct fields(de_t)) if(false) return false;'
