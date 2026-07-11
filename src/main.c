#define _GNU_SOURCE
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
#include "interp.h"
#include "dataset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Interactive startup banner (stderr, REPL only — never in batch mode, so
 * do-file output and the regression suite stay byte-identical).  Colors only
 * when stderr is a terminal, TERM isn't dumb, and NO_COLOR is unset
 * (https://no-color.org).  Suppress entirely with -q / --quiet. */
static void print_banner(const Interp *ip){
    const char *term = getenv("TERM");
    int color = isatty(fileno(stderr))
             && term && strcmp(term, "dumb") != 0
             && !getenv("NO_COLOR");
    const char *A = color ? "\x1b[38;5;24m" : "";   /* steel blue */
    const char *D = color ? "\x1b[2m"       : "";   /* dim */
    const char *R = color ? "\x1b[0m"       : "";
    fprintf(stderr, "\n");
    fprintf(stderr, "%s        (  (%s\n", A, R);
    fprintf(stderr, "%s         )  )%s      tea %s \u2014 tiny econometric assistant\n", A, R, TEA_VERSION);
    fprintf(stderr, "%s      .........%s    %sfree Stata-like data analysis at the command line%s\n", A, R, D, R);
    fprintf(stderr, "%s      |       |]%s   %sGPLv3 \u00b7 Mico Mrkaic \u00b7 github.com/micomrkaic/tea%s\n", A, R, D, R);
    fprintf(stderr, "%s      \\       /%s\n", A, R);
    fprintf(stderr, "%s       `-----\u00b4%s     %stype help \u00b7 sysuse dir for practice datasets \u00b7 Ctrl-D exits%s\n", A, R, D, R);
    if(!ip->strict_stata)
        fprintf(stderr, "                   %s(tea-extensions enabled)%s\n", D, R);
    fprintf(stderr, "\n");
}

extern int g_exit_requested;
extern int g_exit_code;

static void usage(FILE *out){
    fprintf(out,
"Usage: tea [options] [do-file]\n"
"\n"
"Options:\n"
"  --strict-stata      reject tea-only extensions (default)\n"
"  --tea-extensions    allow tea-only extensions\n"
"  -q, --quiet         suppress the startup banner\n"
"  --version           print version and exit\n"
"  --help              print this help and exit\n"
"\n"
"With no do-file, tea reads commands interactively.  Use '-' to read\n"
"do-file from standard input.\n");
}

int main(int argc,char **argv){
    int quiet = 0;
    /* Line-buffer stdout so that when stdout and stderr are merged
     * (e.g. tea foo.do 2>&1) the lines appear in real source order
     * instead of interleaving according to libc's internal buffers.
     * This also matches Stata's behavior of flushing per output line. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    Workspace *ws=ws_new();
    Interp *ip=interp_new(ws);
    int rc=0;

    /* parse leading -- options */
    int argi = 1;
    while(argi < argc &&
          ((argv[argi][0] == '-' && argv[argi][1] == '-') || !strcmp(argv[argi], "-q"))){
        const char *a = argv[argi];
        if(!strcmp(a, "-q") || !strcmp(a, "--quiet")){
            quiet = 1;
        } else if(!strcmp(a, "--strict-stata")){
            ip->strict_stata = true;
        } else if(!strcmp(a, "--tea-extensions")){
            ip->strict_stata = false;
        } else if(!strcmp(a, "--version")){
            printf("tea %s — tiny econometric assistant\n", TEA_VERSION);
            interp_free(ip); ws_free(ws); return 0;
        } else if(!strcmp(a, "--help")){
            usage(stdout);
            interp_free(ip); ws_free(ws); return 0;
        } else if(!strcmp(a, "--")){
            argi++; break;   /* end-of-options marker */
        } else {
            fprintf(stderr, "tea: unrecognized option '%s'\n", a);
            usage(stderr);
            interp_free(ip); ws_free(ws); return 2;
        }
        argi++;
    }

    if(argi < argc && strcmp(argv[argi],"-")!=0){
        FILE *f=fopen(argv[argi],"r");
        if(!f){ fprintf(stderr,"cannot open do-file %s\n",argv[argi]); return 1; }
        rc=run_stream(ip,f,false);
        fclose(f);
    } else if(argi < argc){
        /* explicit '-' — read do-file commands from stdin, non-interactive */
        rc=run_stream(ip,stdin,false);
    } else {
        if(!quiet) print_banner(ip);
        rc=run_stream(ip,stdin,true);
    }
    interp_free(ip);
    ws_free(ws);
    if(g_exit_requested) return g_exit_code;
    return rc;
}
