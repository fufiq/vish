#!/bin/sh
# JIT Coverage on the Test Body: Reasons why functions remain in the interpreter (vish run-jit-jit-stats). Use: tools/
# jit_coverage .sh [vish] - prints the number of failures by class. “Legal” - by design (compiling time code, OS resources,
# native primitives, construction blocks, intentionally negative tests); “flaws” - that can be closed code generation.
#
b=${1:-out/vish}
cd "$(dirname "$0")/.."
for f in tests/*.vish; do timeout 30 $b run --jit --jit-stats $f 2>&1 < /dev/null | grep -E '^  интерпретатор'; done | sed -E 's/^  интерпретатор  //' > /tmp/jit_rejects.$$
total=$(wc -l < /tmp/jit_rejects.$$)
count() { grep -cE "$1" /tmp/jit_rejects.$$; }
echo "всего отказов: $total"
echo "  разбор / ct (код времени компиляции, расширения, quote): $(count 'разбор:|ct — по месту|ct-переменная|ct-состояние|кт-параметр|узел VM читает')"
echo "  ресурсы ОС и потоки (stream/open/listen/connect/create): $(count '^(stream|open|listen|connect|create|ipc_stream|ipc_out)\(.*тип результата не скалярный')"
echo "  нативные примитивы (ast.native): $(count 'ast.native')"
echo "  блоки конструкций (аргумент блока, правила тела): $(count 'аргумент блока|правила тела для блоков')"
echo "  исключения VM (throw io_error/crypto_error): $(count 'throw значения')"
echo "  массив меняется целиком/генераторы/region/ресурс-алиас (по замыслу): $(count 'поколения|генератор|region|ресурс-алиас|потребитель')"
echo "  недоделки: .size не к переменной: $(count 'обращение .size не к переменной')"
echo "  недоделки: индекс не массива(-переменной): $(count 'индекс не массива')"
echo "  недоделки: str с локальной функцией: $(count 'вызывает локальную функцию')"
rm -f /tmp/jit_rejects.$$
