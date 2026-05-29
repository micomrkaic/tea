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

extern int g_exit_requested;
extern int g_exit_code;

static void usage(FILE *out){
    fprintf(out,
"Usage: tea [options] [do-file]\n"
"\n"
"Options:\n"
"  --strict-stata      reject tea-only extensions (default)\n"
"  --tea-extensions    allow tea-only extensions\n"
"  --version           print version and exit\n"
"  --help              print this help and exit\n"
"\n"
"With no do-file, tea reads commands interactively.  Use '-' to read\n"
"do-file from standard input.\n");
}

int main(int argc,char **argv){
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
    while(argi < argc && argv[argi][0] == '-' && argv[argi][1] == '-'){
        const char *a = argv[argi];
        if(!strcmp(a, "--strict-stata")){
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
        fprintf(stderr,"tea %s — tiny econometric assistant.  type 'help' or Ctrl-D to exit.\n",TEA_VERSION);
        if(!ip->strict_stata)
            fprintf(stderr,"(tea-extensions enabled)\n");
        rc=run_stream(ip,stdin,true);
    }
    interp_free(ip);
    ws_free(ws);
    if(g_exit_requested) return g_exit_code;
    return rc;
}
