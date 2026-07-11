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
#include "expr.h"
#include "value.h"
#include <stdio.h>
#include "lex.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================ MODULE
 *
 * parse.c — recursive-descent parser for tea's expression language.
 *
 * Turns a token stream (from lex.h) into an AST.  The grammar follows
 * Stata's operator precedence:
 *
 *   or       :  and ('|' and)*
 *   and      :  rel ('&' rel)*
 *   rel      :  add (('==' | '!=' | '<' | '<=' | '>' | '>=') add)*
 *   add      :  mul (('+' | '-') mul)*
 *   mul      :  pow (('*' | '/') pow)*
 *   pow      :  unary ('^' unary)*
 *   unary    :  ('-' | '!') unary | primary
 *   primary  :  number
 *            |  string
 *            |  variable (with optional TS-op prefix like L. or D2.)
 *            |  variable '[' expr ']'     (subscript)
 *            |  identifier '(' args ')'   (function call)
 *            |  '(' expr ')'
 *
 * The TS-op prefix (parse_ts_prefix) handles tokens like `L.x`, `L2.x`,
 * `D2.x`, `S.x` by stripping the prefix and recording its kind+lag in
 * the AST node, so the evaluator can resolve via the panel index.
 *
 * Error handling: on the first syntax error, p->err is set to a static
 * message and parsing returns NULL.  Callers check the parser's err
 * after node_new and skip evaluation if non-NULL.
 *
 * ==================================================================== */

typedef struct { Lexer L; Frame *f; const char *err; } P;

static Node *p_or(P *p);

static Node *node_new(NodeKind k) {
    Node *n = calloc(1, sizeof *n); n->kind = k; return n;
}
void node_free(Node *n) {
    if (!n) return;
    node_free(n->a); node_free(n->b); node_free(n->next);
    free(n);
}

/* Parse a possibly-chained TS operator prefix like L2D. or S12.  Returns the
 * remaining identifier (the variable) and fills tskind/tslag. We support a
 * single primary operator with optional numeric order (Stata also allows
 * operator lists; v1 keeps the common single/compound L#/F#/D#/S# forms). */
static int try_tsop(const char *id, char *kind, int *lag) {
    /* id looks like  L  L2  D  F3  S  S2 ; bare letter only if followed by . */
    if (!id[0]) return 0;
    char k = toupper((unsigned char)id[0]);
    if (k!='L' && k!='F' && k!='D' && k!='S') return 0;
    const char *q = id + 1;
    int num = 0, have = 0;
    while (isdigit((unsigned char)*q)) { num = num*10 + (*q-'0'); q++; have=1; }
    if (*q != 0) return 0;            /* trailing junk -> not a TS op */
    if (!have) num = 1;               /* bare 'L' / 'F' / 'D' / 'S' -> 1 */
    *kind = k;
    *lag  = (k=='F') ? +num : -num;   /* D/S also use magnitude; sign by kind */
    return 1;
}

static Node *p_primary(P *p) {
    Tok t = lex_next(&p->L);
    if (t.kind == T_NUM) { Node *n=node_new(N_NUM); n->num=t.num; return n; }
    if (t.kind == T_STR) { Node *n=node_new(N_STR); strcpy(n->text,t.text); return n; }
    if (t.kind == T_LP) {
        Node *e = p_or(p);
        Tok r = lex_next(&p->L);
        if (r.kind != T_RP) p->err = "expected )";
        return e;
    }
    if (t.kind == T_MINUS) { Node *n=node_new(N_UNARY); n->op=T_MINUS; n->a=p_primary(p); return n; }
    if (t.kind == T_PLUS)  { return p_primary(p); }
    if (t.kind == T_NOT)   { Node *n=node_new(N_UNARY); n->op=T_NOT; n->a=p_primary(p); return n; }

    if (t.kind == T_DOT) {   /* missing literal: . or .a .. .z */
        Node *n=node_new(N_NUM);
        Tok pk=lex_peek(&p->L);
        if (pk.kind==T_ID && pk.text[0]>='a' && pk.text[0]<='z' && pk.text[1]==0){
            lex_next(&p->L);
            n->num = sv_miss(pk.text[0]-'a'+1);
        } else n->num = sv_miss(0);
        return n;
    }

    if (t.kind == T_ID) {
        /* function call? */
        if (lex_peek(&p->L).kind == T_LP) {
            lex_next(&p->L);
            Node *c = node_new(N_CALL);
            snprintf(c->text, sizeof c->text, "%s", t.text);
            /* date-literal constructors take a raw token, not an expression */
            int tlit = (!strcmp(t.text,"td")||!strcmp(t.text,"tm")||
                        !strcmp(t.text,"tq")||!strcmp(t.text,"tw")||
                        !strcmp(t.text,"th")||!strcmp(t.text,"ty"));
            if (tlit) {
                const char *q = p->L.p;          /* raw source after '(' */
                const char *st = q; int d = 1;
                while (*q && d) { if(*q=='(')d++; else if(*q==')'){d--; if(!d)break;} q++; }
                Node *arg = node_new(N_STR);
                int len = (int)(q-st); if(len>255)len=255;
                snprintf(arg->text, sizeof arg->text, "%.*s", len, st);
                /* trim spaces */
                for(int z=(int)strlen(arg->text)-1;z>=0&&arg->text[z]==' ';z--)arg->text[z]=0;
                c->a = arg;
                p->L.p = (*q==')') ? q+1 : q;    /* consume past ')' */
                p->L.has_peek = 0;
                return c;
            }
            Node **tail = &c->a;
            if (lex_peek(&p->L).kind != T_RP) {
                for (;;) {
                    Node *arg = p_or(p);
                    *tail = arg; tail = &arg->next;
                    if (lex_peek(&p->L).kind == T_COMMA) { lex_next(&p->L); continue; }
                    break;
                }
            }
            Tok r = lex_next(&p->L);
            if (r.kind != T_RP) p->err = "expected ) in call";
            return c;
        }
        /* time-series operator:  L.x  L2.x  D.x  F.x  S2.x */
        char tk; int lag;
        if (lex_peek(&p->L).kind == T_DOT && try_tsop(t.text, &tk, &lag)) {
            lex_next(&p->L);                 /* consume '.' */
            Tok vt = lex_next(&p->L);
            if (vt.kind != T_ID) { p->err = "expected variable after TS operator"; return node_new(N_NUM); }
            Node *n = node_new(N_TSOP);
            n->tskind = tk; n->tslag = lag;
            snprintf(n->text, sizeof n->text, "%s", vt.text);
            return n;
        }
        /* plain variable, with optional [subscript] */
        Node *v = node_new(N_VAR);
        snprintf(v->text, sizeof v->text, "%s", t.text);
        if (lex_peek(&p->L).kind == T_LBRACK) {
            lex_next(&p->L);
            v->a = p_or(p);
            Tok r = lex_next(&p->L);
            if (r.kind != T_RBRACK) p->err = "expected ]";
        }
        return v;
    }
    p->err = "expected expression";
    return node_new(N_NUM);
}

/* ---- Precedence chain ------------------------------------------------
 *
 * Each level handles one rung of the precedence ladder: p_pow at the
 * tightest binding (right-associative), then p_mul, p_add, p_cmp, p_and,
 * p_or at the loosest.  The pattern is uniform: parse a higher-level
 * subexpression on the left, then while the next token is one of the
 * operators for *this* level, consume it and parse another higher-level
 * subexpression on the right.  Left-associative for everything except
 * exponentiation.
 *
 * The whole expression entry point is p_or, the loosest binding.
 * --------------------------------------------------------------------- */

static Node *p_pow(P *p) {
    Node *l = p_primary(p);
    if (lex_peek(&p->L).kind == T_CARET) {
        lex_next(&p->L);
        Node *n = node_new(N_BINARY); n->op=T_CARET; n->a=l; n->b=p_pow(p);
        return n;
    }
    return l;
}
static Node *p_mul(P *p) {
    Node *l = p_pow(p);
    for (;;) {
        TokKind k = lex_peek(&p->L).kind;
        if (k!=T_STAR && k!=T_SLASH) break;
        lex_next(&p->L);
        Node *n=node_new(N_BINARY); n->op=k; n->a=l; n->b=p_pow(p); l=n;
    }
    return l;
}
static Node *p_add(P *p) {
    Node *l = p_mul(p);
    for (;;) {
        TokKind k = lex_peek(&p->L).kind;
        if (k!=T_PLUS && k!=T_MINUS) break;
        lex_next(&p->L);
        Node *n=node_new(N_BINARY); n->op=k; n->a=l; n->b=p_mul(p); l=n;
    }
    return l;
}
static Node *p_cmp(P *p) {
    Node *l = p_add(p);
    for (;;) {
        TokKind k = lex_peek(&p->L).kind;
        if (k!=T_LT&&k!=T_LE&&k!=T_GT&&k!=T_GE&&k!=T_EQEQ&&k!=T_NE&&k!=T_EQ) break;
        lex_next(&p->L);
        Node *n=node_new(N_BINARY); n->op=(k==T_EQ?T_EQEQ:k); n->a=l; n->b=p_add(p); l=n;
    }
    return l;
}
static Node *p_and(P *p) {
    Node *l = p_cmp(p);
    while (lex_peek(&p->L).kind == T_AND) {
        lex_next(&p->L);
        Node *n=node_new(N_BINARY); n->op=T_AND; n->a=l; n->b=p_cmp(p); l=n;
    }
    return l;
}
static Node *p_or(P *p) {
    Node *l = p_and(p);
    while (lex_peek(&p->L).kind == T_OR) {
        lex_next(&p->L);
        Node *n=node_new(N_BINARY); n->op=T_OR; n->a=l; n->b=p_and(p); l=n;
    }
    return l;
}


/* ---- parse-time variable validation -------------------------------------
 * Stata errors r(111) the moment an expression names a variable that does
 * not exist; it never evaluates.  tea used to resolve unknown names to
 * missing at eval time (with the error flag set but ignored by callers),
 * which meant a typo in `keep if` could silently destroy the dataset.
 * Validate the whole AST here so every expression consumer is protected. */
static const char *g_valerr_buf(void){
    static char buf[128]; return buf;
}
static int validate_vars(Node *n, Frame *f, const char **err){
    if(!n) return 1;
    if(n->kind == N_VAR || n->kind == N_TSOP){
        if(strcmp(n->text,"_n") && strcmp(n->text,"_N") &&
           strcmp(n->text,"_pi") && strcmp(n->text,"_rc") &&
           var_find(f, n->text) < 0){
            char *buf = (char*)g_valerr_buf();
            snprintf(buf, 128, "variable %s not found", n->text);
            *err = buf;
            return 0;
        }
    }
    return validate_vars(n->a, f, err)
        && validate_vars(n->b, f, err)
        && validate_vars(n->next, f, err);
}

Node *expr_parse(const char *src, Frame *f, const char **err) {
    P p; lex_init(&p.L, src); p.f=f; p.err=NULL;
    Node *n = p_or(&p);
    if (!p.err && lex_peek(&p.L).kind != T_EOF) p.err = "trailing tokens in expression";
    if (!p.err && p.L.err) p.err = p.L.err;
    if (p.err) { *err = p.err; node_free(n); return NULL; }
    if (f && !validate_vars(n, f, err)) { node_free(n); return NULL; }
    *err = NULL;
    return n;
}
