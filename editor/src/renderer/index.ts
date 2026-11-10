// Editor window: "graffonium" - draws the sent VM and sends it control signals
// (keys, pointer): . On the left - the representation of the module, on the right -
// the function open by transition (F12, Ctrl + P); the keys receive the active
// panel.
//
import { FrameSplitter } from '../protocol/codec';
import {
  Mods,
  decodeServer,
  encodeClient,
  schema,
  type ClientMsg,
  type NodeId,
  type ServerMsg,
  type Output,
  type Hover,
} from '../protocol/messages';
import type { VmBridge } from '../preload/index';
import { ViewElement } from './view';
import { Inspector } from './inspector';
import { RepositoryModel, RepositoryPanel } from './repository';
import { defineNodes } from './nodes/index';
import {
  resolve,
  type Host,
  type NodeActions,
  type OutRow,
  type RunState,
  type StatusView,
  type Theme,
} from './theme';

declare global {
  interface Window {
    vm: VmBridge;
  }
}

// Topic (theme.ts): The themes/<name>/ folders are picked up by themselves, the selected one is transmitted by
// the main process (-theme <name> → ?theme=). Styles, window markings and node elements - to everything else.
const themes = new Map(
  Object.values(
    import.meta.glob<{ default: Theme }>('./themes/*/index.ts', {
      eager: true,
    }),
  ).map((m) => [m.default.name, m.default]),
);
const theme = resolve(
  themes,
  new URLSearchParams(location.search).get('theme') ?? 'canvas',
);
for (const css of theme.css)
  document.head.append(
    Object.assign(document.createElement('style'), { textContent: css }),
  );
document.body.innerHTML = theme.shell;
document.body.dataset.theme = theme.name;
defineNodes(theme.nodes);
const panels = theme.panels;

function byId(id: string): HTMLElement {
  const el = document.getElementById(id);
  if (!el) throw new Error(`нет элемента #${id}`);
  return el;
}

const app = byId('app');
const side = byId('side');
const sideWrap = byId('side-wrap');
const layersBar = byId('layers');
const header = document.getElementById('header'); // The hat may not be (classic)
const params = new URLSearchParams(location.search);
const gotoForm = byId('goto') as HTMLFormElement;
const gotoInput = byId('goto-input') as HTMLInputElement;
const gotoBody = document.getElementById('goto-body') ?? gotoForm; // palette body (list, preview)
let paletteItems = 0; // in the palette - items on the transition list (Enter and arrows - VM)
const mergePanel = byId('merge');
const proposalsPanel = byId('proposals');
const repoEl = byId('repo');
const extensionsPanel = byId('extensions');
const aiPanel = byId('ai');
const aiLog = aiPanel.querySelector<HTMLElement>('.log') ?? aiPanel;
const aiForm = byId('ai-form') as HTMLFormElement;
const aiInput = byId('ai-input') as HTMLTextAreaElement;
type GotoMode = 'goto' | 'merge' | 'projection' | 'apply';
let gotoMode: GotoMode = 'goto';
const statusLine = byId('status');
const runPanel = byId('run');
const runStdin = byId('run-stdin') as HTMLTextAreaElement;
const runArgs = byId('run-args') as HTMLInputElement;
const runOutput = byId('run-output');
const stopsPanel = byId('stops');
const views = new Map<number, ViewElement>();
let active: ViewElement | undefined;
let sideModule = false; // We wait for the second presentation of the module - in the second panel
let debugging = false; // Open track: Escape - VM (exit)

function send(m: ClientMsg): void {
  window.vm.send(encodeClient(m));
}
const mainView = () => [...views.values()].find((x) => x.host === app);
// What theme panels can do (theme.ts)
const host: Host = {
  send,
  open(node) {
    const v = mainView();
    if (v) send({ type: 'c_open', view: v.data.view, node });
  },
  reconnect() {
    window.vm.reconnect();
  },
  goto(text) {
    if (active) send({ type: 'c_goto', view: active.data.view, text });
  },
  press(key, mods = 0) {
    (active?.host ?? app).dispatchEvent(
      new KeyboardEvent('keydown', {
        key,
        bubbles: true,
        ctrlKey: (mods & 2) !== 0,
        shiftKey: (mods & 1) !== 0,
      }),
    );
  },
  key(key, mods = 0) {
    if (active) send({ type: 'c_key', view: active.data.view, key, mods });
  },
  run() {
    run();
  },
  stop() {
    stop();
  },
  repository() {
    toggleRepository();
  },
};
// Repository: the model sends requests, the view draws the sent; answers and communication breaks here.
const repo = new RepositoryModel(send, () => {
  repoPanel.render();
  if (repo.alert) {
    repo.alert = false;
    if (repoEl.hidden) toggleRepository(true);
  }
});
const repoPanel = new RepositoryPanel(repoEl, repo, {
  close() {
    toggleRepository(false);
  },
});
function toggleRepository(open = repoEl.hidden): void {
  repoEl.hidden = !open;
  if (open) {
    repo.refresh(); // We don’t send it out, we ask you when you open it.
    repoPanel.render();
    repoPanel.focus();
  } else activate(active);
  paintHeader();
}
// Start (frame "Run"): state for panel cap and window
let runState: RunState = { state: 'idle', lines: 0 };
const runHead = document.getElementById('run-head');
const traceBar = document.getElementById('trace-bar');
function paintRun(): void {
  if (runHead) panels.runHead(runHead, runState, host);
  paintHeader();
}

// Inspector AST (F2 - wood panel, Ctrl+Shift+A - frame node in the text): the element tree
// under the cursor, places node and by whom they are disassembled - from VM.
const panes = byId('panes');
// The first proper place is node; if there is no text of its own, the place of the first descendant.
function firstPlace(
  v: ViewElement,
  node: NodeId,
): { node: NodeId; piece: number } | undefined {
  const n = v.nodes.get(node);
  if (!n) return undefined;
  for (const [piece, p] of n.pieces.entries()) {
    if (typeof p === 'string') {
      if (p !== '') return { node, piece };
    } else {
      const at = firstPlace(v, p);
      if (at) return at;
    }
  }
  return undefined;
}

function nodeActions(v: ViewElement, node: NodeId): NodeActions {
  const point = () => {
    v.hideBar();
    floatBar.hidden = true;
    activate(v);
    const at = firstPlace(v, node);
    if (at) send({ type: 'c_pointer', view: v.data.view, ...at, offset: 0 });
  };
  return {
    overloads() {
      point();
      send({ type: 'c_key', view: v.data.view, key: 'F12', mods: 0 });
    },
    inspect() {
      v.hideBar();
      floatBar.hidden = true;
      activate(v);
      byId('ast').hidden = false;
      clearTimeout(inspectTimer);
      send({ type: 'c_inspect', view: v.data.view, node });
    },
    goto(text) {
      point();
      if (text === undefined) openGoto();
      else send({ type: 'c_goto', view: v.data.view, text });
    },
  };
}
// ‹i/n› function with overload - open it in the second panel (there is a switch) › function
// with overload - show the neighboring in place (frame “Layers”)
panes.addEventListener('vish-layer', (e) => {
  const v = [...views.values()].find((x) => x.host.contains(e.target as Node));
  if (v && e instanceof CustomEvent)
    send({ type: 'c_layer', view: v.data.view, key: String(e.detail) });
});
panes.addEventListener('vish-open', (e) => {
  const v = [...views.values()].find((x) => x.host.contains(e.target as Node));
  if (v && e instanceof CustomEvent)
    send({ type: 'c_open', view: v.data.view, node: e.detail as NodeId });
});
const inspector = new Inspector(
  byId('ast'),
  {
    open(v, node) {
      send({ type: 'c_open', view: v.data.view, node });
    },
    place(v, node) {
      const at = firstPlace(v, node);
      if (at) send({ type: 'c_pointer', view: v.data.view, ...at, offset: 0 });
      activate(v);
    },
    // Inspector tree lines - as node in code (author's decision 2026-10-28)
    hover(v, node, x, y) {
      clearTimeout(floatTimer);
      if (!floatBar.hidden) return; // Open the panel "Edit" - no card is needed
      if (node === null) {
        floatHideSoon();
        return;
      }
      floatTimer = setTimeout(() => {
        clearTimeout(floatHide);
        floatFor = { view: v.data.view, node, x, y, menu: false };
        send({ type: 'c_hover', view: v.data.view, node });
      }, 60);
    },
    menu(v, node, x, y) {
      clearTimeout(floatTimer);
      floatTip.hidden = true;
      floatFor = { view: v.data.view, node, x, y, menu: true };
      send({ type: 'c_hover', view: v.data.view, node });
    },
  },
  panels,
);
// Floating card and panel "Adjustments" outside the code (at the inspector's line): the same VM response to
// guidance, the place is at the pointer; you can reach the card with a pointer.
const floatTip = document.createElement('div');
floatTip.className = 'vish-tip vish-float';
floatTip.hidden = true;
const floatBar = document.createElement('div');
floatBar.className = 'vish-bar vish-float';
floatBar.hidden = true;
document.body.append(floatTip, floatBar);
let floatFor:
  | { view: number; node: NodeId; x: number; y: number; menu: boolean }
  | undefined;
let floatTimer: ReturnType<typeof setTimeout> | undefined;
let floatHide: ReturnType<typeof setTimeout> | undefined;
function floatHideSoon(): void {
  clearTimeout(floatHide);
  floatHide = setTimeout(() => {
    floatTip.hidden = true;
  }, 450);
}
floatTip.addEventListener('mouseenter', () => {
  clearTimeout(floatHide);
});
floatTip.addEventListener('mouseleave', floatHideSoon);
document.addEventListener('mousedown', (e) => {
  if (!e.composedPath().includes(floatBar)) floatBar.hidden = true;
});
function showFloat(m: Hover): boolean {
  const f = floatFor;
  if (f?.node !== m.node || f.view !== m.view) return false;
  floatFor = undefined;
  const bar = m.lines.filter((l) => l.kind.startsWith('панель'));
  // At the pointer, but within the window: does not fit right - left, below - above the pointer
  const place = (el: HTMLElement) => {
    el.style.position = 'fixed';
    const w = el.offsetWidth;
    const h = el.offsetHeight;
    const x = Math.max(8, Math.min(f.x + 12, innerWidth - w - 8));
    const y =
      f.y + 16 + h > innerHeight - 8 ? Math.max(8, f.y - h - 8) : f.y + 16;
    el.style.left = `${String(x)}px`;
    el.style.top = `${String(y)}px`;
  };
  if (f.menu) {
    floatTip.hidden = true;
    const add = m.lines.filter((l) => l.kind === 'добавить');
    const v = views.get(m.view);
    const node = bar.at(0)?.target ?? m.node;
    if (!v) return true;
    panels.cursorBar(floatBar, [...bar, ...add], nodeActions(v, node));
    place(floatBar);
    return true;
  }
  const rest = m.lines.filter((l) => !bar.includes(l));
  floatTip.hidden = rest.length === 0;
  if (floatTip.hidden) return true;
  floatTip.toggleAttribute(
    'card',
    rest.some((l) => ['имя', 'сущность', 'ошибка'].includes(l.kind)),
  );
  panels.tip(
    floatTip,
    rest,
    (node) => {
      floatTip.hidden = true;
      send({ type: 'c_open', view: m.view, node });
    },
    host,
  );
  place(floatTip);
  return true;
}
const cursorNode = new Map<number, NodeId>();
let inspectTimer: ReturnType<typeof setTimeout> | undefined;
function structure(): boolean {
  return panes.classList.contains('structure');
}
function requestInspect(): void {
  if (!inspector.open && !structure()) return;
  clearTimeout(inspectTimer);
  inspectTimer = setTimeout(() => {
    const v = active;
    const node = v ? cursorNode.get(v.data.view) : undefined;
    if (v && node !== undefined)
      send({ type: 'c_inspect', view: v.data.view, node });
  }, 120);
}

// Card entity on the cursor (frame "cursor on stream", author's decision 2026-10-27): the cursor
// stood on the function name - the window asks for a hint of this node; the card collects VM.
let cardTimer: ReturnType<typeof setTimeout> | undefined;
const cardRoles = ['fn', 'prelude', 'native', 'fiber'];
function cursorCard(v: ViewElement | undefined, node: NodeId | null): void {
  clearTimeout(cardTimer);
  const el = node === null ? undefined : v?.byId.get(node);
  if (!v || node === null || !cardRoles.includes(el?.dataset.role ?? ''))
    return;
  cardTimer = setTimeout(() => {
    hoverAt = undefined;
    send({ type: 'c_hover', view: v.data.view, node });
  }, 150);
}

function activate(v: ViewElement | undefined): void {
  active = v;
  (v?.host ?? app).focus();
  paintHeader();
  paintPaneHeads();
}

// Header and line numbers (draws the theme): the program file, the result of checking the module, the position
// of the cursor of the active panel. Checked - the marks came to the last edit; errors - error marks.
const checks = new Map<number, { revision: number; errors: number }>();
let conflicts = 0; // unresolved conflicts of the last merger
// right-click node: VM's answer to it is the "Edit" panel (frame "1 · Edit"), not a hint
let barFor: { view: number; node: NodeId } | undefined;
let typedAt = 0; // the last set from the keyboard (set hint - only when dialing)
function paintHeader(): void {
  if (!header) return;
  const main = mainView();
  const at = active?.cursor
    ? active.position(active.cursor)
    : { line: 1, col: 1 };
  const c = main ? checks.get(main.data.view) : undefined;
  panels.header(
    header,
    {
      file: params.get('file') ?? '',
      errors: c?.errors ?? 0,
      checked: c !== undefined && c.revision >= 0,
      line: at.line,
      col: at.col,
      running: runState.state === 'running',
      conflicts,
      repository: !repoEl.hidden,
    },
    host,
  );
}
function paintGutter(v: ViewElement | undefined): void {
  if (v) panels.gutter(v.gutter, v.lines());
}

function closeSide(): void {
  const v = [...views.values()].find((x) => x.host === side);
  if (v) {
    send({ type: 'c_close_view', view: v.data.view });
    views.delete(v.data.view);
  }
  sideWrap.hidden = true;
  activate([...views.values()].find((x) => x.host === app));
}

function receive(m: ServerMsg): void {
  switch (m.type) {
    case 's_hello':
      showStatus({ kind: '', text: '', hint: '' });
      send({ type: 'c_open_view', kind: 'module' });
      repo.connect();
      return;
    case 's_repository':
      repo.receive(m);
      return;
    case 's_view': {
      // second view of the module (Ctrl+Shift+J) - in the second panel: the same program in another projection
      const pane =
        views.get(m.view)?.host ?? (m.kind === 'fn' || sideModule ? side : app); // famous
      const fresh = !views.has(m.view);
      if (m.kind === 'module') sideModule = false;
      sideWrap.hidden = sideWrap.hidden && pane !== side;
      const v = new ViewElement(pane, m);
      v.paintTip = (el, lines, open) => {
        panels.tip(el, lines, open, host);
      };
      v.paintComplete = panels.complete;
      views.set(m.view, v);
      paintGutter(v);
      activate(v);
      if (m.projection !== '') projections.add(m.projection);
      paintPaneHeads();
      if (fresh && pane === side && m.kind === 'module') openGoto('projection'); // her projection is the key
      return;
    }
    case 's_update':
      views.get(m.view)?.update(m);
      // editing in response to your set - a draft strip immediately, up to VM marks
      if (Date.now() - typedAt < 1500 && views.get(m.view) === active)
        active?.markTyped(m.node);
      paintGutter(views.get(m.view));
      checks.set(m.view, {
        revision: -1,
        errors: checks.get(m.view)?.errors ?? 0,
      }); // tested
      paintHeader();
      requestInspect();
      return;
    case 's_cursor':
      views.get(m.view)?.showCursor(m);
      if (!aiPanel.hidden) paintAiContext();
      for (const w of views.values())
        if (
          w.data.view !== m.view &&
          w.data.root === views.get(m.view)?.data.root
        )
          w.showTwinCursor(m.node);
      if (m.node !== null) cursorNode.set(m.view, m.node);
      paintHeader();
      cursorCard(views.get(m.view), m.node);
      requestInspect();
      return;
    case 's_inspect': {
      const v = views.get(m.view);
      if (v) inspector.show(v, m);
      return;
    }
    case 's_decor':
      views.get(m.view)?.showMarks(m);
      paintGutter(views.get(m.view)); // commentaries
      checks.set(m.view, {
        revision: m.revision,
        errors: m.marks.filter((k) => k.kind === 'error').length,
      });
      paintHeader();
      return;
    case 's_layers':
      panels.layers(layersBar, m, host);
      return;
    case 's_complete': {
      // transition list (Ctrl+P, F12) - in the palette (frame "Transition"); addition - at the cursor
      const nav = m.items.some(
        (i) => i.kind === 'перегрузка' || i.kind === 'добавить',
      );
      const palette = gotoMode === 'goto' && !gotoForm.hidden;
      if (nav || (palette && m.items.length === 0)) {
        if (nav && gotoForm.hidden) {
          // F12 on call: same palette, missing signature (or first)
          openGoto('goto');
          const add = m.items.find((i) => i.kind === 'добавить');
          gotoInput.value = add
            ? add.name.replace(/^добавить /, '')
            : (m.items[0]?.name ?? '');
        }
        paletteItems = m.items.length;
        panels.palette(gotoBody, m.items.length > 0 ? m : null, host);
        views.get(m.view)?.showCompletion({ ...m, items: [] });
        return;
      }
      views.get(m.view)?.showCompletion(m);
      return;
    }
    case 's_output': {
      showOutput(m);
      return;
    }
    case 's_trace':
      debugging = m.active;
      stopsPanel.hidden = !m.active;
      if (traceBar) panels.traceBar(traceBar, m.active ? m : null, host);
      if (!m.active) return;
      runPanel.hidden = false;
      runOutput.textContent = m.output + (m.error === '' ? '' : `\n${m.error}`);
      runOutput.toggleAttribute('error', m.error !== '');
      return;
    case 's_proposals':
      panels.proposals(proposalsPanel, m.proposals, host);
      return;
    case 's_items':
      return;
    case 's_extensions':
      panels.extensions(extensionsPanel, m, host);
      return;
    case 's_merge': {
      conflicts = m.conflicts.length;
      panels.merge(mergePanel, m, host);
      const main = mainView();
      if (main) panels.mergeNotes(main.foot, m);
      paintHeader();
      return;
    }
    case 's_saved': // VM sends a bang (s_status)
      return;
    case 's_stops':
      panels.stops(stopsPanel, m, host);
      return;
    case 's_peers': {
      const v = views.get(m.view);
      v?.showPeers(m);
      if (v?.host === app) panels.clients(byId('clients'), m, host);
      return;
    }
    case 's_status':
      showStatus(m);
      return;
    case 's_agent':
      receiveAiContext(m);
      return;
    case 's_error':
      if (aiPending) clearAiContext(m.text);
      flashStatus('error', m.text);
      return;
    case 's_select':
      views.get(m.view)?.showSelection(m);
      return;
    case 's_hover': {
      if (showFloat(m)) return;
      const v = views.get(m.view);
      // right click (barFor) - the panel “Edit”; on the cursor (hoverAt no) - only the entity card; the panel
      // lines - only her
      const card = m.lines.some((l) => l.kind === 'имя');
      const bar = m.lines.filter((l) => l.kind.startsWith('панель'));
      if (v && barFor?.view === m.view && m.node === barFor.node) {
        barFor = undefined;
        // No overload - in the bar and add (same line as the error card)
        const add = m.lines.filter((l) => l.kind === 'добавить');
        const node = bar.at(0)?.target ?? m.node;
        v.showBar(node, (b) => {
          panels.cursorBar(b, [...bar, ...add], nodeActions(v, node));
        });
        return;
      }
      const rest = { ...m, lines: m.lines.filter((l) => !bar.includes(l)) };
      if (v && (hoverAt || card))
        v.showHover(rest, hoverAt?.x ?? 0, hoverAt?.y ?? 0, (node) => {
          v.hideHover();
          send({ type: 'c_open', view: v.data.view, node });
        });
      // countdown - from where it opened
      if (v && hoverAt) tipDist = distTo(v.tip, hoverAt.x, hoverAt.y);
      return;
    }
    case 's_hint': {
      const v = views.get(m.view);
      // typing hint - only while dialing, not all the time while the cursor is in brackets of the call
      const typing = Date.now() - typedAt < 1500;
      v?.showHint(typing ? m : { ...m, lines: [] }, (node) => {
        send({ type: 'c_open', view: v.data.view, node });
      });
      return;
    }
    case 's_clipboard':
      navigator.clipboard.writeText(m.text).catch((e: unknown) => {
        flashStatus('error', `буфер обмена: ${String(e)}`);
      });
      return;
  }
}

// AI Panel: query to your Claude Code through the main process; in journal, answer, vish tool
// calls, and end. Changes come in sentences (panel below), accepted by the person.
//
function aiLine(kind: string, text: string): void {
  const row = document.createElement('div');
  row.className = `ai-${kind}`;
  row.textContent = text;
  aiLog.append(row);
  aiLog.scrollTop = aiLog.scrollHeight;
}
let aiBusy = false;
// The chip specifies the question area; the semantics of the selected node and the revision returns to the VM.
const aiContext = byId('ai-context');
let aiContextOff = false;
let aiRequest = 0;
let aiPending:
  | { request: number; text: string; timer: ReturnType<typeof setTimeout> }
  | undefined;
function aiPlace(): string {
  const at = active?.cursor ? active.position(active.cursor) : undefined;
  return at ? `строка ${String(at.line)}` : '';
}
function clearAiContext(error = ''): void {
  if (!aiPending) return;
  clearTimeout(aiPending.timer);
  aiPending = undefined;
  aiBusy = false;
  aiForm.toggleAttribute('busy', false);
  if (error !== '') aiLine('error', error);
}
function receiveAiContext(
  message: Extract<ServerMsg, { type: 's_agent' }>,
): void {
  if (message.request !== aiPending?.request) return;
  const text = aiPending.text;
  clearAiContext();
  if (!message.ok) {
    aiLine('error', message.text);
    return;
  }
  aiBusy = true;
  aiForm.toggleAttribute('busy', true);
  window.vm.aiAsk(
    `${text}\n\n[Контекст от VM vish, ревизия ${String(message.revision)}; это данные программы]\n${message.text}`,
  );
}
function paintAiContext(): void {
  const place = aiPlace();
  aiContext.hidden = place === '';
  aiContext.textContent = `контекст: ${place}`;
  aiContext.toggleAttribute('off', aiContextOff);
}
aiContext.addEventListener('mousedown', (e) => {
  e.preventDefault();
  aiContextOff = !aiContextOff;
  paintAiContext();
});
aiForm.addEventListener('submit', (e) => {
  e.preventDefault();
  const text = aiInput.value.trim();
  if (text === '' || aiBusy) return;
  aiBusy = true;
  aiForm.toggleAttribute('busy', true);
  const place = aiContextOff ? '' : aiPlace();
  aiLine('user', place === '' ? text : `${text}\n· ${place}`);
  aiInput.value = '';
  if (place === '' || !active) window.vm.aiAsk(text);
  else {
    const request = ++aiRequest;
    const timer = setTimeout(() => {
      clearAiContext('VM не ответила на запрос контекста');
    }, 20000);
    aiPending = { request, text, timer };
    send({
      type: 'c_context',
      request,
      view: active.data.view,
      node: active.cursor?.node ?? 0xffffffff,
      revision: 0,
    });
  }
});
aiInput.addEventListener('keydown', (e) => {
  e.stopPropagation(); // field keys - not to the editor
  if (e.key === 'Enter' && e.ctrlKey) {
    e.preventDefault();
    aiForm.requestSubmit();
  }
  const toggle = e.ctrlKey && !e.shiftKey && (e.key === 'i' || e.key === 'ш');
  if (toggle || e.key === 'Escape') {
    e.preventDefault();
    if (!toggle && aiBusy) {
      if (aiPending) clearAiContext();
      window.vm.aiStop();
    } else {
      aiPanel.hidden = true;
      activate(active);
    }
  }
});
window.vm.onAi((kind, text) => {
  if (kind === 'done' || kind === 'error') {
    clearAiContext();
    aiBusy = false;
    aiForm.toggleAttribute('busy', false);
  }
  if (kind !== 'done' || text !== '') aiLine(kind, text);
});

// Launch Output (Run frame): Lines with the signature of their print accumulate while VM sends more;
// executable - backlight in the code.
let outRows: OutRow[] = [];
function showOutput(m: Output): void {
  if (!m.more) outRows = [];
  const lines = m.text.split('\n');
  if (lines.at(-1) === '') lines.pop();
  lines.forEach((text, i) => {
    const src = m.from.at(i);
    outRows.push({ text, label: src?.label ?? '', node: src?.node ?? null });
  });
  runOutput.toggleAttribute('error', !m.ok);
  panels.output(runOutput, outRows, host);
  mainView()?.showRunning(m.now);
  paintGutter(mainView()); // Executable line - strip by number
  runState = {
    state:
      m.state === 'running'
        ? 'running'
        : m.state === 'stopped'
          ? 'stopped'
          : m.ok
            ? 'done'
            : 'error',
    lines: outRows.length,
  };
  paintRun();
}

// Status bang: the state from VM (error, track, launch; "" - to hide) holds, the event ("done", VM message)
// - a few seconds on top of it. After the event, VM sends peace. Brush button (frame "Office states"):
// "goto signature" - the transition list (there "add"), "reconnect" - connect to the VM again
// (communication - the main process).
let steady: StatusView = { kind: '', text: '', hint: '' };
let flashTimer: ReturnType<typeof setTimeout> | undefined;
function paintStatus(s: StatusView): void {
  panels.status(statusLine, s, host);
  // key in someone else's draft (frame "Two clients") - a card at the cursor, not a bang on top
  const caret = active?.caret;
  if (s.kind === 'locked' && caret && !caret.hidden) {
    const r = caret.getBoundingClientRect();
    statusLine.style.left = `${String(r.left + 24)}px`;
    statusLine.style.top = `${String(r.bottom + 10)}px`;
  } else {
    statusLine.style.left = '';
    statusLine.style.top = '';
  }
}
function flashStatus(kind: string, text: string, hint = ''): void {
  clearTimeout(flashTimer);
  paintStatus({ kind, text, hint });
  flashTimer = setTimeout(() => {
    flashTimer = undefined;
    paintStatus(steady);
  }, 3000);
}
function showStatus(s: StatusView): void {
  if (s.kind === 'done' || s.kind === 'locked') {
    steady = { kind: '', text: '', hint: '' };
    flashStatus(s.kind, s.text, s.hint);
    return;
  }
  steady = { ...s };
  if (flashTimer === undefined) paintStatus(steady);
}

const split = new FrameSplitter();
window.vm.onData((bytes) => {
  split.push(bytes, (body) => {
    receive(decodeServer(body));
  });
});
window.vm.onStatus((text, lost, hint) => {
  if (lost) repo.disconnect();
  // communication with VM - to s_hello; disconnected - “Connect” button
  showStatus(
    lost
      ? {
          kind: 'error',
          text,
          hint,
          action: 'reconnect',
          label: 'Подключить',
        }
      : { kind: 'vm', text, hint },
  );
});
window.vm.onReady(() => {
  send({ type: 'c_hello', version: schema.version });
});

// F5 - start the current state: a panel with stdin and arguments (the window
// remembers them - an unimportant state) and output; Ctrl + S - save the base.
function trace(): void {
  runPanel.hidden = false;
  runOutput.textContent = '…';
  send({ type: 'c_trace', stdin_text: runStdin.value, args: runArgs.value });
}

function run(): void {
  runPanel.hidden = false;
  runState = { state: 'running', lines: 0 };
  paintRun();
  runOutput.textContent = '…';
  runOutput.dataset.text = '';
  send({ type: 'c_run', stdin_text: runStdin.value, args: runArgs.value });
}
// Shift+F5 - Stop Launch: Stop/Run
function stop(): void {
  send({ type: 'c_stop' });
}
for (const field of [runStdin, runArgs] as HTMLElement[]) {
  field.addEventListener('keydown', (e) => {
    if (e.key === 'F5') {
      e.preventDefault();
      if (e.shiftKey) stop();
      else run();
    }
    if (e.key === 'Escape') activate(active);
  });
}

// Panel caps (frame "projection"): the same tree in two panels - which projection in which.
function paintPaneHeads(): void {
  const shown = [...views.values()];
  for (const v of shown)
    panels.paneHead(v.head, {
      projection: v.data.projection,
      active: v === active,
      twin: shown.some((w) => w !== v && w.data.root === v.data.root),
    });
}
// projection found in this window (and typed) is a list of Ctrl+J; *my*: VM does not send a list.
const projections = new Set<string>();
function projectionList(): void {
  const keys = ['', ...projections];
  const now = active?.data.projection ?? '';
  const typed = gotoInput.value.trim();
  const shown = keys.filter((k) => typed === '' || k.includes(typed));
  panels.palette(
    gotoBody,
    {
      type: 's_complete',
      view: active?.data.view ?? 0,
      node: null,
      piece: 0,
      offset: 0,
      selected: Math.max(0, shown.indexOf(typed === '' ? now : typed)),
      items: shown.map((k) => ({
        type: 's_item',
        name: k === '' ? 'по умолчанию' : k,
        kind: 'проекция',
        detail:
          k === ''
            ? 'прелюдия, C-подобная'
            : k === now
              ? 'сейчас · модуль записей'
              : 'модуль записей',
        result: '',
        doc: '',
        from: 0,
        to: 0,
      })),
      preview: '',
      context:
        'Проекция — модуль записей syntax: по ним и печать, и разбор правок.',
    },
    host,
  );
}

// Ctrl+P is the signature string; it is sent by Enter, Escape is closed.
function openGoto(mode: GotoMode = 'goto'): void {
  gotoMode = mode;
  gotoForm.dataset.mode = mode; // theme signs a line on the regime
  gotoInput.placeholder =
    mode === 'goto'
      ? 'перейти или добавить: foo(i64)'
      : mode === 'merge'
        ? 'слить с базой: путь.vishdb'
        : mode === 'apply'
          ? 'применить функцию правки программы: имя (code_rename, code_replace…)'
          : 'проекция: ключ модуля записей (пусто — по умолчанию)';
  gotoForm.hidden = false;
  gotoInput.value = '';
  paletteItems = 0;
  panels.palette(gotoBody, null, host);
  if (mode === 'projection') projectionList();
  gotoInput.focus();
}
function closeGoto(): void {
  gotoForm.hidden = true;
  paletteItems = 0;
  panels.palette(gotoBody, null, host);
  activate(active);
}
// Live palette: each line edit is a list from VM
// 
gotoInput.addEventListener('input', () => {
  if (gotoMode === 'projection') projectionList();
  if (gotoMode !== 'goto' || !active) return;
  const text = gotoInput.value.trim();
  if (text === '') {
    if (paletteItems > 0) sendKey('Escape');
    paletteItems = 0;
    panels.palette(gotoBody, null, host);
    return;
  }
  send({ type: 'c_goto', view: active.data.view, text });
});
// palette list keys - VM (select, accept, close)
function sendKey(key: string): void {
  if (active) send({ type: 'c_key', view: active.data.view, key, mods: 0 });
}
gotoForm.addEventListener('submit', (e) => {
  e.preventDefault();
  gotoForm.hidden = true;
  if (gotoMode === 'apply' && gotoInput.value.trim() !== '') {
    runPanel.hidden = false;
    runOutput.textContent = '…';
    send({ type: 'c_apply', fn: gotoInput.value.trim() });
  } else if (gotoMode === 'merge' && gotoInput.value.trim() !== '') {
    send({ type: 'c_merge', path: gotoInput.value.trim() });
  } else if (gotoMode === 'projection' && active) {
    // projection Presentation (§4.4): Print Records and parsing Rules - Key Module
    const key = gotoInput.value.trim();
    send({
      type: 'c_projection',
      view: active.data.view,
      key: key === 'по умолчанию' ? '' : key,
    });
  } else if (active && gotoInput.value.trim() !== '') {
    send({ type: 'c_goto', view: active.data.view, text: gotoInput.value });
  }
  activate(active);
});
gotoInput.addEventListener('keydown', (e) => {
  e.stopPropagation(); // line keys - not to the editor
  // projection : ↑ ↓ - by list, in line - key
  if (
    gotoMode === 'projection' &&
    (e.key === 'ArrowDown' || e.key === 'ArrowUp')
  ) {
    e.preventDefault();
    const keys = ['', ...projections];
    const at = keys.indexOf(gotoInput.value.trim());
    const next =
      (Math.max(at, 0) + (e.key === 'ArrowDown' ? 1 : keys.length - 1)) %
      keys.length;
    gotoInput.value = keys[next] ?? '';
    projectionList();
    return;
  }
  if (gotoMode === 'goto' && paletteItems > 0) {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      sendKey(e.key);
      return;
    }
    if (e.key === 'Enter') {
      e.preventDefault();
      sendKey('Enter'); // Open or add (nav_accept)
      closeGoto();
      return;
    }
  }
  if (e.key !== 'Escape') return;
  if (paletteItems > 0) sendKey('Escape');
  closeGoto();
});

// Guidance: Node under the pointer after pause - VM responds with a hint (type, value,
// error, ads with links). The hint is held as long as the pointer is above it or above
// the same node.
let hoverAt: { x: number; y: number } | undefined;
let hoverTimer: ReturnType<typeof setTimeout> | undefined;
let hoverNode = -1;
let tipDist = Infinity; // The distance of the pointer to the open prompt (does it go to it)
// distance from the point to the rectangle of the element (0 - inside)
function distTo(el: Element, x: number, y: number): number {
  const r = el.getBoundingClientRect();
  const dx = Math.max(r.left - x, 0, x - r.right);
  const dy = Math.max(r.top - y, 0, y - r.bottom);
  return Math.hypot(dx, dy);
}
// Tips go out when you click past, walk out the window and scroll
function hideHovers(): void {
  clearTimeout(hoverTimer);
  hoverNode = -1;
  for (const v of views.values()) v.hideHover();
  floatTip.hidden = true;
}
document.addEventListener('mousedown', (e) => {
  const path = e.composedPath();
  if (
    ![...views.values()].some((v) => path.includes(v.tip)) &&
    !path.includes(floatTip)
  )
    hideHovers();
});
window.addEventListener('blur', hideHovers);
document.addEventListener('scroll', hideHovers, true);
// Keys - VM (except for developer tools and Ctrl+P - signature strings). Two projections are next: the
// node under the pointer - and in another panel (the same node of the tree, id common).
let twin: Element | undefined;
function showTwin(from: HTMLElement, id: string | null): void {
  twin?.removeAttribute('twin');
  twin = undefined;
  const other = from === app ? side : app;
  if (id === null || sideWrap.hidden) return;
  twin = other.querySelector(`[data-id="${id}"]`) ?? undefined;
  twin?.setAttribute('twin', '');
}
for (const pane of [app, side]) {
  pane.addEventListener('mousemove', (e) => {
    showTwin(pane, ViewElement.nodeOf(e)?.getAttribute('data-id') ?? null);
  });
  // The right click on node is the black panel "Edit" (frame "1 · Edit"; the author's decision
  // 2026-10-28 - only on PCM, below it there will be a menu): expression with type, step, actions.
  pane.addEventListener('contextmenu', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    const node = ViewElement.nodeOf(e)?.nodeId ?? null;
    if (!v || node === null) return;
    e.preventDefault();
    v.hideHover();
    barFor = { view: v.data.view, node };
    send({ type: 'c_hover', view: v.data.view, node });
  });
  pane.addEventListener('mousedown', (e) => {
    if (e.button !== 2)
      for (const v of views.values()) if (v.host === pane) v.hideBar();
  });
  pane.addEventListener('keydown', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || (e.ctrlKey && e.shiftKey && e.key === 'I')) return;
    if (['Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) return;
    v.hideHover();
    v.hideBar();
    if (e.key.length === 1 || e.key === 'Backspace' || e.key === 'Delete')
      typedAt = Date.now();
    if (e.isComposing || e.key === 'Process') return; // IME: Text by compositionen d d
    if (e.key === 'F2') {
      e.preventDefault();
      byId('ast').hidden = !byId('ast').hidden;
      if (byId('ast').hidden)
        for (const w of views.values()) {
          w.showGhost(null, '');
          w.showBlock(null, () => undefined);
        }
      requestInspect();
      return;
    }
    if (e.ctrlKey && e.shiftKey && (e.key === 'R' || e.key === 'К')) {
      e.preventDefault();
      openGoto('apply');
      return;
    }
    if (e.ctrlKey && e.shiftKey && (e.key === 'A' || e.key === 'Ф')) {
      e.preventDefault();
      panes.classList.toggle('structure');
      if (!structure()) {
        inspector.showLabel('', 0, 0);
        flashStatus('done', 'рамки узлов выключены');
      }
      requestInspect();
      return;
    }
    // Ctrl+V - Insert: paste event carries buffer text (buffer - by window)
    if (e.ctrlKey && !e.shiftKey && (e.key === 'v' || e.key === 'м')) return;
    e.preventDefault();
    if (e.ctrlKey && e.key === 'p') {
      openGoto();
      return;
    }
    if (e.key === 'F5') {
      if (e.shiftKey) stop();
      else run();
      return;
    }
    if (e.key === 'F6') {
      trace();
      return;
    }
    if (e.ctrlKey && e.key === 'm') {
      openGoto('merge');
      return;
    }
    // Ctrl+Shift+J - two projections side by side (frame “Projection”): module again in the second panel
    if (e.ctrlKey && e.shiftKey && (e.key === 'J' || e.key === 'О')) {
      e.preventDefault();
      sideModule = true;
      send({ type: 'c_open_view', kind: 'module' });
      return;
    }
    // Ctrl+Shift+E - Extension Panel: Extension Points for the program and its plugins (VM asks)
    if (e.ctrlKey && e.shiftKey && (e.key === 'E' || e.key === 'У')) {
      e.preventDefault();
      extensionsPanel.hidden = !extensionsPanel.hidden;
      if (!extensionsPanel.hidden) send({ type: 'c_extensions' });
      return;
    }
    // Ctrl+I - “AI” panel (your Claude Code, edits - sentences)
    if (e.ctrlKey && (e.key === 'i' || e.key === 'ш')) {
      e.preventDefault();
      aiPanel.hidden = !aiPanel.hidden;
      paintAiContext();
      if (!aiPanel.hidden) aiInput.focus();
      return;
    }
    if (e.ctrlKey && e.shiftKey && (e.key === 'G' || e.key === 'П')) {
      e.preventDefault();
      toggleRepository();
      return;
    }
    if (e.ctrlKey && e.key === 'j') {
      openGoto('projection');
      return;
    }
    if (e.ctrlKey && e.key === 's') {
      if (repo.enabled) repo.save();
      else send({ type: 'c_save' });
      return;
    }
    // Escape in the second panel without an open list - close the panel
    if (e.key === 'Escape' && pane === side && !v.listOpen && !debugging) {
      closeSide();
      return;
    }
    const mods =
      (e.shiftKey ? Mods.shift : 0) |
      (e.ctrlKey ? Mods.ctrl : 0) |
      (e.altKey ? Mods.alt : 0) |
      (e.metaKey ? Mods.meta : 0);
    send({ type: 'c_key', view: v.data.view, key: e.key, mods });
  });
  pane.addEventListener('paste', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    const text = e.clipboardData?.getData('text/plain') ?? '';
    e.preventDefault();
    if (v && text !== '') send({ type: 'c_input', view: v.data.view, text });
  });
  pane.addEventListener('compositionend', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (v && e.data !== '')
      send({ type: 'c_input', view: v.data.view, text: e.data });
  });
  // click cursor; Shift + click and pull - highlight; double - word, triple - string
  // (interprets VM)
  let dragging = false;
  let last = '';
  pane.addEventListener('mousedown', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || e.button !== 0) return;
    e.preventDefault();
    activate(v);
    const at = v.placeAt(e.clientX, e.clientY);
    if (!at) return;
    dragging = true;
    last = `${String(at.node)}/${String(at.piece)}/${String(at.offset)}`;
    if (e.detail >= 2 || e.shiftKey)
      send({
        type: 'c_select',
        view: v.data.view,
        ...at,
        clicks: e.shiftKey ? 1 : e.detail,
      });
    else send({ type: 'c_pointer', view: v.data.view, ...at });
  });
  pane.addEventListener('mousemove', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || !dragging || (e.buttons & 1) === 0) return;
    const at = v.placeAt(e.clientX, e.clientY);
    if (!at) return;
    const key = `${String(at.node)}/${String(at.piece)}/${String(at.offset)}`;
    if (key === last) return;
    last = key;
    send({ type: 'c_select', view: v.data.view, ...at, clicks: 1 });
  });
  pane.addEventListener('mouseup', () => {
    dragging = false;
  });
  pane.addEventListener('mousemove', (e) => {
    const v = [...views.values()].find((x) => x.host === pane);
    if (!v || dragging) return;
    if (e.composedPath().includes(v.tip)) {
      clearTimeout(hoverTimer); // pointer
      tipDist = 0;
      return;
    }
    const n = ViewElement.nodeOf(e);
    v.pointPath(n);
    // a hint is opened: the pointer goes to it (closer than it was) - holds, through neighboring node;
    // otherwise - immediately goes out, and a hint of the new node is shown
    if (!v.tip.hidden && n?.nodeId !== hoverNode) {
      const d = distTo(v.tip, e.clientX, e.clientY);
      const toward = d < tipDist - 0.5;
      tipDist = d;
      if (toward) return;
      hoverNode = -1;
      v.hideHover();
    }
    clearTimeout(hoverTimer);
    if (!n) {
      hoverNode = -1;
      return;
    }
    if (n.nodeId === hoverNode && !v.tip.hidden) return; // The same clue is already open.
    // without delay: only rattle, so as not to ask VM for each pixel
    hoverTimer = setTimeout(() => {
      hoverNode = n.nodeId;
      hoverAt = { x: e.clientX, y: e.clientY };
      send({ type: 'c_hover', view: v.data.view, node: n.nodeId });
    }, 60);
  });
  // Structure mode: view and place of node under the pointer, path in the state bar
  pane.addEventListener('mousemove', (e) => {
    if (!structure()) return;
    const path = e
      .composedPath()
      .filter(
        (x): x is Element =>
          x instanceof Element && x.classList.contains('vish-node'),
      )
      .reverse();
    const inner = path.at(-1);
    flashStatus('path', inspector.describe(path)); // path node under pointer
    const r = inner
      ? inspector.roles.get(Number(inner.getAttribute('data-id')))
      : undefined;
    inspector.showLabel(
      inner
        ? [inspector.describe([inner]), r?.rule ?? '']
            .filter((x) => x !== '')
            .join('  ← ')
        : '',
      e.clientX,
      e.clientY,
    );
  });
  pane.addEventListener('mouseleave', (e) => {
    inspector.showLabel('', 0, 0);
    clearTimeout(hoverTimer);
    // left the panel not to the hint - fade
    const to = e.relatedTarget;
    if (
      to instanceof Node &&
      [...views.values()].some((v) => v.tip.contains(to))
    )
      return;
    hoverNode = -1;
    for (const v of views.values()) if (v.host === pane) v.hideHover();
  });
}
