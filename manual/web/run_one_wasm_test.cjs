// Run ONE regression test inside the WASM module, all output to stdout.
// Used as a subprocess by run_wasm_tests.cjs so that `shell` command output
// (which node's emscripten system() writes to the inherited fd) is captured
// in the same stream, in order, exactly like the native harness's 2>&1.
const fs = require('fs');
const path = require('path');
const createTea = require('./tea.js');
const name = process.argv[2];
const REG = path.resolve(__dirname, '../tests/regression');
(async () => {
  const M = await createTea({
    wasmBinary: fs.readFileSync(path.join(__dirname, 'tea.wasm')),
    print:    s => process.stdout.write(s + '\n'),
    printErr: s => process.stdout.write(s + '\n'),
  });
  (function load(s, d) {
    M.FS.mkdir(d);
    for (const e of fs.readdirSync(s, { withFileTypes: true })) {
      const sp = path.join(s, e.name), dp = d + '/' + e.name;
      if (e.isDirectory()) load(sp, dp);
      else M.FS.writeFile(dp, fs.readFileSync(sp));
    }
  })(path.resolve(__dirname, '../tests'), '/tests');
  // Mount host /tmp over MEMFS /tmp so `shell` (which spawns host processes)
  // sees the files tea writes there — mirrors native test semantics.
  try { M.FS.mount(M.NODEFS, { root: '/tmp' }, '/tmp'); } catch (e) {}
  const flagsFile = path.join(REG, name + '.flags');
  const ext = fs.existsSync(flagsFile) &&
              fs.readFileSync(flagsFile, 'utf8').includes('--tea-extensions') ? 1 : 0;
  M.ccall('tea_web_run_dofile', 'number', ['string','number'],
          ['/tests/regression/' + name + '.do', ext]);
  process.exit(0);
})();
