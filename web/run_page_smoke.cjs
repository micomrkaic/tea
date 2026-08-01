// run_page_smoke.cjs — execute index.html's inline scripts in a stubbed
// browser environment and require the loader to reach 'ready'.
//
// Why this exists: the wasm regression suite drives tea.wasm through
// tea.js directly and never executes the page script, so a
// ReferenceError in index.html (Bug 41: a rename left one dangling
// `Module.` reference) shipped green through a 77/77 gate and broke the
// browser edition outright.  This harness closes that hole: any thrown
// error, unhandled rejection, or failure to reach the 'ready' status
// within the timeout fails the gate.
//
// The stubs are deliberately permissive (every element exists, every
// listener attaches) — the harness tests CONTROL FLOW of the loader,
// not rendering.  xterm.js is stubbed; tea.js and tea.wasm are real.
'use strict';
const fs = require('fs');
const vm = require('vm');
const path = require('path');

const html = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');
const wasmBuf = fs.readFileSync(path.join(__dirname, 'tea.wasm'));
const teaJs = fs.readFileSync(path.join(__dirname, 'tea.js'), 'utf8');

// ---- minimal DOM ----
function makeEl(id) {
  const el = {
    id, style: {}, dataset: {}, value: '', textContent: '', innerHTML: '',
    firstChild: null, children: [],
    classList: { add(){}, remove(){}, toggle(){}, contains(){ return false; } },
    addEventListener(){}, removeEventListener(){},
    appendChild(c){ this.children.push(c); return c; },
    insertBefore(c){ this.children.unshift(c); return c; },
    querySelector(){ return makeEl('q'); },
    querySelectorAll(){ return []; },
    setAttribute(){}, getAttribute(){ return null; },
    focus(){}, blur(){}, click(){}, remove(){},
    getBoundingClientRect(){ return {width: 800, height: 600, top: 0, left: 0}; },
  };
  return el;
}
const elements = new Map();
const document = {
  getElementById(id){ if(!elements.has(id)) elements.set(id, makeEl(id)); return elements.get(id); },
  createElement(tag){ return makeEl('<'+tag+'>'); },
  addEventListener(){}, removeEventListener(){},
  querySelectorAll(){ return []; },
  body: makeEl('body'),
};

// ---- xterm stubs ----
class Terminal {
  constructor(){ this.cols = 80; this.rows = 24; }
  open(){} loadAddon(){} write(){} writeln(){} onData(){} onKey(){}
  attachCustomKeyEventHandler(){} focus(){} scrollToBottom(){} reset(){}
}
const FitAddon = { FitAddon: class { fit(){} activate(){} } };

// ---- fetch: serves the real tea.wasm with a streaming body ----
async function fetchStub(url){
  if (!/tea\.wasm/.test(String(url))) throw new Error('unexpected fetch: ' + url);
  const bytes = new Uint8Array(wasmBuf);
  let served = false;
  return {
    ok: true, status: 200,
    headers: { get: h => /content-length/i.test(h) ? String(bytes.length) : null },
    body: { getReader(){ return { read: async () =>
        served ? {done: true} : (served = true, {done: false, value: bytes}) }; } },
    arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.length),
  };
}

// ---- context ----
let fatalMsg = null;
const sandbox = {
  console, setTimeout, setInterval, clearTimeout, clearInterval,
  queueMicrotask, TextDecoder, TextEncoder, Uint8Array, Blob,
  performance: { now: () => Date.now() },
  requestAnimationFrame: cb => setTimeout(cb, 0),
  document, Terminal, FitAddon, fetch: fetchStub,
  localStorage: { getItem: () => null, setItem(){}, removeItem(){} },
  navigator: { userAgent: 'page-smoke' },
  WebAssembly, Math, Date, JSON, Promise, Error, String, Number, Object, Array,
  URL: Object.assign(function(){ }, { createObjectURL: () => 'blob:x', revokeObjectURL(){} }),
  location: { href: 'http://localhost/', search: '' },
};
sandbox.window = sandbox;
sandbox.self = sandbox;
sandbox.globalThis = sandbox;
sandbox.window.addEventListener = () => {};
const ctx = vm.createContext(sandbox);

// tea.js first (as the <script src> tag would), then every inline script
// local page scripts load in tag order: lineeditor.js (the readline-style
// editor class), then tea.js — exactly as the browser would
vm.runInContext(fs.readFileSync(path.join(__dirname, 'lineeditor.js'), 'utf8'),
                ctx, { filename: 'lineeditor.js' });
vm.runInContext(teaJs, ctx, { filename: 'tea.js' });
const inline = [...html.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/g)]
  .map(m => m[1]).filter(s => s.trim());
if (!inline.length) { console.error('page smoke: no inline scripts found'); process.exit(1); }

process.on('unhandledRejection', e => {
  console.error('page smoke: unhandled rejection:', e && e.message ? e.message : e);
  process.exit(1);
});

try {
  for (const src of inline) vm.runInContext(src, ctx, { filename: 'index.html#inline' });
} catch (e) {
  console.error('page smoke: threw during load:', e.message);
  process.exit(1);
}

// success = the loader sets #status to 'ready'; failure = the splash
// shows the fatal() message or we time out.
const statusEl = document.getElementById('status');
const splashEl = document.getElementById('splash-status');
// 'ready' is not the loader's final act — the fatal() catch can land one
// microtask later (exactly how Bug 41 presented: a flash of ready, then
// the failure splash).  So: fatal always wins, and 'ready' must hold
// steadily for a full second before it counts as success.
const t0 = Date.now();
let readyAt = 0;
(function poll(){
  if (/failed to load/.test(String(splashEl.textContent))) {
    console.error('page smoke: fatal():', splashEl.textContent);
    process.exit(1);
  }
  if (statusEl.textContent === 'ready') {
    if (!readyAt) readyAt = Date.now();
    else if (Date.now() - readyAt > 1000) {
      console.log('page smoke: ready and stable (loader completed)');
      process.exit(0);
    }
  } else readyAt = 0;
  if (Date.now() - t0 > 60000) {
    console.error('page smoke: timeout; status=%j splash=%j',
                  statusEl.textContent, splashEl.textContent);
    process.exit(1);
  }
  setTimeout(poll, 100);
})();
