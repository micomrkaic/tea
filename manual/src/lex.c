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
#include "lex.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

void lex_init(Lexer *L, const char *src) {
    L->p = src; L->has_peek = 0; L->err = NULL;
}

static Tok scan(Lexer *L) {
    Tok t; memset(&t, 0, sizeof t);
    const char *p = L->p;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0) { t.kind = T_EOF; L->p = p; return t; }

    /* string literal: "..."  or compound `"..."' (Stata) */
    if (*p == '"' || (p[0] == '`' && p[1] == '"')) {
        bool compound = (*p == '`');
        p += compound ? 2 : 1;
        int n = 0;
        while (*p && !(compound ? (p[0] == '"' && p[1] == '\'')
                                 : (*p == '"'))) {
            if (n < 255) t.text[n++] = *p;
            p++;
        }
        if (*p) p += compound ? 2 : 1;
        t.text[n] = 0; t.kind = T_STR; L->p = p; return t;
    }

    if (isdigit((unsigned char)*p) ||
        (*p == '.' && isdigit((unsigned char)p[1]))) {
        char *end;
        t.num = strtod(p, &end);
        t.kind = T_NUM; L->p = end; return t;
    }

    if (isalpha((unsigned char)*p) || *p == '_') {
        int n = 0;
        while (isalnum((unsigned char)*p) || *p == '_') {
            if (n < 255) t.text[n++] = *p;
            p++;
        }
        t.text[n] = 0;
        if      (!strcmp(t.text, "and") ) t.kind = T_AND;
        else if (!strcmp(t.text, "or")  ) t.kind = T_OR;
        else                              t.kind = T_ID;
        L->p = p; return t;
    }

    switch (*p) {
        case '+': t.kind = T_PLUS;  p++; break;
        case '-': t.kind = T_MINUS; p++; break;
        case '*': t.kind = T_STAR;  p++; break;
        case '/': t.kind = T_SLASH; p++; break;
        case '^': t.kind = T_CARET; p++; break;
        case '(': t.kind = T_LP;    p++; break;
        case ')': t.kind = T_RP;    p++; break;
        case '[': t.kind = T_LBRACK;p++; break;
        case ']': t.kind = T_RBRACK;p++; break;
        case ',': t.kind = T_COMMA; p++; break;
        case '.': t.kind = T_DOT;   p++; break;
        case '&': t.kind = T_AND;   p++; break;
        case '|': t.kind = T_OR;    p++; break;
        case '~': if (p[1]=='='){t.kind=T_NE;p+=2;} else {t.kind=T_NOT;p++;} break;
        case '!': if (p[1]=='='){t.kind=T_NE;p+=2;} else {t.kind=T_NOT;p++;} break;
        case '=': if (p[1]=='='){t.kind=T_EQEQ;p+=2;} else {t.kind=T_EQ;p++;} break;
        case '<': if (p[1]=='='){t.kind=T_LE;p+=2;}
                  else if(p[1]=='>'){t.kind=T_NE;p+=2;}
                  else {t.kind=T_LT;p++;} break;
        case '>': if (p[1]=='='){t.kind=T_GE;p+=2;} else {t.kind=T_GT;p++;} break;
        default:  L->err = "unexpected character"; t.kind = T_EOF; p++; break;
    }
    L->p = p; return t;
}

Tok lex_next(Lexer *L) {
    if (L->has_peek) { L->has_peek = 0; L->cur = L->peeked; return L->cur; }
    L->cur = scan(L); return L->cur;
}

Tok lex_peek(Lexer *L) {
    if (!L->has_peek) { L->peeked = scan(L); L->has_peek = 1; }
    return L->peeked;
}
