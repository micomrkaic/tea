/* tea — embed-API interrupt gate.
 * Proves the cooperative Break contract from tea_embed.h:
 *   1. tea_embed_interrupt() may be called between commands (the GUI
 *      calls it from the UI thread);
 *   2. the NEXT command boundary consumes it: the command is not run,
 *      rc becomes 1 (mirrored into _rc for the evaluator);
 *   3. the flag is one-shot: the command after that runs normally and
 *      a table command resets rc to 0 per tea's rc convention.
 */
#include <stdio.h>
#include "tea_embed.h"
int main(void){
    if (tea_embed_init() != 0) { fprintf(stderr, "init failed\n"); return 1; }
    tea_embed_exec("quietly set obs 5");
    if (tea_embed_last_rc() != 0) { fprintf(stderr, "setup rc=%d\n", tea_embed_last_rc()); return 1; }
    tea_embed_interrupt();
    tea_embed_exec("quietly count");        /* broken, not run */
    if (tea_embed_last_rc() != 1) {
        fprintf(stderr, "break not honored: rc=%d\n", tea_embed_last_rc());
        return 1;
    }
    tea_embed_exec("quietly count");        /* flag consumed: runs, rc back to 0 */
    if (tea_embed_last_rc() != 0) {
        fprintf(stderr, "break flag stuck: rc=%d\n", tea_embed_last_rc());
        return 1;
    }
    puts("embed break test: PASS");
    return 0;
}
