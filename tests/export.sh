#!/bin/sh
# Text export repository (vish repo export): the .vish directory from which the repository is created anew. Use:
# tests/export.sh <vish> <catalogue >>
set -e
b=$1; d=$2
rm -rf "$d"; mkdir -p "$d"; cd "$d"
printf 'main() {\n  print(twice(21));\n}\n' > p.vish
$b repo init p.vish a.vishrepo > /dev/null
$b repo module a.vishrepo math 'twice(i64 x) => x + x;' > /dev/null
$b repo module a.vishrepo geo.vec 'sq(i64 x) => x * x;' > /dev/null
$b repo snapshot a.vishrepo 'Модули' > /dev/null
$b repo export a.vishrepo out > /dev/null
[ -f out/main.vish ] && [ -f out/math.vish ] && [ -f out/geo/vec.vish ] || { echo "нет файлов экспорта"; ls -R out; exit 1; }
grep -q "^use math;" out/main.vish || { echo "в main.vish нет use math"; exit 1; }
grep -q "^module geo.vec;" out/geo/vec.vish || { echo "нет заголовка модуля"; exit 1; }
grep -q "Модули" out/history.txt || { echo "нет истории"; exit 1; }
cp -r out out2; $b repo export a.vishrepo out2 > /dev/null
diff -r out out2 > /dev/null || { echo "экспорт недетерминирован"; exit 1; }
$b repo init out/main.vish b.vishrepo > /dev/null
$b repo info b.vishrepo | grep -q "module 0 math" || { echo "импорт: нет модуля math"; exit 1; }
$b repo info b.vishrepo | grep -q "module 0 geo.vec" || { echo "импорт: нет модуля geo.vec"; exit 1; }
$b run b.vishrepo 2>&1 | grep -q "^42" || { echo "импортированная программа не работает"; $b run b.vishrepo; exit 1; }
