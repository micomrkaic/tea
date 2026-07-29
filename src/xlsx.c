/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Native .xlsx reader: no ssconvert, no gnumeric, works identically on
 * native builds and in the browser (WASM).  An xlsx file is a ZIP of
 * XML parts; we need exactly four: xl/workbook.xml (sheet names),
 * xl/_rels/workbook.xml.rels (name -> part path), xl/sharedStrings.xml,
 * and the chosen worksheet.  Cells are emitted as CSV to a temp file so
 * the whole downstream (firstrow naming, case(), cellrange slicing)
 * is byte-identical with the old ssconvert path.
 *
 * Handled cell types: shared strings (t="s", including <r> rich runs),
 * literal strings (t="str": the CACHED VALUE of a formula), inline
 * strings (t="inlineStr"), booleans (t="b" -> 1/0), and numbers (no t).
 * Formula bodies (<f>) are ignored in favor of the cached <v> — the
 * file carries the last evaluated values.  XML entities are decoded.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "xlsx.h"

/* ---- little-endian readers -------------------------------------------- */
static unsigned rd16(const unsigned char *p){ return p[0] | (p[1]<<8); }
static unsigned long rd32(const unsigned char *p){
    return (unsigned long)p[0] | ((unsigned long)p[1]<<8)
         | ((unsigned long)p[2]<<16) | ((unsigned long)p[3]<<24);
}

/* ---- extract one member from the ZIP ---------------------------------- */
/* Returns malloc'd NUL-terminated buffer (out_len excludes the NUL), or
 * NULL if the member is absent.  Supports methods 0 (stored) and 8
 * (deflate), which covers every xlsx writer in practice. */
static unsigned char *zip_read(const unsigned char *zip, size_t zlen,
                               const char *member, size_t *out_len){
    if(zlen < 22) return NULL;
    /* find End Of Central Directory: scan back over a possible comment */
    long eocd = -1;
    size_t scan_from = zlen >= 22 + 65535 ? zlen - 22 - 65535 : 0;
    for(long i = (long)zlen - 22; i >= (long)scan_from; i--){
        if(zip[i]==0x50 && zip[i+1]==0x4b && zip[i+2]==0x05 && zip[i+3]==0x06){
            eocd = i; break; }
    }
    if(eocd < 0) return NULL;
    unsigned long nent = rd16(zip+eocd+10);
    unsigned long cdof = rd32(zip+eocd+16);
    size_t p = cdof;
    for(unsigned long e = 0; e < nent; e++){
        if(p + 46 > zlen || rd32(zip+p) != 0x02014b50ul) return NULL;
        unsigned method   = rd16(zip+p+10);
        unsigned long csz = rd32(zip+p+20);
        unsigned long usz = rd32(zip+p+24);
        unsigned nlen     = rd16(zip+p+28);
        unsigned xlen     = rd16(zip+p+30);
        unsigned clen     = rd16(zip+p+32);
        unsigned long lho = rd32(zip+p+42);
        if(nlen == strlen(member) && !memcmp(zip+p+46, member, nlen)){
            /* local header: name/extra lengths there can differ from CD */
            if(lho + 30 > zlen || rd32(zip+lho) != 0x04034b50ul) return NULL;
            unsigned lnl = rd16(zip+lho+26), lxl = rd16(zip+lho+28);
            size_t doff = lho + 30 + lnl + lxl;
            if(doff + csz > zlen) return NULL;
            unsigned char *out = malloc(usz + 1);
            if(!out) return NULL;
            if(method == 0){
                if(csz != usz){ free(out); return NULL; }
                memcpy(out, zip+doff, usz);
            } else if(method == 8){
                z_stream zs; memset(&zs, 0, sizeof zs);
                if(inflateInit2(&zs, -15) != Z_OK){ free(out); return NULL; }
                zs.next_in  = (unsigned char*)(zip+doff); zs.avail_in  = (unsigned)csz;
                zs.next_out = out;                        zs.avail_out = (unsigned)usz;
                int zrc = inflate(&zs, Z_FINISH);
                inflateEnd(&zs);
                if(zrc != Z_STREAM_END){ free(out); return NULL; }
            } else { free(out); return NULL; }
            out[usz] = 0;
            if(out_len) *out_len = usz;
            return out;
        }
        p += 46 + nlen + xlen + clen;
    }
    return NULL;
}

/* ---- tiny XML helpers -------------------------------------------------- */
/* find next occurrence of tag start "<name" at or after p (NUL-safe on
 * our NUL-terminated buffers; xlsx XML parts contain no interior NULs) */
static const char *xfind(const char *p, const char *tag){
    return p ? strstr(p, tag) : NULL;
}
/* attribute value inside the current tag (before '>'); returns malloc'd
 * copy or NULL */
static char *xattr(const char *tag_start, const char *name){
    const char *gt = strchr(tag_start, '>');
    if(!gt) return NULL;
    char pat[64]; snprintf(pat, sizeof pat, " %s=\"", name);
    const char *a = strstr(tag_start, pat);
    if(!a || a > gt) return NULL;
    a += strlen(pat);
    const char *e = strchr(a, '"');
    if(!e || e > gt) return NULL;
    char *out = malloc((size_t)(e-a)+1);
    if(!out) return NULL;
    memcpy(out, a, (size_t)(e-a)); out[e-a] = 0;
    return out;
}
/* decode XML entities in place */
static void xml_unescape(char *s){
    char *w = s;
    while(*s){
        if(*s == '&'){
            if(!strncmp(s,"&amp;",5)){ *w++='&'; s+=5; continue; }
            if(!strncmp(s,"&lt;",4)) { *w++='<'; s+=4; continue; }
            if(!strncmp(s,"&gt;",4)) { *w++='>'; s+=4; continue; }
            if(!strncmp(s,"&quot;",6)){ *w++='"'; s+=6; continue; }
            if(!strncmp(s,"&apos;",6)){ *w++='\''; s+=6; continue; }
            if(s[1]=='#'){
                long code = strtol(s+2, (char**)&s, s[2]=='x'||s[2]=='X' ? 16 : 10);
                if(*s==';') s++;
                if(code > 0 && code < 128) *w++ = (char)code;  /* ASCII */
                else { /* UTF-8 encode BMP code points */
                    if(code < 0x800){ *w++=(char)(0xC0|(code>>6)); *w++=(char)(0x80|(code&0x3F)); }
                    else if(code <= 0xFFFF){ *w++=(char)(0xE0|(code>>12)); *w++=(char)(0x80|((code>>6)&0x3F)); *w++=(char)(0x80|(code&0x3F)); }
                }
                continue;
            }
        }
        *w++ = *s++;
    }
    *w = 0;
}
/* concatenated text of all <t>...</t> inside [p, close_tag) — handles
 * plain <si><t> and rich-run <si><r><t>..</t></r><r>.. */
static char *xtext_all(const char *p, const char *end){
    size_t cap = 64, len = 0;
    char *out = malloc(cap); if(!out) return NULL; out[0]=0;
    const char *t = p;
    while((t = strstr(t, "<t")) && t < end){
        const char *tag_end = strchr(t, '>');
        if(!tag_end || tag_end > end) break;
        if(t[2] != '>' && t[2] != ' ' && t[2] != '/'){ t = tag_end; continue; }
        if(tag_end[-1] == '/'){ t = tag_end; continue; }       /* <t/> */
        const char *close = strstr(tag_end, "</t>");
        if(!close || close > end) break;
        size_t n = (size_t)(close - tag_end - 1);
        if(len + n + 1 > cap){ while(len+n+1>cap) cap*=2; out = realloc(out, cap); if(!out) return NULL; }
        memcpy(out+len, tag_end+1, n); len += n; out[len]=0;
        t = close + 4;
    }
    xml_unescape(out);
    return out;
}

/* ---- shared strings table --------------------------------------------- */
typedef struct { char **s; size_t n; } SST;
static void sst_load(const unsigned char *zip, size_t zlen, SST *sst){
    sst->s = NULL; sst->n = 0;
    size_t xl; unsigned char *xml = zip_read(zip, zlen, "xl/sharedStrings.xml", &xl);
    if(!xml) return;
    size_t cap = 64; sst->s = malloc(cap * sizeof(char*));
    const char *p = (const char*)xml;
    while((p = xfind(p, "<si"))){
        if(p[3] != '>' && p[3] != ' '){ p += 3; continue; }
        const char *close = strstr(p, "</si>");
        if(!close) close = (const char*)xml + xl;
        if(sst->n == cap){ cap *= 2; sst->s = realloc(sst->s, cap * sizeof(char*)); }
        sst->s[sst->n++] = xtext_all(p, close);
        p = close;
    }
    free(xml);
}
static void sst_free(SST *sst){
    for(size_t i = 0; i < sst->n; i++) free(sst->s[i]);
    free(sst->s);
}

/* ---- resolve a sheet name to its part path ---------------------------- */
/* empty wanted_name = first sheet.  Returns malloc'd "xl/worksheets/.." */
static char *sheet_part(const unsigned char *zip, size_t zlen,
                        const char *wanted_name){
    unsigned char *wb = zip_read(zip, zlen, "xl/workbook.xml", NULL);
    if(!wb) return NULL;
    char *rid = NULL;
    const char *p = (const char*)wb;
    while((p = xfind(p, "<sheet "))){
        char *nm = xattr(p, "name");
        if(nm) xml_unescape(nm);
        if(!wanted_name[0] || (nm && !strcmp(nm, wanted_name))){
            rid = xattr(p, "r:id");
            free(nm);
            break;
        }
        free(nm);
        p += 7;
    }
    free(wb);
    if(!rid) return NULL;
    unsigned char *rels = zip_read(zip, zlen, "xl/_rels/workbook.xml.rels", NULL);
    char *part = NULL;
    if(rels){
        const char *q = (const char*)rels;
        while((q = xfind(q, "<Relationship "))){
            char *id = xattr(q, "Id");
            if(id && !strcmp(id, rid)){
                char *tg = xattr(q, "Target");
                free(id);
                if(tg){
                    if(!strncmp(tg, "/xl/", 4)){        /* absolute */
                        part = strdup(tg+1);
                    } else {
                        part = malloc(strlen(tg)+4);
                        if(part){ strcpy(part, "xl/"); strcat(part, tg); }
                    }
                    free(tg);
                }
                break;
            }
            free(id);
            q += 14;
        }
        free(rels);
    }
    free(rid);
    return part;
}

/* ---- A1 column letters -> 0-based index ------------------------------- */
static int col_of_ref(const char *r){
    int c = 0;
    for(; *r >= 'A' && *r <= 'Z'; r++) c = c*26 + (*r - 'A' + 1);
    return c - 1;
}

/* forward decl from commands.c: CSV cell writer with RFC 4180 quoting */
void csv_quote_write_pub(FILE *fp, const char *s, char delim);

/* ---- the reader -------------------------------------------------------- */
int xlsx_to_csv(const char *xlsx_path, const char *sheet_name,
                const char *csv_path){
    FILE *f = fopen(xlsx_path, "rb");
    if(!f) return 601;
    fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
    if(fl <= 0){ fclose(f); return 601; }
    unsigned char *zip = malloc((size_t)fl);
    if(!zip || fread(zip, 1, (size_t)fl, f) != (size_t)fl){
        free(zip); fclose(f); return 601; }
    fclose(f);

    char *part = sheet_part(zip, (size_t)fl, sheet_name ? sheet_name : "");
    if(!part){ free(zip); return 601; }          /* no such sheet */
    unsigned char *sx = zip_read(zip, (size_t)fl, part, NULL);
    free(part);
    if(!sx){ free(zip); return 601; }

    SST sst; sst_load(zip, (size_t)fl, &sst);
    free(zip);

    FILE *out = fopen(csv_path, "wb");
    if(!out){ free(sx); sst_free(&sst); return 603; }

    const char *p = (const char*)sx;
    while((p = xfind(p, "<row"))){
        if(p[4] != '>' && p[4] != ' '){ p += 4; continue; }
        const char *row_end = strstr(p, "</row>");
        const char *self_end = strchr(p, '>');
        int self_closed = self_end && self_end[-1] == '/';
        if(self_closed){ fputc('\n', out); p = self_end; continue; }
        if(!row_end) row_end = (const char*)sx + strlen((const char*)sx);
        int col = 0;
        const char *c = p;
        while((c = xfind(c, "<c ")) && c < row_end){
            const char *cend = strchr(c, '>');
            if(!cend || cend > row_end) break;
            char *ref = xattr(c, "r");
            int  tcol = ref ? col_of_ref(ref) : col;
            free(ref);
            for(; col < tcol; col++) fputc(',', out);   /* gap cells */
            if(col > 0 && tcol == 0){ /* malformed; ignore */ }
            char *ty = xattr(c, "t");
            char *val = NULL;
            int cell_closed = cend[-1] == '/';
            const char *cclose = cell_closed ? cend : strstr(cend, "</c>");
            if(!cclose || cclose > row_end) cclose = row_end;
            if(!cell_closed){
                if(ty && !strcmp(ty, "inlineStr")){
                    val = xtext_all(cend, cclose);
                } else {
                    const char *v = strstr(cend, "<v>");
                    if(v && v < cclose){
                        const char *ve = strstr(v, "</v>");
                        if(ve && ve <= cclose){
                            size_t n = (size_t)(ve - v - 3);
                            val = malloc(n+1);
                            if(val){ memcpy(val, v+3, n); val[n]=0; xml_unescape(val); }
                        }
                    }
                }
            }
            const char *text = "";
            char *resolved = NULL;
            if(val){
                if(ty && !strcmp(ty, "s")){
                    long idx = strtol(val, NULL, 10);
                    if(idx >= 0 && (size_t)idx < sst.n && sst.s[idx]) text = sst.s[idx];
                } else if(ty && !strcmp(ty, "b")){
                    text = strcmp(val, "0") ? "1" : "0";
                } else {
                    text = val;    /* number, or t="str" cached formula value */
                }
            }
            (void)resolved;
            if(col > 0) { /* separator handled by gap loop + here */ }
            if(tcol > 0 && col == tcol) { /* at target col */ }
            csv_quote_write_pub(out, text, ',');
            col = tcol + 1;
            free(val); free(ty);
            c = cclose;
            /* peek: another cell in this row? then separator */
            const char *nx = xfind(c, "<c ");
            if(nx && nx < row_end) fputc(',', out);
        }
        fputc('\n', out);
        p = row_end;
    }
    fclose(out);
    free(sx);
    sst_free(&sst);
    return 0;
}
