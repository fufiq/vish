// Repository panel: modules, branches, pictures, comparison, merging and restoration of the semantic
// repository (.vishrepo). The condition and difs entirely at VM
// 
// - there are only requests tied to the audit, and showing the sent. The view of
// the panel is mine (author's decision).
import type { RepositoryResult, ClientMsg } from '../protocol/messages';

const WAIT_MS = 30000; // VM did not reply: condition unknown until fresh info arrives

export interface Review {
  target: string; // diff key: Picture number, branch or HEAD
  revision: number; // VM audit, on which the comparison is obtained
  diff: string;
}
export interface Clash {
  target: string;
  revision: number;
  diff: string;
}
interface Pending {
  request: number;
  action: string;
  key: string;
  save: boolean; // After fresh info - save (Ctrl+S)
}

// Model: unique requests, one action at a time, revision - only sent by VM.
export class RepositoryModel {
  connected = false;
  known = false; // After the connection came the state of VM
  result: RepositoryResult | undefined;
  message = '';
  failed = false;
  alert = false; // Ctrl+S failure: show the panel even if it is closed
  pending: Pending | undefined;
  review: Review | undefined;
  clash: Clash | undefined;
  settled: (action: string, ok: boolean) => void = () => undefined;
  #next = 0;
  #timer: ReturnType<typeof setTimeout> | undefined;

  constructor(
    private readonly sendMsg: (m: ClientMsg) => void,
    private readonly changed: () => void,
  ) {}

  get enabled(): boolean {
    return this.known && (this.result?.path ?? '') !== '';
  }
  get revision(): number {
    return this.result?.revision ?? 0;
  }
  get busy(): boolean {
    return this.pending !== undefined;
  }
  reviewFresh(): boolean {
    return this.review?.revision === this.revision;
  }
  clashFresh(): boolean {
    return this.clash?.revision === this.revision;
  }

  #fail(text: string): false {
    this.message = text;
    this.failed = true;
    this.changed();
    return false;
  }

  #ask(action: string, key: string, text: string, save = false): boolean {
    if (!this.connected) return this.#fail('Нет связи с VM');
    if (this.pending) return false; // re-sending while we wait
    if (action !== 'info' && !this.known)
      return this.#fail('Состояние репозитория неизвестно — обновите');
    const request = ++this.#next; // 0 - Not at the request of the client
    const revision = action === 'info' ? 0 : this.revision;
    this.pending = { request, action, key, save };
    this.#timer = setTimeout(() => {
      if (this.pending?.request !== request) return;
      this.pending = undefined;
      this.known = false; // Late response ignored; dangerous actions - after a new info
      this.#fail('VM не ответила на запрос репозитория');
      this.refresh();
    }, WAIT_MS);
    this.sendMsg({
      type: 'c_repository',
      request,
      action,
      key,
      text,
      revision,
    });
    this.changed();
    return true;
  }

  connect(): void {
    this.connected = true;
    this.refresh();
  }
  disconnect(): void {
    clearTimeout(this.#timer);
    this.connected = false;
    this.known = false;
    this.pending = undefined;
    this.review = undefined;
    this.clash = undefined;
    this.message =
      'Связь с VM потеряна — репозиторий обновится при подключении';
    this.failed = true;
    this.changed();
  }

  refresh(): boolean {
    return this.#ask('info', '', '');
  }
  init(path: string): boolean {
    const key = path.trim();
    return key === ''
      ? this.#fail('Укажите путь к .vishrepo')
      : this.#ask('init', key, '');
  }
  createModule(name: string, body: string): boolean {
    const key = name.trim();
    if (!this.enabled) return false;
    return key === ''
      ? this.#fail('Укажите имя модуля')
      : this.#ask('module', key, body);
  }
  openModule(node: number): boolean {
    return this.enabled && this.#ask('open', String(node), '');
  }
  snapshot(label: string): boolean {
    const key = label.trim();
    if (!this.enabled) return false;
    return key === ''
      ? this.#fail('Укажите подпись снимка')
      : this.#ask('snapshot', key, '');
  }
  compare(target: string): boolean {
    return this.enabled && target !== '' && this.#ask('diff', target, '');
  }
  // only after comparison with this image in the same VM revision
  restore(id: number): boolean {
    if (!this.enabled) return false;
    if (!this.reviewFresh() || this.review?.target !== String(id))
      return this.#fail(
        'Сначала сравните со снимком — восстановление без просмотра не отправляется',
      );
    return this.#ask('restore', String(id), '');
  }
  fork(name: string): boolean {
    const key = name.trim();
    if (!this.enabled) return false;
    return key === ''
      ? this.#fail('Укажите имя ветки')
      : this.#ask('fork', key, '');
  }
  checkout(branch: string): boolean {
    return this.enabled && branch !== '' && this.#ask('checkout', branch, '');
  }
  // parsing conflicts; 'ours' / 'theirs' - choice of party only after shown conflicts
  merge(target: string, side: '' | 'ours' | 'theirs'): boolean {
    if (!this.enabled || target === '') return false;
    if (side !== '' && (!this.clashFresh() || this.clash?.target !== target))
      return this.#fail('Сначала просмотрите конфликты слияния с этой целью');
    return this.#ask('merge', target, side);
  }
  // Ctrl+S: Fresh info, then save with a sent revision
  save(): boolean {
    if (!this.enabled) return false;
    return this.#ask('info', '', '', true);
  }

  receive(m: RepositoryResult): void {
    if (this.known && m.revision < this.revision) {
      if (m.request !== 0 && m.request === this.pending?.request) {
        clearTimeout(this.#timer);
        this.pending = undefined;
        this.#fail('Состояние изменилось — обновите репозиторий');
      }
      return;
    }
    if (m.request === 0) {
      this.#apply(m); // Not at the request of the client: status, but not waiting
      this.changed();
      return;
    }
    const p = this.pending;
    if (p?.request !== m.request) return; // obsolete or not
    clearTimeout(this.#timer);
    this.pending = undefined;
    this.#apply(m);
    this.message = m.message;
    this.failed = !m.ok;
    if (p.action === 'diff' && m.ok)
      this.review = { target: p.key, revision: m.revision, diff: m.diff };
    else if (p.action === 'merge' && !m.ok && m.diff !== '')
      this.clash = { target: p.key, revision: m.revision, diff: m.diff };
    else if (m.ok && p.action !== 'info' && p.action !== 'open') {
      this.review = undefined; // The condition has changed - the previous parsing is not about him.
      this.clash = undefined;
    }
    if (!m.ok && (p.save || p.action === 'save')) this.alert = true;
    this.settled(p.action, m.ok);
    this.changed();
    if (p.save && m.ok && this.enabled) this.#ask('save', '', '');
  }

  #apply(m: RepositoryResult): void {
    this.known = true;
    const r = this.result;
    // An error without a state does not erase the former; a new revision is a must.
    if (m.ok || m.path !== '' || m.entries.length > 0 || !r) this.result = m;
    else if (m.revision !== 0) this.result = { ...r, revision: m.revision };
  }
}

const make = <K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className = '',
  text = '',
): HTMLElementTagNameMap[K] => {
  const el = document.createElement(tag);
  if (className !== '') el.className = className;
  if (text !== '') el.textContent = text;
  return el;
};

export interface PanelHooks {
  close(): void; // hide the panel, focus in code
}

// View: The frame is assembled once, the input fields are not recreated; lists and states are updated.
export class RepositoryPanel {
  readonly #title = make('span', 'title', 'Репозиторий');
  readonly #chip = make('span', 'chip');
  readonly #msg = make('div', 'msg');
  readonly #init = make('form', 'section init');
  readonly #initPath = make('input');
  readonly #main = make('div', 'main');
  readonly #branchSel = make('select');
  readonly #forkName = make('input');
  readonly #mergeSel = make('select');
  readonly #modules = make('div', 'modules');
  readonly #modName = make('input');
  readonly #modBody = make('textarea');
  readonly #label = make('input');
  readonly #history = make('div', 'history');
  readonly #review = make('div', 'review');
  readonly #clash = make('div', 'clash');
  readonly #buttons: HTMLButtonElement[] = [];
  #dyn: HTMLButtonElement[] = []; // List buttons - recreated

  constructor(
    readonly root: HTMLElement,
    readonly model: RepositoryModel,
    hooks: PanelHooks,
  ) {
    root.setAttribute('role', 'region');
    root.setAttribute('aria-label', 'Репозиторий');
    const close = make('button', 'close', '×');
    close.type = 'button';
    close.setAttribute('aria-label', 'Закрыть репозиторий');
    close.addEventListener('click', () => {
      hooks.close();
    });
    const head = make('div', 'head');
    head.append(this.#title, this.#chip, make('span', 'fill'), close);
    this.#msg.setAttribute('role', 'status');
    this.#msg.setAttribute('aria-live', 'polite');

    this.#initPath.placeholder = '/путь/к/проекту.vishrepo';
    this.#initPath.setAttribute('aria-label', 'Путь репозитория .vishrepo');
    this.#init.append(
      make(
        'div',
        'hint',
        'Репозиторий не открыт. Он создаётся из текущей программы.',
      ),
      this.#initPath,
      this.#button('Создать репозиторий', 'submit'),
    );
    this.#init.addEventListener('submit', (e) => {
      e.preventDefault();
      model.init(this.#initPath.value);
    });

    const branch = this.#form('section', 'Ветки', () => {
      model.checkout(this.#branchSel.value);
    });
    this.#branchSel.setAttribute('aria-label', 'Ветка');
    branch.append(this.#branchSel, this.#button('Переключить', 'submit'));
    const fork = this.#form('', '', () => {
      model.fork(this.#forkName.value);
    });
    this.#forkName.placeholder = 'новая ветка';
    this.#forkName.setAttribute('aria-label', 'Имя новой ветки');
    fork.append(this.#forkName, this.#button('Ответвить', 'submit'));
    const merge = this.#form('', '', () => {
      model.merge(this.#mergeSel.value, '');
    });
    this.#mergeSel.setAttribute('aria-label', 'Ветка для слияния');
    merge.append(this.#mergeSel, this.#button('Слить в текущую', 'submit'));
    branch.append(fork, merge);

    const mods = this.#form('section', 'Модули', () => {
      model.createModule(this.#modName.value, this.#modBody.value);
    });
    this.#modName.placeholder = 'имя нового модуля';
    this.#modName.setAttribute('aria-label', 'Имя нового модуля');
    this.#modBody.placeholder = 'начальный текст (необязательно)';
    this.#modBody.setAttribute('aria-label', 'Начальный текст модуля');
    this.#modBody.rows = 2;
    mods.prepend(this.#modules);
    mods.append(
      this.#modName,
      this.#modBody,
      this.#button('Создать модуль', 'submit'),
    );

    const snap = this.#form('section', 'История', () => {
      model.snapshot(this.#label.value);
    });
    this.#label.placeholder = 'подпись снимка';
    this.#label.setAttribute('aria-label', 'Подпись снимка');
    const head2 = this.#button('Сравнить с HEAD', 'button', () => {
      model.compare('HEAD');
    });
    snap.append(
      this.#label,
      this.#button('Снимок', 'submit'),
      head2,
      this.#history,
    );

    const save = this.#button('Сохранить (Ctrl+S)', 'button', () => {
      model.save();
    });
    this.#main.append(branch, mods, snap, this.#review, this.#clash, save);
    const body = make('div', 'body');
    body.append(this.#msg, this.#init, this.#main);
    root.replaceChildren(head, body);

    root.addEventListener('keydown', (e) => {
      e.stopPropagation(); // The keys of the panel - not the editor
      const toggle =
        e.ctrlKey && e.shiftKey && (e.key === 'G' || e.key === 'П');
      if (e.key === 'Escape' || toggle) {
        e.preventDefault();
        hooks.close();
      }
    });
    model.settled = (action, ok) => {
      if (!ok) return;
      if (action === 'init') this.#initPath.value = '';
      if (action === 'module') this.#modName.value = this.#modBody.value = '';
      if (action === 'snapshot') this.#label.value = '';
      if (action === 'fork') this.#forkName.value = '';
    };
  }

  #form(cls: string, title: string, submit: () => void): HTMLFormElement {
    const f = make('form', cls);
    if (title !== '') f.append(make('h3', '', title));
    f.addEventListener('submit', (e) => {
      e.preventDefault();
      submit();
    });
    return f;
  }
  #button(
    text: string,
    type: 'button' | 'submit',
    click?: () => void,
  ): HTMLButtonElement {
    const b = make('button', '', text);
    b.type = type;
    // Click does not take the focus from the input and does not reach the code
    b.addEventListener('mousedown', (e) => {
      e.preventDefault();
    });
    if (click) b.addEventListener('click', click);
    this.#buttons.push(b);
    return b;
  }

  #options(sel: HTMLSelectElement, names: string[], keep: string): void {
    const was = sel.value;
    sel.replaceChildren(
      ...names.map((n) =>
        Object.assign(make('option'), { value: n, textContent: n }),
      ),
    );
    sel.value = names.includes(was)
      ? was
      : names.includes(keep)
        ? keep
        : (names[0] ?? '');
  }

  focus(): void {
    const first = this.model.enabled ? this.#label : this.#initPath;
    first.focus();
  }

  render(): void {
    const m = this.model;
    const r = m.result;
    const enabled = m.enabled;
    this.#chip.textContent = enabled
      ? `${r?.branch ?? ''}${r?.dirty ? ' · есть правки' : ''}`
      : m.connected
        ? 'нет репозитория'
        : 'нет связи';
    this.#chip.title = r?.path ?? '';
    this.#chip.toggleAttribute('dirty', enabled && r?.dirty === true);
    this.#msg.textContent = m.message;
    this.#msg.toggleAttribute('error', m.failed);
    this.#msg.hidden = m.message === '';
    this.root.toggleAttribute('busy', m.busy);
    this.#init.hidden = enabled || !m.known;
    this.#main.hidden = !enabled;
    for (const b of this.#buttons) b.disabled = m.busy || !m.connected;

    const entries = r?.entries ?? [];
    const branches = entries.filter((e) => e.kind === 'branch');
    this.#options(
      this.#branchSel,
      branches.map((e) => e.name),
      r?.branch ?? '',
    );
    this.#options(
      this.#mergeSel,
      branches.map((e) => e.name).filter((n) => n !== r?.branch),
      '',
    );
    for (const b of this.#dyn) b.remove();
    this.#dyn = [];

    const mods = entries.filter((e) => e.kind === 'module' && e.node !== null);
    this.#modules.replaceChildren(
      ...(mods.length === 0
        ? [make('div', 'empty', 'модулей нет')]
        : mods.map((e) => {
            const b = make('button', 'module', e.name);
            b.type = 'button';
            b.title = `Открыть модуль ${e.name}`;
            b.disabled = m.busy;
            b.addEventListener('mousedown', (ev) => {
              ev.preventDefault();
            });
            b.addEventListener('click', () => {
              if (e.node !== null) m.openModule(e.node);
            });
            this.#dyn.push(b);
            return b;
          })),
    );

    const snaps = entries
      .filter((e) => e.kind === 'snapshot')
      .sort((a, b) => b.id - a.id);
    this.#history.replaceChildren(
      ...(snaps.length === 0
        ? [make('div', 'empty', 'снимков нет')]
        : snaps.map((e) => {
            const row = make('div', 'snap');
            const heads = branches
              .filter((x) => x.head === e.id)
              .map((x) => x.name);
            const parents = [e.parent, e.head]
              .filter((p) => p > 0)
              .map((p) => `#${String(p)}`);
            row.append(
              make('span', 'id', `#${String(e.id)}`),
              make('span', 'name', e.name),
              make(
                'span',
                'from',
                parents.length ? `← ${parents.join(' + ')}` : '',
              ),
              ...heads.map((h) => make('span', 'tag', h)),
            );
            const b = make('button', '', 'Сравнить');
            b.type = 'button';
            b.disabled = m.busy;
            b.setAttribute(
              'aria-label',
              `Сравнить рабочую версию со снимком ${String(e.id)} ${e.name}`,
            );
            b.addEventListener('mousedown', (ev) => {
              ev.preventDefault();
            });
            b.addEventListener('click', () => {
              m.compare(String(e.id));
            });
            this.#dyn.push(b);
            row.append(b);
            return row;
          })),
    );

    this.#paintReview(snaps.map((e) => [String(e.id), e.name] as const));
    this.#paintClash(r?.branch ?? '');
  }

  #paintReview(names: readonly (readonly [string, string])[]): void {
    const m = this.model;
    const rv = m.review;
    this.#review.hidden = !rv;
    if (!rv) return;
    const label = names.find(([id]) => id === rv.target)?.[1];
    const snapshot = label !== undefined;
    const fresh = m.reviewFresh();
    const head = make(
      'div',
      'rhead',
      snapshot
        ? `Рабочая версия и снимок #${rv.target} «${label}»`
        : `Рабочая версия и ${rv.target}`,
    );
    const pre = make(
      'pre',
      'diff',
      rv.diff === '' ? '(различий нет)' : rv.diff,
    );
    pre.tabIndex = 0;
    pre.setAttribute('aria-label', 'Различия');
    const nodes: HTMLElement[] = [head, pre];
    if (!fresh)
      nodes.push(
        make(
          'div',
          'stale',
          'Устарело: программа изменилась после сравнения — сравните заново.',
        ),
      );
    if (snapshot) {
      nodes.push(
        make(
          'div',
          'warn',
          'Восстановление отбросит текущее содержимое рабочей версии.',
        ),
      );
      const b = make('button', 'danger', `Восстановить снимок #${rv.target}`);
      b.type = 'button';
      b.disabled = m.busy || !fresh;
      b.addEventListener('mousedown', (ev) => {
        ev.preventDefault();
      });
      b.addEventListener('click', () => {
        m.restore(Number(rv.target));
      });
      this.#dyn.push(b);
      nodes.push(b);
    }
    const hide = make('button', '', 'Скрыть сравнение');
    hide.type = 'button';
    hide.addEventListener('mousedown', (ev) => {
      ev.preventDefault();
    });
    hide.addEventListener('click', () => {
      m.review = undefined;
      this.render();
    });
    this.#dyn.push(hide);
    nodes.push(hide);
    this.#review.replaceChildren(...nodes);
  }

  #paintClash(branch: string): void {
    const m = this.model;
    const c = m.clash;
    this.#clash.hidden = !c;
    if (!c) return;
    const fresh = m.clashFresh();
    const pre = make('pre', 'diff', c.diff);
    pre.tabIndex = 0;
    pre.setAttribute('aria-label', 'Конфликты слияния');
    const nodes: HTMLElement[] = [
      make('div', 'rhead', `Конфликты слияния с «${c.target}»`),
      pre,
      make(
        'div',
        'warn',
        'Выберите сторону для конфликтующих мест; остальные изменения сольются в любом случае.',
      ),
    ];
    if (!fresh)
      nodes.push(
        make('div', 'stale', 'Устарело: слияние нужно разобрать заново.'),
      );
    for (const [side, text] of [
      ['ours', `Конфликты — моё («${branch}»), слить «${c.target}»`],
      ['theirs', `Конфликты — их («${c.target}»), слить «${c.target}»`],
    ] as const) {
      const b = make('button', side, text);
      b.type = 'button';
      b.disabled = m.busy || !fresh;
      b.addEventListener('mousedown', (ev) => {
        ev.preventDefault();
      });
      b.addEventListener('click', () => {
        m.merge(c.target, side);
      });
      this.#dyn.push(b);
      nodes.push(b);
    }
    this.#clash.replaceChildren(...nodes);
  }
}
