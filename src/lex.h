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
/* lex.h — tokenizer shared by command parser and expression parser. */
#ifndef PSTATA_LEX_H
#define PSTATA_LEX_H

typedef enum {
    T_EOF, T_NUM, T_STR, T_ID,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_CARET,
    T_LP, T_RP, T_COMMA, T_DOT,
    T_EQ, T_EQEQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_AND, T_OR, T_NOT,
    T_LBRACK, T_RBRACK
} TokKind;

typedef struct {
    TokKind kind;
    double  num;
    char    text[256];   /* identifier / string literal contents */
} Tok;

typedef struct {
    const char *p;
    Tok cur;
    Tok peeked;
    int has_peek;
    const char *err;
} Lexer;

void lex_init(Lexer *L, const char *src);
Tok  lex_next(Lexer *L);
Tok  lex_peek(Lexer *L);

#endif
