/* progress.h — time-gated progress line for long operations.
 *
 * Contract: cheap enough to call from hot loops (a masked branch until the
 * activation threshold), and INVISIBLE to anything non-interactive:
 *   - drawn to stderr only when stderr is a TTY
 *   - nothing appears until ~1s of wall time has elapsed (short operations
 *     therefore produce zero output)
 *   - redraws at most every 250ms; erased with \r on completion, so nothing
 *     lands in scrollback or logs
 *   - `set progress off` disables globally
 *   - compiled to no-ops under __EMSCRIPTEN__ (no TTY in the browser;
 *     a JS hook can come later)
 *
 * Usage:
 *   progress_begin("reshape long", total_rows);   // total 0 = unknown
 *   loop { ...; progress_step(n); }
 *   progress_end();
 * begin/end pairs don't nest; a begin while active is ignored (the outer
 * operation owns the line). */
#ifndef TEA_PROGRESS_H
#define TEA_PROGRESS_H
#include <stddef.h>

extern int g_progress_enabled;   /* `set progress on|off`; default 1 */

void progress_begin(const char *label, size_t total);
void progress_step(size_t n);
void progress_end(void);

/* activity mode: external work of unknown length (a converter child).
 * Same contract (TTY-only, 1s activation, erased on end); renders a
 * spinner + elapsed seconds instead of a count. */
void progress_begin_activity(const char *label);
void progress_tick(void);

#endif
