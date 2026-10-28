// Ввод в окно по CDP: node act.mjs <порт> шаг… ; шаги:
//   click:<js-элемент>[:right] — щелчок (справа — у правого края); rclick:<js-элемент> — правый щелчок;
//   hover:<js-элемент> — указатель на середину; key:<Key>[+shift|+ctrl|+ctrlshift]; text:<строка>; wait:<мс>
const [port, ...steps] = process.argv.slice(2);
const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const page = list.find((t) => t.type === 'page' && (!process.env.PAGE || t.url.includes(process.env.PAGE)));
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r));
let id = 0;
const call = (method, params = {}) =>
  new Promise((ok) => {
    const my = ++id;
    const on = (e) => {
      const m = JSON.parse(e.data);
      if (m.id === my) {
        ws.removeEventListener('message', on);
        ok(m.result ?? m);
      }
    };
    ws.addEventListener('message', on);
    ws.send(JSON.stringify({ id: my, method, params }));
  });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rect = async (expr) => {
  const r = await call('Runtime.evaluate', { expression: `(()=>{const r=(${expr}).getBoundingClientRect();return [r.left,r.top,r.width,r.height]})()`, returnByValue: true });
  return r.result.value;
};
const codes = { Backspace: 8, Tab: 9, Enter: 13, Escape: 27, ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Delete: 46, F2: 113, F4: 115, F5: 116, F6: 117, F10: 121, F11: 122, F12: 123 };
for (const s of steps) {
  const [kind, ...rest] = s.split(':');
  const arg = rest.join(':');
  if (kind === 'wait') await sleep(Number(arg));
  else if (kind === 'click') {
    const right = arg.endsWith(':right');
    const [x0, y0, w, h] = await rect(right ? arg.slice(0, -6) : arg);
    const x = right ? x0 + w - 1 : x0 + 1, y = y0 + h / 2;
    for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 });
    await sleep(200);
  } else if (kind === 'rclick') {
    const [x0, y0, w, h] = await rect(arg);
    for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x: x0 + w / 2, y: y0 + h / 2, button: 'right', clickCount: 1 });
    await sleep(200);
  } else if (kind === 'hover') {
    const [x0, y0, w, h] = await rect(arg);
    await call('Input.dispatchMouseEvent', { type: 'mouseMoved', x: x0 + w / 2, y: y0 + h / 2 });
    await sleep(200);
  } else if (kind === 'move') {
    const [x, y] = arg.split(',').map(Number);
    await call('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y });
    await sleep(60);
  } else if (kind === 'key') {
    const [key, mod] = arg.split('+');
    const modifiers = (mod?.includes('shift') ? 8 : 0) | (mod?.includes('ctrl') ? 2 : 0);
    const vk = codes[key] ?? key.toUpperCase().charCodeAt(0);
    const text = modifiers === 0 && key.length === 1 ? key : key === 'Enter' ? '\r' : undefined;
    await call('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', key, text, code: key.length === 1 ? `Key${key.toUpperCase()}` : key, windowsVirtualKeyCode: vk, modifiers });
    await call('Input.dispatchKeyEvent', { type: 'keyUp', key, windowsVirtualKeyCode: vk, modifiers });
    await sleep(250);
  } else if (kind === 'text') {
    await call('Input.insertText', { text: arg });
    await sleep(250);
  }
}
ws.close();
