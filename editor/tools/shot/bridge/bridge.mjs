// Мост окна редактора в обычный браузер: сокет VM на вкладку (id), данные — SSE, отправка — POST.
// node bridge.mjs <vish> <программа> <корень renderer> <shim.js> <порт>
import { createServer } from 'node:http';
import { connect } from 'node:net';
import { spawn } from 'node:child_process';
import { readFileSync, existsSync, rmSync } from 'node:fs';
import { join, extname } from 'node:path';
const [vish, program, root, shim, port] = process.argv.slice(2);
const sock = `/tmp/claude-1000/vb${process.pid}.sock`;
if (existsSync(sock)) rmSync(sock);
spawn(vish, ['serve', program, sock], { stdio: 'inherit' });
const open = () => new Promise((ok) => { const s = connect(sock); s.once('connect', () => ok(s)); s.once('error', () => ok(undefined)); });
for (let i = 0; i < 200; ++i) { await new Promise((r) => setTimeout(r, 50)); const s = await open(); if (s) { s.end(); break; } }
const peers = new Map();
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.woff2': 'font/woff2', '.woff': 'font/woff' };
createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const id = url.searchParams.get('id');
  if (url.pathname === '/events') {
    res.writeHead(200, { 'content-type': 'text/event-stream' });
    const vm = await open();
    peers.set(id, vm);
    vm.on('data', (d) => res.write(`event: data\ndata: ${d.toString('base64')}\n\n`));
    req.on('close', () => { vm.end(); peers.delete(id); });
    res.write('event: ready\ndata: \n\n');
    return;
  }
  if (url.pathname === '/send') {
    const parts = [];
    req.on('data', (c) => parts.push(c));
    req.on('end', () => { peers.get(id)?.write(Buffer.concat(parts)); res.end(); });
    return;
  }
  if (url.pathname === '/shim.js') { res.writeHead(200, { 'content-type': 'text/javascript' }); res.end(readFileSync(shim)); return; }
  const path = url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
  const file = join(root, path);
  if (!existsSync(file)) { res.writeHead(404); res.end(); return; }
  let body = readFileSync(file);
  if (path === 'index.html') body = Buffer.from(body.toString().replace('<head>', '<head><script src="/shim.js"></script>'));
  res.writeHead(200, { 'content-type': types[extname(file)] ?? 'application/octet-stream' });
  res.end(body);
}).listen(Number(port), '127.0.0.1', () => console.log(`bridge on ${port}`));
