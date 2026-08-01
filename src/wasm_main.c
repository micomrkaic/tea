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
#include <string.h>
#include "tea_version.h"
#include "interp.h"
#include "dataset.h"
#include "dta.h"

static Workspace  *g_web_ws = NULL;
static Interp     *g_web_ip = NULL;
static TeaSession *g_web_s  = NULL;

EMSCRIPTEN_KEEPALIVE
void tea_web_set_style(int on){
    extern int g_tea_style;
    g_tea_style = on ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int tea_web_init(void){
    if (g_web_s) return 0;                    /* already initialized */
    g_web_ws = ws_new();
    g_web_ip = interp_new(g_web_ws);
    g_web_s  = tea_session_new(g_web_ip, /*interactive=*/true);
    return g_web_s ? 0 : 1;
}

/* Serialize the CURRENT in-memory dataset to a .dta at `path`, silently.
 * Returns 0 on success, 1 if no data is loaded, 2 on write error.  Used
 * by the "Download workspace files" button so the workspace download
 * includes the data being worked on — no explicit `save` required. */
EMSCRIPTEN_KEEPALIVE
int tea_web_save_memory(const char *path);
EMSCRIPTEN_KEEPALIVE
int tea_web_save_memory(const char *path){
    if (!g_web_ws || !g_web_ws->cur) return 1;
    Frame *f = g_web_ws->cur;
    if (f->nvar == 0 || f->nobs == 0) return 1;
    const char *err = NULL;
    int rc = dta_write(f, g_web_ws, path, 118, &err);
    return rc == 0 ? 0 : 2;
}

/* Cheap fingerprint of the current in-memory dataset: names, types,
 * labels, and all data bytes, FNV-1a folded to 32 bits.  The UI calls
 * this after every executed command to light the "Save data" button
 * exactly when memory has changed since the last save.  0 = no data. */
EMSCRIPTEN_KEEPALIVE
unsigned tea_web_data_hash(void);
EMSCRIPTEN_KEEPALIVE
unsigned tea_web_data_hash(void){
    if (!g_web_ws || !g_web_ws->cur) return 0;
    Frame *f = g_web_ws->cur;
    if (f->nvar == 0) return 0;
    unsigned h = 2166136261u;
    #define MIX(p, n) do { const unsigned char *_b = (const unsigned char*)(p); \
        for (size_t _i = 0; _i < (n); _i++){ h ^= _b[_i]; h *= 16777619u; } } while (0)
    MIX(&f->nobs, sizeof f->nobs);
    for (int v = 0; v < f->nvar; v++){
        Variable *V = &f->vars[v];
        MIX(V->name, strlen(V->name)+1);
        MIX(V->vlabel, strlen(V->vlabel)+1);
        MIX(&V->type, sizeof V->type);
        if (V->type == VT_NUM){
            MIX(V->num, f->nobs * sizeof(double));
        } else {
            for (size_t i = 0; i < f->nobs; i++){
                const char *s = V->str[i] ? V->str[i] : "";
                MIX(s, strlen(s)+1);
            }
        }
    }
    #undef MIX
    return h ? h : 1;   /* reserve 0 for "no data" */
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

/* completion for the browser terminal: newline-joined candidates */
extern int tea_complete(Frame *f, const char *line, int point,
                        char *out, size_t outsz);
EMSCRIPTEN_KEEPALIVE
const char *tea_web_complete(const char *line, int point){
    static char buf[8192];
    buf[0] = 0;
    if(g_web_ws) tea_complete(g_web_ws->cur, line ? line : "", point, buf, sizeof buf);
    return buf;
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
