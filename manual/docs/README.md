# GitHub Pages deployment folder

This folder is what https://micomrkaic.github.io/tea/ serves (Settings →
Pages → branch `master`, folder `/docs`).  It is a copy of the browser
bundle in `web/`; `web/` is the source of truth, `docs/` is the deployed
snapshot.  To update the demo after rebuilding the WASM:

    cp web/index.html web/tea.js web/tea.wasm web/xterm.min.js web/xterm.min.css web/tea_logo.jpg docs/
    git add -f docs/ && git commit -m "update Pages demo" && git push
