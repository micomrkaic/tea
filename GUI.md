# tea-qt — the Qt desktop shell

tea-qt is the third frontend over the one tea core, next to the
readline REPL (`./tea`) and the browser build (`web/`).  It is a
native Qt6 Widgets application:

- a **terminal-style console**: the prompt lives at the tail of the
  results document and input interleaves with output exactly as in
  the CLI — Tab completion, Up/Down history, read-only scrollback;
- a **do-file editor** dock: open/save/run file/run selection; running
  writes the buffer to a temp file and feeds it through `do`, so what
  you see is what runs, no save required (Ctrl+R / Ctrl+Shift+R);
- a virtual-scrolling **data browser** over the live frame (zero
  copies, value labels rendered, 1-based observation numbers);
- a variables pane (double-click inserts the name at the prompt) and
  a history pane (double-click reruns);
- an SVG **plots dock** that picks up `tea_graph.svg` as graph
  commands write it — baselined at startup, so a stale graph from an
  earlier session in the same directory never surfaces;
- a **Break** toolbar action, honored at the next command boundary.

## Architecture

- The core stays pure C17.  `gui/tea_qt.cpp` is the only C++
  translation unit in the tree, and it talks to the core exclusively
  through the neutral embedding API `src/tea_embed.h` — the same
  surface the browser frontend uses.  GUI code never includes a core
  header.
- All embed calls run on a worker `QThread`; the UI never blocks on
  a running command.  The single any-thread call is
  `tea_embed_interrupt()` (the Break button), a cooperative flag the
  interpreter polls at every command boundary — each do-file line,
  each loop iteration — so a runaway `foreach` stops at the next
  line without corrupting the frame mid-command.
- The data and variables models are `QAbstractTableModel`s reading
  the frame in place through the embed accessors, refreshed BETWEEN
  commands only (full model reset on command completion).
- stdout/stderr are captured at the fd level (pipes + reader
  threads), so every core TU's output — including estimation tables —
  reaches the results pane uniformly, with zero core changes.

## Building

    make gui        # or: make qt      -> ./tea-qt
    make gui-test   # headless gate: offscreen window runs a do-file
                    # through the worker, asserts output + models
    make embed-test # the interrupt-contract gate (core-level)

Linux needs qt6-base-dev + qt6-svg-dev; macOS: brew install qt6
(then macdeployqt for a self-contained .app — planned, not yet
wired).  Qt is linked dynamically under LGPLv3; tea remains GPLv3.

## Testing

`make gate` runs both native regression suites, the decaf leak
check, the embed interrupt gate, and — when Qt is present — the
headless GUI smoke.  The GUI is not an untested surface: the smoke
constructs the real window offscreen, runs a real do-file through
the real worker thread, and asserts the captured output and both
model dimensions.

## Planned (v1.1+)

- do-file editor tab with QSyntaxHighlighter and run-selection
- `make gui-decaf`: the same shell over the decaf tier — a native
  "decaf tea.app" data-management application with no terminal
  anywhere
- macdeployqt packaging for a drag-installable macOS bundle
