import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type {
  ClientMsg,
  RepositoryAsk,
  RepositoryEntry,
  RepositoryResult,
} from '../protocol/messages';
import { RepositoryModel, RepositoryPanel } from './repository';

const entry = (e: Partial<RepositoryEntry>): RepositoryEntry => ({
  type: 'repository_entry',
  kind: '',
  id: 0,
  parent: 0,
  node: null,
  name: '',
  detail: '',
  head: 0,
  ...e,
});
const entries = [
  entry({ kind: 'module', node: 7, name: 'geo' }),
  entry({ kind: 'snapshot', id: 1, name: 'начало' }),
  entry({ kind: 'snapshot', id: 2, parent: 1, name: 'второй' }),
  entry({ kind: 'branch', name: 'main', head: 2 }),
  entry({ kind: 'branch', name: 'dev', head: 1 }),
];
const result = (r: Partial<RepositoryResult>): RepositoryResult => ({
  type: 's_repository',
  request: 0,
  ok: true,
  revision: 5,
  path: '/p.vishrepo',
  branch: 'main',
  head: 2,
  dirty: false,
  message: '',
  diff: '',
  entries,
  ...r,
});

const element = (selector: string): HTMLElement => {
  const el = document.querySelector<HTMLElement>(selector);
  if (!el) throw new Error(`нет ${selector}`);
  return el;
};
let sent: RepositoryAsk[];
let model: RepositoryModel;
let panel: RepositoryPanel;
const last = () => sent[sent.length - 1];
const reply = (r: Partial<RepositoryResult>) => {
  model.receive(result({ request: last().request, ...r }));
};
const click = (text: string) => {
  const b = [...panel.root.querySelectorAll('button')].find(
    (x) => x.textContent === text,
  );
  if (!b) throw new Error(`нет кнопки ${text}`);
  b.click();
};
const input = (label: string) => {
  const el = panel.root.querySelector<HTMLInputElement>(
    `[aria-label="${label}"]`,
  );
  if (!el) throw new Error(`нет поля ${label}`);
  return el;
};

// Connected model with audit status 5
function ready(): void {
  model.connect();
  reply({});
  sent.length = 0;
}

beforeEach(() => {
  vi.useFakeTimers();
  sent = [];
  document.body.innerHTML = '<aside id="repo"></aside>';
  model = new RepositoryModel(
    (m: ClientMsg) => {
      if (m.type === 'c_repository') sent.push(m);
    },
    () => {
      panel.render();
    },
  );
  panel = new RepositoryPanel(element('#repo'), model, { close: vi.fn() });
});
afterEach(() => {
  vi.useRealTimers();
});

describe('репозиторий: запросы', () => {
  it('info без ревизии, остальные — с присланной VM; ответ request 0 не снимает ожидание', () => {
    model.connect();
    expect(last()).toMatchObject({ action: 'info', revision: 0 });
    expect(model.enabled).toBe(false);
    reply({ revision: 5 });
    expect(model.enabled).toBe(true);
    model.snapshot('метка');
    expect(last()).toMatchObject({
      action: 'snapshot',
      key: 'метка',
      revision: 5,
    });
    model.receive(result({ request: 0, revision: 9, branch: 'dev' }));
    expect(model.busy).toBe(true);
    expect(model.revision).toBe(9);
    expect(model.result?.branch).toBe('dev');
  });

  it('пока ждём ответ, второй запрос не отправляется; ответ с чужим номером игнорируется', () => {
    ready();
    model.snapshot('a');
    model.snapshot('b');
    model.compare('1');
    expect(sent).toHaveLength(1);
    const asked = last().request;
    model.receive(
      result({ request: asked + 7, message: 'чужой', revision: 99 }),
    );
    expect(model.busy).toBe(true);
    expect(model.revision).toBe(5);
    reply({ message: 'снимок', revision: 6 });
    expect(model.busy).toBe(false);
    model.receive(result({ request: asked, message: 'повтор', revision: 100 }));
    expect(model.revision).toBe(6);
    expect(model.message).toBe('снимок');
  });

  it('init, модуль и открытие: ключи и ревизия', () => {
    model.connect();
    reply({ path: '', entries: [] });
    expect(model.enabled).toBe(false);
    expect(model.createModule('x', '')).toBe(false);
    model.init(' /tmp/a.vishrepo ');
    expect(last()).toMatchObject({
      action: 'init',
      key: '/tmp/a.vishrepo',
      revision: 5,
    });
    reply({ revision: 6 });
    model.createModule('geo2', 'f() {}');
    expect(last()).toMatchObject({
      action: 'module',
      key: 'geo2',
      text: 'f() {}',
      revision: 6,
    });
    reply({ revision: 7 });
    click('geo');
    expect(last()).toMatchObject({ action: 'open', key: '7', revision: 7 });
  });

  it('ошибка «ревизия изменилась» берёт свежую ревизию и ничего не повторяет', () => {
    ready();
    model.snapshot('x');
    reply({
      ok: false,
      message: 'ревизия изменилась',
      revision: 8,
      path: '',
      entries: [],
    });
    expect(sent).toHaveLength(1);
    expect(model.revision).toBe(8);
    expect(model.failed).toBe(true);
    expect(model.result?.entries).toHaveLength(entries.length); // condition
  });
});

describe('сравнение и восстановление', () => {
  it('восстановить без сравнения нельзя; после сравнения — только на той же ревизии', () => {
    ready();
    expect(model.restore(1)).toBe(false);
    expect(sent).toHaveLength(0);
    click('Сравнить');
    expect(last()).toMatchObject({ action: 'diff', key: '2' }); // newest snapshot
    reply({ diff: '-a\n+b', revision: 5 });
    expect(panel.root.querySelector('.diff')?.textContent).toBe('-a\n+b');
    expect(model.restore(1)).toBe(false); // compared to picture 2
    expect(sent).toHaveLength(1);
    click('Восстановить снимок #2');
    expect(last()).toMatchObject({ action: 'restore', key: '2', revision: 5 });
    reply({ revision: 6, message: 'восстановлено' });
    expect(model.review).toBeUndefined();
  });

  it('если ревизия ушла вперёд, сравнение устарело и восстановление заблокировано', () => {
    ready();
    model.compare('2');
    reply({ diff: 'd', revision: 5 });
    model.receive(result({ request: 0, revision: 6 }));
    expect(model.reviewFresh()).toBe(false);
    expect(model.restore(2)).toBe(false);
    expect(sent).toHaveLength(1);
    expect(panel.root.textContent).toContain('Устарело');
    const b = [...panel.root.querySelectorAll('button')].find((x) =>
      x.textContent.startsWith('Восстановить'),
    );
    expect(b).toBeDefined();
    expect(b instanceof HTMLButtonElement && b.disabled).toBe(true);
  });

  it('свежее info с той же ревизией оставляет сравнение и вводимый текст', () => {
    ready();
    input('Подпись снимка').value = 'черновик подписи';
    model.compare('2');
    reply({ diff: 'd' });
    model.refresh();
    reply({});
    expect(model.review?.diff).toBe('d');
    expect(input('Подпись снимка').value).toBe('черновик подписи');
  });
});

describe('слияние', () => {
  it('запоздавшее info не откатывает новое состояние и не запускает save', () => {
    ready();
    model.save();
    const request = last().request;
    model.receive(result({ request: 0, revision: 7 }));
    model.receive(result({ request, revision: 6 }));
    expect(model.revision).toBe(7);
    expect(model.busy).toBe(false);
    expect(sent).toHaveLength(1);
    expect(model.failed).toBe(true);
  });
  it('конфликты показываются, сторона выбирается явно с точной целью', () => {
    ready();
    expect(model.merge('dev', 'ours')).toBe(false); // without viewing
    expect(sent).toHaveLength(0);
    model.checkout('dev'); // non-merger
    reply({});
    sent.length = 0;
    input('Ветка для слияния').value = 'dev';
    click('Слить в текущую');
    expect(last()).toMatchObject({ action: 'merge', key: 'dev', text: '' });
    reply({ ok: false, diff: 'конфликт: f()', message: 'есть конфликты' });
    expect(panel.root.querySelector('.clash .diff')?.textContent).toBe(
      'конфликт: f()',
    );
    const labels = [...panel.root.querySelectorAll('.clash button')].map(
      (b) => b.textContent,
    );
    expect(labels).toEqual([
      'Конфликты — моё («main»), слить «dev»',
      'Конфликты — их («dev»), слить «dev»',
    ]);
    click(labels[1]);
    expect(last()).toMatchObject({
      action: 'merge',
      key: 'dev',
      text: 'theirs',
      revision: 5,
    });
    reply({ revision: 6, message: 'слито' });
    expect(model.clash).toBeUndefined();
  });

  it('слияние без конфликтов — одно сообщение, без повтора', () => {
    ready();
    model.merge('dev', '');
    reply({ revision: 6, message: 'слито' });
    expect(sent).toHaveLength(1);
    expect(model.clash).toBeUndefined();
    expect(model.message).toBe('слито');
  });

  it('конфликты устаревают вместе с ревизией', () => {
    ready();
    model.merge('dev', '');
    reply({ ok: false, diff: 'x' });
    model.receive(result({ request: 0, revision: 6 }));
    expect(model.merge('dev', 'ours')).toBe(false);
    expect(sent).toHaveLength(1);
  });
});

describe('связь и сохранение', () => {
  it('обрыв снимает ожидание и блокирует действия до нового info', () => {
    ready();
    model.snapshot('a');
    expect(model.busy).toBe(true);
    model.disconnect();
    expect(model.busy).toBe(false);
    expect(model.snapshot('b')).toBe(false);
    expect(sent).toHaveLength(1);
    model.connect();
    expect(last()).toMatchObject({ action: 'info' });
    reply({ revision: 11 });
    expect(model.snapshot('b')).toBe(true);
    expect(last().revision).toBe(11);
  });

  it('без ответа VM состояние неизвестно, поздний ответ игнорируется', () => {
    ready();
    model.snapshot('a');
    const asked = last().request;
    vi.advanceTimersByTime(31000);
    expect(model.busy).toBe(true); // New Info awaits an answer
    expect(last()).toMatchObject({ action: 'info' });
    model.receive(result({ request: asked, revision: 50 }));
    expect(model.revision).toBe(5);
  });

  it('Ctrl+S: свежее info, затем save с присланной ревизией', () => {
    ready();
    expect(model.save()).toBe(true);
    expect(last()).toMatchObject({ action: 'info' });
    reply({ revision: 12 });
    expect(last()).toMatchObject({ action: 'save', revision: 12 });
    reply({ revision: 12, ok: false, message: 'не записано' });
    expect(model.alert).toBe(true);
  });

  it('без репозитория Ctrl+S ничего не шлёт', () => {
    model.connect();
    reply({ path: '', entries: [] });
    sent.length = 0;
    expect(model.save()).toBe(false);
    expect(sent).toHaveLength(0);
  });
});

describe('вывод данных', () => {
  it('имена и подписи — текст, не разметка', () => {
    const evil = '<img src=x onerror=alert(1)><b>';
    model.connect();
    reply({
      branch: evil,
      message: evil,
      entries: [
        entry({ kind: 'module', node: 1, name: evil }),
        entry({ kind: 'snapshot', id: 1, name: evil }),
        entry({ kind: 'branch', name: evil, head: 1 }),
      ],
    });
    model.compare('1');
    reply({ diff: evil, entries: result({}).entries });
    expect(panel.root.querySelector('img')).toBeNull();
    expect(panel.root.querySelector('b')).toBeNull();
    expect(panel.root.textContent).toContain(evil);
  });

  it('кнопки заблокированы, пока идёт запрос; Escape закрывает панель', () => {
    ready();
    const close = vi.fn();
    panel = new RepositoryPanel(panel.root, model, { close });
    panel.render();
    model.snapshot('a');
    expect(
      [...panel.root.querySelectorAll('button')]
        .filter((b) => b.textContent === 'Сравнить')
        .every((b) => b.disabled),
    ).toBe(true);
    panel.root.dispatchEvent(
      new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }),
    );
    expect(close).toHaveBeenCalled();
  });
});
