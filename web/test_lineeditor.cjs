// Unit tests for web/lineeditor.js — pure-logic checks under node.
const LineEditor = require('./lineeditor.js');
const assert = require('assert');

function mk(completions) {
  const out = [];
  const submitted = [];
  const ed = new LineEditor({
    write: s => out.push(s),
    prompt: () => '. ',
    cols: () => 80,
    submit: l => submitted.push(l),
    complete: (line, pt) => completions ? completions(line, pt) : [],
    clearScreen: () => out.push('<CLS>'),
  });
  return { ed, out, submitted };
}

let t = 0; const ok = name => console.log('PASS', ++t + '.', name);

// typing and cursor movement
{
  const { ed } = mk();
  ed.feed('regress y x');
  assert.equal(ed.buf, 'regress y x'); assert.equal(ed.pos, 11);
  ed.feed('\x1b[D\x1b[D');                    // left twice
  assert.equal(ed.pos, 9);
  ed.feed('z');                                // insert mid-line
  assert.equal(ed.buf, 'regress yz x'); assert.equal(ed.pos, 10);
  ed.feed('\x01');                             // Ctrl-A
  assert.equal(ed.pos, 0);
  ed.feed('\x05');                             // Ctrl-E
  assert.equal(ed.pos, 12);
  ok('cursor movement + mid-line insert');
}

// backspace / delete
{
  const { ed } = mk();
  ed.feed('abcd\x1b[D\x1b[D');                 // cursor between b and c
  ed.feed('\x7f');                              // backspace removes b
  assert.equal(ed.buf, 'acd'); assert.equal(ed.pos, 1);
  ed.feed('\x1b[3~');                           // delete removes c
  assert.equal(ed.buf, 'ad');
  ok('backspace and delete at cursor');
}

// kill commands
{
  const { ed } = mk();
  ed.feed('summarize gdp growth\x01');
  ed.feed('\x0b');                              // Ctrl-K from col 0 kills all
  assert.equal(ed.buf, '');
  ed.feed('one two three');
  ed.feed('\x17');                              // Ctrl-W kills "three"
  assert.equal(ed.buf, 'one two ');
  ed.feed('\x15');                              // Ctrl-U kills to start
  assert.equal(ed.buf, '');
  ok('Ctrl-K / Ctrl-W / Ctrl-U');
}

// history with stash
{
  const { ed, submitted } = mk();
  ed.feed('first\r'); ed.feed('second\r');
  assert.deepEqual(submitted, ['first', 'second']);
  ed.feed('draft');                             // in-progress line
  ed.feed('\x1b[A');                            // up -> second
  assert.equal(ed.buf, 'second');
  ed.feed('\x1b[A');                            // up -> first
  assert.equal(ed.buf, 'first');
  ed.feed('\x1b[A');                            // at oldest: stays
  assert.equal(ed.buf, 'first');
  ed.feed('\x1b[B'); ed.feed('\x1b[B');         // down, down -> draft restored
  assert.equal(ed.buf, 'draft');
  ok('history up/down with in-progress stash');
}

// tab completion: unique, common prefix, ambiguous list
{
  const table = ['regress', 'replace', 'rename', 'reshape'];
  const { ed, out } = mk((line, pt) => {
    const w = line.slice(0, pt).split(/[ ,(]/).pop();
    return table.filter(c => c.startsWith(w));
  });
  ed.feed('reg\t');
  assert.equal(ed.buf, 'regress ');             // unique -> complete + space
  ed.feed('\x03');                              // cancel line
  ed.feed('re\t');
  assert.equal(ed.buf, 're');                   // ambiguous, no growth: listed
  assert.ok(out.join('').includes('regress  replace  rename  reshape'));
  ed.feed('s\t');
  assert.equal(ed.buf, 'reshape ');             // unique after 's'
  ok('tab completion: unique / list / prefix');
}

// Ctrl-L and Ctrl-C
{
  const { ed, out } = mk();
  ed.feed('half a line');
  ed.feed('\x0c');
  assert.ok(out.includes('<CLS>'));
  assert.equal(ed.buf, 'half a line');          // Ctrl-L preserves the line
  ed.feed('\x03');
  assert.equal(ed.buf, '');                     // Ctrl-C clears it
  ok('Ctrl-L preserves line; Ctrl-C cancels');
}

// paste with embedded newlines submits line-by-line
{
  const { ed, submitted } = mk();
  ed.feed('display 1\r\ndisplay 2\r\ndisplay 3');
  assert.deepEqual(submitted, ['display 1', 'display 2']);
  assert.equal(ed.buf, 'display 3');            // last fragment stays editable
  ok('multi-line paste');
}

// soft-wrap cursor arithmetic (regression guard for the renderer)
{
  const { ed } = mk();
  ed.h.cols = () => 10;
  ed.feed('abcdefghijklmno');                   // prompt(2) + 15 chars: 2 rows
  assert.equal(ed._cursorRow(), 1);
  ed.feed('\x01');
  assert.equal(ed._cursorRow(), 0);
  ok('wrapped-line cursor row arithmetic');
}

console.log('\nall line-editor tests passed');
