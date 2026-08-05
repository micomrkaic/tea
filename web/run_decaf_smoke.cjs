// decaf wasm smoke gate: boot web/decaf/tea.js, run the tier's
// integration test (80_decaf_plumbing) through the wasm and diff
// against the shared golden, then assert the estimation tier is
// genuinely absent (regress must be an unrecognized command).
const fs = require('fs');
const path = require('path');
delete globalThis.fetch;
const createTea = require('./decaf/tea.js');

(async () => {
  const M = await createTea({
    wasmBinary: fs.readFileSync(path.join(__dirname, 'decaf', 'tea.wasm')),
    print: s => out.push(s),
    printErr: s => out.push(s),
  });
  let out = [];
  M._tea_web_init();

  const ver = M.UTF8ToString(M._tea_web_version());
  console.log('decaf wasm version:', ver);

  // 1) plumbing golden through the wasm
  const doPath = path.join(__dirname, '..', 'tests', 'regression', '80_decaf_plumbing.do');
  const golden = fs.readFileSync(
    path.join(__dirname, '..', 'tests', 'regression', '80_decaf_plumbing.expected'), 'utf8').trimEnd();
  M.FS.writeFile('/t80.do', fs.readFileSync(doPath, 'utf8'));
  out = [];
  const p = M.allocateUTF8 ? M.allocateUTF8('/t80.do') : (() => {
    const b = M._malloc(8); M.stringToUTF8('/t80.do', b, 8); return b; })();
  M._tea_web_run_dofile(p); M._free(p);
  const got = out.join('\n').trimEnd();
  if (got !== golden) {
    console.error('decaf wasm: plumbing output diverges from golden');
    const g = golden.split('\n'), a = got.split('\n');
    for (let i = 0; i < Math.max(g.length, a.length); i++)
      if (g[i] !== a[i]) { console.error(`  line ${i+1}:\n  - ${g[i]}\n  + ${a[i]}`); break; }
    process.exit(1);
  }
  console.log('decaf wasm: plumbing golden matches');

  // 2) estimation must be absent
  out = [];
  const rc = M.ccall('tea_web_exec', 'number', ['string'], ['regress y x']);
  const text = out.join('\n');
  // tea_web_exec's return is line-processed status, not the command rc;
  // absence is proven by the unrecognized-command error itself
  if (!/unrecognized command: regress/.test(text)) {
    console.error('decaf wasm LEAK: regress executed (rc=' + rc + '): ' + text);
    process.exit(1);
  }
  console.log('decaf wasm: estimation tier absent (regress -> unrecognized)');
  console.log('decaf wasm smoke: PASS');
})().catch(e => { console.error('decaf smoke failed:', e); process.exit(1); });
