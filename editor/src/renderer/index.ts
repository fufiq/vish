// Editor window: "graffonium" - draws sent VM and sends her control signals (keys,
// pointer):
//
import { FrameSplitter } from '../protocol/codec';
import {
  Mods,
  decodeServer,
  encodeClient,
  schema,
  type ClientMsg,
  type ServerMsg,
} from '../protocol/messages';
import type { VmBridge } from '../preload/index';
import { ViewElement } from './view';
import './nodes/index';
import './styles.css';

declare global {
  interface Window {
    vm: VmBridge;
  }
}

function byId(id: string): HTMLElement {
  const el = document.getElementById(id);
  if (!el) throw new Error(`нет элемента #${id}`);
  return el;
}

const app = byId('app');
const statusLine = byId('status');
let view: ViewElement | undefined;

function send(m: ClientMsg): void {
  window.vm.send(encodeClient(m));
}

function receive(m: ServerMsg): void {
  switch (m.type) {
    case 's_hello':
      statusLine.textContent = `${m.program} · ревизия ${String(m.revision)}`;
      send({ type: 'c_open_view', kind: 'module' });
      return;
    case 's_view':
      view = new ViewElement(app, m);
      app.focus();
      return;
    case 's_update':
      view?.update(m);
      statusLine.textContent = `ревизия ${String(m.revision)}`;
      return;
    case 's_cursor':
      view?.showCursor(m);
      return;
    case 's_decor': {
      const error = view?.showMarks(m);
      statusLine.textContent =
        error === undefined
          ? `ревизия ${String(m.revision)}`
          : `ошибка: ${error.split('\n')[0] ?? ''}`;
      statusLine.toggleAttribute('error', error !== undefined);
      return;
    }
    case 's_complete':
      view?.showCompletion(m);
      return;
    case 's_error':
      statusLine.textContent = `VM: ${m.text}`;
      return;
  }
}

const split = new FrameSplitter();
window.vm.onData((bytes) => {
  split.push(bytes, (body) => {
    receive(decodeServer(body));
  });
});
window.vm.onStatus((text) => {
  statusLine.textContent = text;
});
window.vm.onReady(() => {
  send({ type: 'c_hello', version: schema.version });
});

// Keys - VM (except for developer tools and window reboot).
app.addEventListener('keydown', (e) => {
  if (!view || e.key === 'F12' || (e.ctrlKey && e.shiftKey && e.key === 'I'))
    return;
  if (['Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) return;
  e.preventDefault();
  const mods =
    (e.shiftKey ? Mods.shift : 0) |
    (e.ctrlKey ? Mods.ctrl : 0) |
    (e.altKey ? Mods.alt : 0) |
    (e.metaKey ? Mods.meta : 0);
  send({ type: 'c_key', view: view.data.view, key: e.key, mods });
});

app.addEventListener('mousedown', (e) => {
  if (!view) return;
  e.preventDefault();
  app.focus();
  const at = view.placeAt(e.clientX, e.clientY);
  if (at) send({ type: 'c_pointer', view: view.data.view, ...at });
});
