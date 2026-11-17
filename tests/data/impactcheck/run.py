#!/usr/bin/env python3
"""RA-3: запуск на сохранённой копии бинаря, без сборки. Моё."""
import pathlib
import random
import re
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())
mutant = '--mutant' in sys.argv[2:]
with tempfile.TemporaryDirectory(prefix='vish-ra3-') as directory:
    base = pathlib.Path(directory)
    for cmd in ('modcheck', 'fmtcheck', 'nodecheck'):
        expr = cmd == 'nodecheck'
        suffix = '.txt' if expr else '.vish'
        first, second = base / ('a' + suffix), base / ('b' + suffix)
        original = '1\n2\n' if expr else 'ra3_a() => 1;\n'
        first.write_text(original)
        second.write_text('41\n' if expr else 'ra3_b() => 41;\n')
        # A body, a signature, an insertion, a deletion, a rename, a scope rule.
        versions = (['3\n2\n', '3\n2\n4\n', '3\n', '5\n'] if expr else [
            'ra3_a() => 2;\n',
            'ra3_a(i64 x) => x;\n',
            'ra3_a(i64 x) => x;\nra3_new() => 3;\n',
            'ra3_new() => 3;\n',
            'ra3_renamed() => 3;\n',
            'ct { }\nra3_renamed() => 3;\n'])
        argv = [binary, cmd, '--impact', str(first), str(second)]
        for i, text in enumerate(versions):
            snapshot = base / f'{cmd}-{i}.snapshot'
            snapshot.write_text(text)
            argv += ['--delta', '0', str(snapshot)]
        argv += ['--rollback']
        # A random deterministic sequence, a repeated rollback.
        rng = random.Random(1703)
        for i in range(12):
            n = rng.randrange(1, 100)
            snapshot = base / f'{cmd}-random-{i}.snapshot'
            snapshot.write_text(f'{n}\n' if expr else f'ra3_a() => {n};\n')
            argv += ['--delta', '0', str(snapshot)]
        argv += ['--rollback']
        result = subprocess.run(argv, text=True, capture_output=True)
        print(cmd, 'exit', result.returncode)
        print(result.stdout)
        print(result.stderr)
        if mutant:
            assert result.returncode != 0, 'мутация пропуска impact не поймана'
            assert 'кэш != полный эталон' in result.stdout, 'мутация должна дать расхождение результатов'
            continue
        assert result.returncode == 0
        rows = re.findall(r'выполнено файлов (\d+), повторно использовано (\d+), всего (\d+), расхождения (\d+)', result.stdout)
        assert len(rows) == len(versions) + 15, rows
        assert rows[0] == ('2', '0', '2', '0'), rows[0]
        assert rows[1] == ('2', '0', '2', '0'), rows[1]  # the regular closure through implicit hooks
        assert all(int(a) + int(b) == int(c) and int(d) == 0 for a, b, c, d in rows)
        if not expr:
            assert rows[len(versions)] == ('2', '0', '2', '0'), 'ct должен широко инвалидировать'
        assert rows[len(versions) + 1] == ('2', '0', '2', '0'), 'rollback'
        # Independent expectations of the full oracles of the source snapshot.
        legacy = subprocess.run([binary, cmd, str(first)], capture_output=True, text=True)
        assert legacy.returncode == 0
        expected = ('совпало 2, пропущено 0, расхождений 0' if expr else
                    'совпало 1, расхождений 0')
        assert expected in legacy.stdout, legacy.stdout
# A non-trivial unchanged data file contains no functions: the regular impact
# allows reuse here. This is a separate file-level case, not a speedup inside a file.
    if not mutant:
        data = base / 'data.vish'
        data.write_text(''.join(f'{n};\n' for n in range(41, 105)))
        first = base / 'reuse.vish'
        first.write_text('ra3_a() => 1;\n')
        delta = base / 'reuse.snapshot'
        delta.write_text('ra3_a() => 2;\nra3_new() => 3;\n')
        for cmd in ('modcheck', 'fmtcheck'):
            result = subprocess.run([binary, cmd, '--impact', str(first), str(data), '--delta', '0', str(delta), '--rollback'], text=True, capture_output=True)
            print('reuse', cmd, 'exit', result.returncode)
            print(result.stdout)
            print(result.stderr)
            assert result.returncode == 0
            rows = re.findall(r'выполнено файлов (\d+), повторно использовано (\d+), всего (\d+), расхождения (\d+)', result.stdout)
            assert rows == [('2','0','2','0'), ('1','1','2','0'), ('2','0','2','0')], rows
print('RA-3: PASS')
