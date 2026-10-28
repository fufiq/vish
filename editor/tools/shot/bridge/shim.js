// window.vm for renderer in the browser: each tab has its socket to VM (id), data - SSE, sending - POST.
const ready = [];
const id = Math.random().toString(36).slice(2);
window.vm = {
  send(bytes) { fetch('/send?id=' + id, { method: 'POST', body: bytes }); },
  onData(cb) {
    const es = new EventSource('/events?id=' + id);
    es.addEventListener('data', (e) => cb(Uint8Array.from(atob(e.data), (c) => c.charCodeAt(0))));
    es.addEventListener('ready', () => ready.forEach((f) => f()));
  },
  onReady(cb) { ready.push(cb); },
  onStatus(cb) { window.__status = cb; },
  reconnect() {},
  aiAsk() {},
  aiStop() {},
  onAi() {},
};
