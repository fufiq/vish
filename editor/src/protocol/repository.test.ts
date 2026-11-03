// @vitest-environment node Real container, individual CLI processes and internal
// AST invariants.
import { execFileSync, spawn, spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';

const root = join(import.meta.dirname, '../../..');
const vish = join(root, 'out/vish');
const dirs: string[] = [];
function workspace() {
  const dir = mkdtempSync(join(tmpdir(), 'vish-repository-'));
  dirs.push(dir);
  return { dir, path: join(dir, 'program.vishrepo') };
}
function cli(...args: string[]) {
  return execFileSync(vish, args, { encoding: 'utf8', timeout: 15000 });
}
function edit(path: string, fn: string, source: string) {
  const output = execFileSync(vish, ['vm', path], {
    encoding: 'utf8',
    timeout: 15000,
    input: `edit ${fn}\n${source}\n.\nsave ${path}\nquit\n`,
  });
  expect(output).toContain('сохранено:');
  return output;
}
afterEach(() => {
  for (const dir of dirs.splice(0))
    rmSync(dir, { recursive: true, force: true });
});

describe('локальный семантический репозиторий', () => {
  it('создаёт модули без файлов и исполняет их после перезапуска', () => {
    const { path } = workspace();
    cli('repo', 'new', path);
    cli('repo', 'module', path, 'math', 'twice(i64 x) => x + x;');
    cli('repo', 'module', path, 'app', 'main() => print(math.twice(4));');
    const info = cli('repo', 'info', path);
    expect(info).toContain('main');
    expect(info).toContain('math');
    expect(info).toContain('app');
    // Import of the source - single; further implementation does not depend on it.
    const imported = workspace();
    const seed = join(imported.dir, 'seed.vish');
    writeFileSync(seed, 'main() => print(1);\n');
    cli('repo', 'init', seed, imported.path);
    rmSync(seed);
    cli('repo', 'module', imported.path, 'math', 'twice(i64 x) => x + x;');
    edit(imported.path, 'main', 'main() => print(math.twice(4));');
    expect(cli('run', imported.path)).toBe('8\n');
    expect(cli('repo', 'diff', imported.path)).toContain('math');
  }, 60000);

  it('различает переименование той же функции и сохраняет исходник отдельно', () => {
    const { dir, path } = workspace();
    const seed = join(dir, 'seed.vish');
    const source =
      'f(x) => x + 1;\nmain() => print(f(2));\nrename() => code_rename(code(f), "renamed");\n';
    writeFileSync(seed, source);
    cli('repo', 'init', seed, path);
    cli('apply', path, 'rename', path);
    expect(cli('run', path)).toBe('3\n');
    const diff = cli('repo', 'diff', path);
    expect(diff).toContain('переименование');
    expect(diff).toContain('renamed');
    expect(readFileSync(seed, 'utf8')).toBe(source);
    cli('repo', 'restore', path, '1');
    expect(cli('fmt', path)).toContain('f(x)');
    expect(cli('run', path)).toBe('3\n');
  }, 60000);

  it('ветки, dirty-защита, конфликт, явное решение и восстановление всей программы', () => {
    const { dir, path } = workspace();
    const seed = join(dir, 'seed.vish');
    writeFileSync(seed, 'f(i64 x) => x + 1;\nmain() => print(f(2));\n');
    cli('repo', 'init', seed, path);
    cli('repo', 'fork', path, 'dev');
    edit(path, 'f', 'f(i64 x) => x + 3;');
    cli('repo', 'module', path, 'extra', 'answer() => 42;');
    cli('repo', 'snapshot', path, 'Работа dev');
    cli('repo', 'checkout', path, 'main');
    expect(cli('run', path)).toBe('3\n');
    edit(path, 'f', 'f(i64 x) => x + 2;');
    const before = readFileSync(path);
    const dirty = spawnSync(vish, ['repo', 'checkout', path, 'dev'], {
      encoding: 'utf8',
    });
    expect(dirty.status).toBe(1);
    expect(dirty.stderr).toContain('изменения');
    expect(readFileSync(path)).toEqual(before);
    cli('repo', 'snapshot', path, 'Работа main');
    const clean = readFileSync(path);
    const clash = spawnSync(vish, ['repo', 'merge', path, 'dev'], {
      encoding: 'utf8',
    });
    expect(clash.status).toBe(1);
    expect(clash.stderr).toContain('конфликт:');
    expect(readFileSync(path)).toEqual(clean);
    cli('repo', 'merge', path, 'dev', 'ours');
    expect(cli('run', path)).toBe('4\n');
    expect(cli('repo', 'info', path)).toContain('extra');
    cli('repo', 'restore', path, '1');
    expect(cli('run', path)).toBe('3\n');
    expect(cli('repo', 'info', path)).not.toContain('module 0 extra');
    cli('repo', 'checkout', path, 'dev');
    expect(cli('run', path)).toBe('5\n');
    expect(cli('repo', 'info', path)).toContain('extra');
  }, 120000);

  it('имя модуля cli не даёт программе возможности хоста репозитория', () => {
    const { dir, path } = workspace();
    const program = join(dir, 'fake-host.vish');
    writeFileSync(
      program,
      `module cli; use tools; main() { r = repository_storage(0, "init", "${path}", "", 0, 0); print(r[0], r[1]); }`,
    );
    const output = execFileSync(vish, ['vm', program], {
      encoding: 'utf8',
      input: 'run\nquit\n',
      timeout: 15000,
    });
    expect(output).toContain('false');
    expect(output).toContain('репозиторий доступен только хосту');
  }, 60000);

  it('конфликт одинаковых имён новых модулей решается явно на Vish', () => {
    const { path } = workspace();
    cli('repo', 'new', path);
    cli('repo', 'fork', path, 'dev');
    cli('repo', 'module', path, 'shared', 'answer() => 1;');
    cli('repo', 'snapshot', path, 'dev');
    cli('repo', 'checkout', path, 'main');
    cli('repo', 'module', path, 'shared', 'answer() => 2;');
    cli('repo', 'snapshot', path, 'main');
    const before = readFileSync(path);
    const clash = spawnSync(vish, ['repo', 'merge', path, 'dev'], {
      encoding: 'utf8',
    });
    expect(clash.status).toBe(1);
    expect(clash.stderr).toContain('имя модуля');
    expect(readFileSync(path)).toEqual(before);
    cli('repo', 'merge', path, 'dev', 'theirs');
    const info = cli('repo', 'info', path);
    expect(info.match(/module 0 shared/g)).toHaveLength(1);
  }, 60000);

  it('проверяет восстановление ссылок, повреждение и конкурирующую запись в C++', async () => {
    const { dir, path } = workspace();
    const binary = join(dir, 'storage-test');
    execFileSync(
      '/usr/bin/g++-16',
      [
        '-std=c++26',
        '-freflection',
        '-fcontracts',
        '-O0',
        '-g1',
        '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer',
        '-I' + join(root, 'include'),
        join(root, 'tests/repository_storage.cpp'),
        '-luring',
        '-o',
        binary,
      ],
      { timeout: 300000, stdio: 'pipe' },
    );
    expect(
      execFileSync(binary, [path], { encoding: 'utf8', timeout: 20000 }),
    ).toContain('concurrent save: OK');
    const invalid = spawnSync(
      vish,
      ['repo', 'info', path + '.badmeta.vishrepo'],
      {
        encoding: 'utf8',
      },
    );
    expect(invalid.status).toBe(1);
    expect(invalid.stderr).toContain('повреждён список веток');
    const writers = [
      spawn(binary, [path, 'race']),
      spawn(binary, [path, 'race']),
    ];
    try {
      const ended = writers.map(
        (child) =>
          new Promise<number | null>((resolve, reject) => {
            child.once('exit', resolve);
            child.once('error', reject);
          }),
      );
      await Promise.all(
        writers.map(
          (child) =>
            new Promise<void>((resolve, reject) => {
              child.stdout.once('data', (data: Buffer) => {
                if (data.toString().includes('ready')) resolve();
                else reject(new Error('нет барьера писателя'));
              });
              child.once('error', reject);
            }),
        ),
      );
      for (const child of writers) child.stdin.end('\n');
      expect((await Promise.all(ended)).sort()).toEqual([0, 2]);
    } finally {
      for (const child of writers) child.kill();
    }
  }, 330000);
});
