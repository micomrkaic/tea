/* progress.c — see progress.h for the contract. */
#define _POSIX_C_SOURCE 200809L
#include "progress.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_progress_enabled = 1;

#ifdef __EMSCRIPTEN__
void progress_begin(const char *label, size_t total){ (void)label; (void)total; }
void progress_step(size_t n){ (void)n; }
void progress_end(void){ }
#else

#define ACTIVATE_NS   1000000000LL   /* 1s before anything is drawn   */
#define REDRAW_NS      250000000LL   /* then at most every 250ms      */
#define CHECK_EVERY        65536     /* clock checked every 64K units */

static struct {
    int active;                /* between begin and end               */
    int drawn;                 /* something is on the line            */
    int last_len;              /* chars of the last draw (for erase)  */
    char label[64];
    size_t total, done, acc;   /* acc: units since last clock check   */
    long long t0, tlast;       /* monotonic ns                        */
} P;

static long long now_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec*1000000000LL + ts.tv_nsec;
}

/* 1234567 -> "1,234,567" (buf must hold 32) */
static void commafmt(size_t v, char *buf){
    char raw[32]; int rl = snprintf(raw, sizeof raw, "%zu", v);
    int w = 0;
    for(int i = 0; i < rl; i++){
        if(i && (rl - i) % 3 == 0) buf[w++] = ',';
        buf[w++] = raw[i];
    }
    buf[w] = 0;
}

static void draw(void){
    char line[160], a[32], b[32];
    commafmt(P.done, a);
    if(P.total){
        int pct = (int)(100.0 * (double)P.done / (double)P.total);
        if(pct > 100) pct = 100;
        commafmt(P.total, b);
        snprintf(line, sizeof line, "%s: %d%% (%s/%s)", P.label, pct, a, b);
    } else {
        snprintf(line, sizeof line, "%s: %s", P.label, a);
    }
    int len = (int)strlen(line);
    /* pad over any longer previous draw so no tail is left behind */
    fprintf(stderr, "\r%s%*s", line, P.last_len > len ? P.last_len - len : 0, "");
    fflush(stderr);
    P.last_len = len;
    P.drawn = 1;
}

void progress_begin(const char *label, size_t total){
    if(P.active) return;                        /* outer operation owns the line */
    if(!g_progress_enabled || !isatty(2)) return;
    memset(&P, 0, sizeof P);
    P.active = 1;
    snprintf(P.label, sizeof P.label, "%s", label);
    P.total = total;
    P.t0 = P.tlast = now_ns();
}

void progress_step(size_t n){
    if(!P.active) return;
    P.done += n;
    P.acc  += n;
    if(P.acc < CHECK_EVERY) return;             /* hot path: two adds + compare */
    P.acc = 0;
    long long t = now_ns();
    if(!P.drawn){ if(t - P.t0    < ACTIVATE_NS) return; }
    else        { if(t - P.tlast < REDRAW_NS)   return; }
    P.tlast = t;
    draw();
}

void progress_end(void){
    if(!P.active) return;
    if(P.drawn){
        fprintf(stderr, "\r%*s\r", P.last_len, "");
        fflush(stderr);
    }
    P.active = 0;
}
#endif
