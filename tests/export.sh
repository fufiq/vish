#!/bin/sh
# Text export repository (vish repo export): catalog .vish with all pictures; vish repo import - repository again. Use:
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
# History: two snapshots, a branch, unsaved edits of the live program - everything comes back via vish repo import
$b repo module a.vishrepo extra 'e() => 1;' > /dev/null
$b repo snapshot a.vishrepo 'С extra' > /dev/null
$b repo fork a.vishrepo feat > /dev/null
$b repo snapshot a.vishrepo 'На ветке' > /dev/null
$b repo module a.vishrepo more 'm() => 2;' > /dev/null          # живая правка вне снимка
rm -rf out3; $b repo export a.vishrepo out3 > /dev/null
[ -f out3/snapshots/1/main.vish ] && [ -f out3/snapshots/3/extra.vish ] || { echo "нет снимков в экспорте"; ls -R out3; exit 1; }
$b repo import out3 c.vishrepo > /dev/null || { echo "импорт упал"; $b repo import out3 d.vishrepo; exit 1; }
$b repo info c.vishrepo | grep -q "snapshot 4 На ветке" || { echo "импорт: нет снимка 3"; $b repo info c.vishrepo; exit 1; }
$b repo info c.vishrepo | grep -q "branch 0 feat" || { echo "импорт: нет ветки feat"; $b repo info c.vishrepo; exit 1; }
$b repo info c.vishrepo | grep -q "ветка: feat" || { echo "импорт: не та текущая ветка"; $b repo info c.vishrepo; exit 1; }
$b repo info c.vishrepo | grep -q "module 0 more" || { echo "импорт: нет живой правки (модуль more)"; $b repo info c.vishrepo; exit 1; }
$b repo info c.vishrepo | grep -q "есть изменения" || { echo "импорт: живая правка не осталась несохранённой"; exit 1; }
rm -rf out4; $b repo export c.vishrepo out4 > /dev/null
# The order of use in main.vish may differ (new element stands at the end) - compare the strings as a set
for f in $(cd out3 && find . -name '*.vish' -o -name history.txt); do
  sort out3/$f > s3.tmp; sort out4/$f > s4.tmp
  cmp -s s3.tmp s4.tmp || { echo "экспорт импортированного отличается от исходного: $f"; diff s3.tmp s4.tmp | head; exit 1; }
done
