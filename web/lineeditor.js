/* tea browser terminal — readline-style line editor.
 * Pure logic (no DOM/xterm dependency): the host supplies callbacks.
 * Handles: cursor movement (arrows, Home/End, Ctrl-A/E/B/F), editing
 * (Backspace, Delete, Ctrl-K/U/W), history (Up/Down with in-progress
 * stash), Ctrl-L clear, Ctrl-C cancel, Tab completion, and paste
 * (including multi-line).  GPLv3, part of tea.
 */
'use strict';

class LineEditor {
  /**
   * host = {
   *   write(s)            raw output to the terminal
   *   prompt()            current prompt string
   *   cols()              terminal width
   *   submit(line)        a completed line
   *   complete(line, pt)  -> array of candidate strings (may be empty)
   *   clearScreen()       Ctrl-L
   * }
   */
  constructor(host) {
    this.h = host;
    this.buf = '';
    this.pos = 0;
    this.hist = [];
    this.histIdx = 0;      // == hist.length when editing a fresh line
    this.stash = '';       // in-progress line saved during history browsing
  }

  /* ---- rendering ------------------------------------------------------ */
  // Redraw prompt+buffer and place the cursor, correctly across soft-wrapped
  // rows: move to the top of the edit region, clear down, rewrite, reposition.
  _cursorRow() {                    // row of the cursor within the edit region
    return Math.floor((this.h.prompt().length + this.pos) / this.h.cols());
  }
  render() {
    const p = this.h.prompt(), cols = this.h.cols();
    const up = this._lastCursorRow || 0;
    let out = '';
    if (up > 0) out += `\x1b[${up}A`;         // to first row of region
    out += '\r\x1b[J' + p + this.buf;          // clear down, rewrite
    const endRow = Math.floor((p.length + this.buf.length) / cols);
    const curRow = Math.floor((p.length + this.pos) / cols);
    const curCol = (p.length + this.pos) % cols;
    // terminal cursor now sits at end of text; move to (curRow, curCol)
    if (endRow > curRow) out += `\x1b[${endRow - curRow}A`;
    out += '\r';
    if (curCol > 0) out += `\x1b[${curCol}C`;
    this.h.write(out);
    this._lastCursorRow = curRow;
  }
  freshPrompt() {                    // after submit/cancel: region starts anew
    this._lastCursorRow = 0;
    this.buf = ''; this.pos = 0;
    this.render();
  }

  /* ---- edit primitives ------------------------------------------------ */
  insert(s) {
    this.buf = this.buf.slice(0, this.pos) + s + this.buf.slice(this.pos);
    this.pos += s.length;
    this.render();
  }
  backspace() {
    if (!this.pos) return;
    this.buf = this.buf.slice(0, this.pos - 1) + this.buf.slice(this.pos);
    this.pos--; this.render();
  }
  del() {
    if (this.pos >= this.buf.length) return;
    this.buf = this.buf.slice(0, this.pos) + this.buf.slice(this.pos + 1);
    this.render();
  }
  killToEnd()   { this.buf = this.buf.slice(0, this.pos); this.render(); }
  killToStart() { this.buf = this.buf.slice(this.pos); this.pos = 0; this.render(); }
  killWord() {
    let i = this.pos;
    while (i > 0 && this.buf[i-1] === ' ') i--;
    while (i > 0 && this.buf[i-1] !== ' ') i--;
    this.buf = this.buf.slice(0, i) + this.buf.slice(this.pos);
    this.pos = i; this.render();
  }
  move(d) {
    const np = Math.min(this.buf.length, Math.max(0, this.pos + d));
    if (np !== this.pos) { this.pos = np; this.render(); }
  }
  home() { if (this.pos) { this.pos = 0; this.render(); } }
  end()  { if (this.pos !== this.buf.length) { this.pos = this.buf.length; this.render(); } }

  /* ---- history --------------------------------------------------------- */
  histUp() {
    if (!this.hist.length || this.histIdx === 0) return;
    if (this.histIdx === this.hist.length) this.stash = this.buf;
    this.histIdx--;
    this.buf = this.hist[this.histIdx];
    this.pos = this.buf.length;
    this.render();
  }
  histDown() {
    if (this.histIdx >= this.hist.length) return;
    this.histIdx++;
    this.buf = this.histIdx === this.hist.length ? this.stash : this.hist[this.histIdx];
    this.pos = this.buf.length;
    this.render();
  }

  /* ---- completion ------------------------------------------------------ */
  static commonPrefix(a) {
    if (!a.length) return '';
    let p = a[0];
    for (const s of a) { let i = 0; while (i < p.length && p[i] === s[i]) i++; p = p.slice(0, i); }
    return p;
  }
  tab() {
    const cands = this.h.complete(this.buf, this.pos) || [];
    if (!cands.length) return;
    // word being completed: back up to a separator
    let ws = this.pos;
    while (ws > 0 && !' ,('.includes(this.buf[ws-1])) ws--;
    const word = this.buf.slice(ws, this.pos);
    if (cands.length === 1) {
      const add = cands[0].slice(word.length) + ' ';
      this.insert(add);
      return;
    }
    const cp = LineEditor.commonPrefix(cands);
    if (cp.length > word.length) {
      this.insert(cp.slice(word.length));
      return;
    }
    // ambiguous: list candidates below, then redraw the line
    this.h.write('\r\n' + cands.join('  ') + '\r\n');
    this._lastCursorRow = 0;
    this.render();
  }

  /* ---- submit / cancel -------------------------------------------------- */
  enter() {
    this.h.write('\r\n');
    const line = this.buf;
    if (line.trim().length) {
      this.hist.push(line);
    }
    this.histIdx = this.hist.length; this.stash = '';
    this.buf = ''; this.pos = 0; this._lastCursorRow = 0;
    this.h.submit(line);
  }
  cancel() {                        // Ctrl-C
    this.h.write('^C\r\n');
    this.histIdx = this.hist.length; this.stash = '';
    this.freshPrompt();
  }

  /* ---- input dispatch --------------------------------------------------- */
  // Feed raw xterm onData strings (single keys, escape sequences, or pastes).
  feed(data) {
    let i = 0;
    while (i < data.length) {
      const ch = data[i];
      if (ch === '\x1b') {                       // escape sequences
        const rest = data.slice(i);
        const m = rest.match(/^\x1b(\[[0-9;]*[A-Za-z~]|O[A-Z])/);
        const seq = m ? m[0] : '\x1b';
        switch (seq) {
          case '\x1b[A': this.histUp(); break;
          case '\x1b[B': this.histDown(); break;
          case '\x1b[C': this.move(1); break;
          case '\x1b[D': this.move(-1); break;
          case '\x1b[H': case '\x1bOH': case '\x1b[1~': this.home(); break;
          case '\x1b[F': case '\x1bOF': case '\x1b[4~': this.end(); break;
          case '\x1b[3~': this.del(); break;
          default: break;                        // ignore unknown sequences
        }
        i += seq.length;
        continue;
      }
      if (ch === '\r' || ch === '\n') {
        this.enter();
        if (i + 1 < data.length && ch === '\r' && data[i+1] === '\n') i++;
        i++;
        continue;
      }
      switch (ch) {
        case '\x7f': case '\b': this.backspace(); break;
        case '\t':   this.tab(); break;
        case '\x01': this.home(); break;          // Ctrl-A
        case '\x05': this.end(); break;           // Ctrl-E
        case '\x02': this.move(-1); break;        // Ctrl-B
        case '\x06': this.move(1); break;         // Ctrl-F
        case '\x0b': this.killToEnd(); break;     // Ctrl-K
        case '\x15': this.killToStart(); break;   // Ctrl-U
        case '\x17': this.killWord(); break;      // Ctrl-W
        case '\x0c':                                // Ctrl-L
          this.h.clearScreen();
          this._lastCursorRow = 0;
          this.render();
          break;
        case '\x03': this.cancel(); break;        // Ctrl-C
        default:
          if (ch >= ' ') this.insert(ch);
      }
      i++;
    }
  }
}

if (typeof module !== 'undefined') module.exports = LineEditor;
