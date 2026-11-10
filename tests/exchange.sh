#!/bin/sh
# Exchange between repositories (vish repo pull|push): images with new numbers, source/branch branches, only
# forward, journal divergence - failure. Use: tests/exchange.sh <vish> <catalogue >>
set -e
b=$1; d=$2
rm -rf "$d"; mkdir -p "$d"; cd "$d"
printf 'main() {\n  print(1);\n}\n' > p.vish
$b repo init p.vish a.vishrepo > /dev/null
cp a.vishrepo b.vishrepo                      # независимая копия: общий префикс журнала
$b repo module b.vishrepo math 'twice(i64 x) => x + x;' > /dev/null
$b repo snapshot b.vishrepo 'Математика' > /dev/null
out=$($b repo pull a.vishrepo b.vishrepo)
echo "$out" | grep -q "снимков 1" || { echo "pull: $out"; exit 1; }
$b repo info a.vishrepo | grep -q "branch 0 b/main" || { echo "нет ветки b/main"; exit 1; }
$b repo info a.vishrepo | grep -q "snapshot 2 Математика" || { echo "нет снимка"; exit 1; }
out=$($b repo pull a.vishrepo b.vishrepo)     # повтор ничего не добавляет
echo "$out" | grep -q "операций журнала 0, снимков 0" || { echo "повтор: $out"; exit 1; }
$b repo checkout a.vishrepo b/main > /dev/null # свои правки на отслеживаемой ветке
$b repo module a.vishrepo mine 'm() => 1;' > /dev/null
$b repo snapshot a.vishrepo 'Моё' > /dev/null
$b repo snapshot b.vishrepo 'Ещё у них' > /dev/null
out=$($b repo pull a.vishrepo b.vishrepo)
echo "$out" | grep -q "не сдвинуты.*b/main" || { echo "ветка со своими снимками сдвинута: $out"; exit 1; }
cp a.vishrepo c.vishrepo                      # журналы расходятся: каждый дописал своё
$b repo module a.vishrepo left 'l() => 1;' > /dev/null
$b repo module c.vishrepo right 'r() => 2;' > /dev/null
$b repo snapshot a.vishrepo 'A-сн' > /dev/null
$b repo snapshot c.vishrepo 'C-сн' > /dev/null
out=$($b repo pull a.vishrepo c.vishrepo)     # журналы разошлись — слияние по модулям: чужой модуль приходит
echo "$out" | grep -q "новый модуль right" || { echo "слияние: $out"; exit 1; }
$b repo info a.vishrepo | grep -q "module 0 right" || { echo "нет модуля right"; exit 1; }
$b repo info a.vishrepo | grep -q "Слияние: " || { echo "нет снимка слияния"; exit 1; }
$b repo push a.vishrepo b.vishrepo > /dev/null  # отдать: b — префикс a
$b repo info b.vishrepo | grep -q "branch 0 a/main" || { echo "push не дал ветку a/main"; exit 1; }
echo ok
