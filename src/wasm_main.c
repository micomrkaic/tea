/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* wasm_main.c — browser entry points.  Replaces main.c in the WASM build.
 *
 * The page calls:
 *   tea_web_init()            once, after the module loads
 *   tea_web_exec(line) -> int feeds one physical input line; returns 1 when
 *                          the engine wants a continuation ("> ") prompt
 *   tea_web_version() -> str version string for the banner
 *
 * Output goes through stdout/stderr, which Emscripten routes to
 * Module.print / Module.printErr — the page wires those to xterm.js.
 */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <stdio.h>
#include "interp.h"
#include "dataset.h"

static Workspace  *g_web_ws = NULL;
static Interp     *g_web_ip = NULL;
static TeaSession *g_web_s  = NULL;

EMSCRIPTEN_KEEPALIVE
int tea_web_init(void){
    if (g_web_s) return 0;                    /* already initialized */
    g_web_ws = ws_new();
    g_web_ip = interp_new(g_web_ws);
    g_web_s  = tea_session_new(g_web_ip, /*interactive=*/true);
    return g_web_s ? 0 : 1;
}

EMSCRIPTEN_KEEPALIVE
int tea_web_exec(const char *line){
    if (!g_web_s) return -1;
    bool need_more = false;
    tea_session_feed(g_web_s, line ? line : "", &need_more);
    fflush(stdout); fflush(stderr);
    return need_more ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
const char *tea_web_version(void){
    return TEA_VERSION;
}

/* Batch entry point: run a do-file from MEMFS through the exact same
 * non-interactive path as the native binary (used to validate the WASM
 * build against the regression suite; also useful for a "run .do" button). */
EMSCRIPTEN_KEEPALIVE
int tea_web_run_dofile(const char *path, int tea_extensions){
    Workspace *ws = ws_new();
    Interp *ip = interp_new(ws);
    if(tea_extensions) ip->strict_stata = false;
    FILE *f = fopen(path, "r");
    if(!f){ fprintf(stderr, "cannot open do-file %s\n", path); interp_free(ip); ws_free(ws); return 1; }
    int rc = run_stream(ip, f, false);
    fclose(f);
    fflush(stdout); fflush(stderr);
    interp_free(ip);
    ws_free(ws);
    return rc;
}
#endif /* __EMSCRIPTEN__ */

/* keep the translation unit non-empty in native builds */
typedef int tea_wasm_main_placeholder;
