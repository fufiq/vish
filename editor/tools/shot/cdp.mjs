// Драйвер окна по CDP: node cdp.mjs <порт> <команда> [аргумент]
//   eval <выражение> | shot <файл.png> | call <метод> [json]; PAGE — подстрока url страницы
import { writeFileSync } from 'node:fs';
const [port, cmd, arg] = process.argv.slice(2);
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
if (cmd === 'eval') {
  const r = await call('Runtime.evaluate', { expression: arg, awaitPromise: true, returnByValue: true });
  console.log(JSON.stringify(r.result?.value ?? r, null, 1));
} else if (cmd === 'shot') {
  const r = await call('Page.captureScreenshot', { format: 'png' });
  writeFileSync(arg, Buffer.from(r.data, 'base64'));
  console.log('ok');
} else if (cmd === 'call') {
  const r = await call(arg, JSON.parse(process.argv[5] ?? '{}'));
  console.log(JSON.stringify(r).slice(0, 300));
}
ws.close();
