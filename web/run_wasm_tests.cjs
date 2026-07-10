// tea WASM regression harness: spawn one node per test so that both
// Module.print output and `shell` (spawnSync, inherited fd) output are
// captured in a single ordered stream, then diff against .expected.
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const REG = path.resolve(__dirname, '../tests/regression');
const dos = fs.readdirSync(REG).filter(f => f.endsWith('.do')).sort();
let pass = 0, fail = 0; const failed = [];
for (const f of dos) {
  const name = f.replace(/\.do$/, '');
  const expectedFile = path.join(REG, name + '.expected');
  if (!fs.existsSync(expectedFile)) { console.log('SKIP', name); continue; }
  const r = spawnSync('node', [path.join(__dirname, 'run_one_wasm_test.cjs'), name],
                      { encoding: 'utf8', maxBuffer: 64*1024*1024 });
  const actual = (r.stdout || '').replace(/\n$/, '');
  const expected = fs.readFileSync(expectedFile, 'utf8').replace(/\n$/, '');
  if (actual === expected) { pass++; console.log('PASS', name); }
  else {
    fail++; failed.push(name);
    console.log('FAIL', name);
    const a = actual.split('\n'), e = expected.split('\n');
    for (let i = 0; i < Math.max(a.length, e.length); i++)
      if (a[i] !== e[i]) { console.log('  line', i+1, '\n   exp:', JSON.stringify(e[i]), '\n   got:', JSON.stringify(a[i])); break; }
  }
}
console.log(`\ntea WASM regression: ${pass}/${pass+fail} passed`);
if (fail) { console.log('  failed:', failed.join(' ')); process.exit(1); }
