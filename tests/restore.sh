#!/bin/sh
# Close/Open: the X.vishdb database, saved later by X.vish (Ctrl+S editor), is restored with `vish serve X.vish`; the
# text is newer than the base - the text opens. Use: tests/restore.sh <vish> <catalogue >>
set -e
b=$1; d=$2
rm -rf "$d"; mkdir -p "$d"
printf 'main() {\n  print(1);\n}\n' > "$d/a.vish"
printf 'main() {\n  print(2);\n}\n' > "$d/saved.vish"
sleep 1
$b save "$d/saved.vish" "$d/a.vishdb"        # база новее текста
serve() { # сообщение VM до закрытия сокета: запустить, дождаться, погасить
  rm -f "$d/s.sock"
  timeout 5 $b serve "$1" "$d/s.sock" 2> "$d/err.txt" || true
}
serve "$d/a.vish"
grep -q "восстановлено из $d/a.vishdb" "$d/err.txt" || { echo "нет сообщения о восстановлении"; cat "$d/err.txt"; exit 1; }
sleep 1
touch "$d/a.vish"                             # текст теперь новее базы
serve "$d/a.vish"
if grep -q "восстановлено" "$d/err.txt"; then echo "восстановлено при более новом тексте"; exit 1; fi
echo ok
