/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * factor.c — Implementation of Stata-style factor-variable expansion.
 *
 * See factor.h for the grammar.  Three phases:
 *
 *   1. Parse the token into a list of "terms".  Each term is a list of
 *      "atoms" joined by '#'.  `##` is unrolled into multiple terms:
 *      A##B → {A, B, A#B}; A##B##C → {A, B, C, A#B, A#C, B#C, A#B#C}.
 *
 *   2. For each term, enumerate the column space:
 *      - For each `i.` atom: collect the sorted unique non-missing levels
 *        of its variable.  Pick the base level (smallest unless `ib<n>.`).
 *      - For each `c.` atom: a single "slot" (no enumeration).
 *      - The cross-product of (non-base levels of each i. atom) gives
 *        the set of cells to materialise.  If any non-base level is
 *        present for ALL i. atoms in the term, that cell becomes one
 *        output column.
 *
 *   3. For each cell: compute its values over all rows.  A cell's value
 *      at row r is the product of:
 *      - For each `i.` atom in the term: 1 if var[r] == level, else 0.
 *      - For each `c.` atom in the term: the value of var[r].
 *      Missing values propagate.  Append the column with a canonical
 *      display name.
 */
#define _GNU_SOURCE
#include "factor.h"
#include "value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>

/* ---- token detection -------------------------------------------------- */

bool factor_is_factor_token(const char *tok)
{
    if(!tok || !*tok) return false;
    /* Quick win: contains '#' anywhere */
    if(strchr(tok, '#')) return true;
    /* Otherwise must start with i., c., or ib<digits>. */
    if(tok[0] == 'i' && tok[1] == '.') return true;
    if(tok[0] == 'c' && tok[1] == '.') return true;
    if(tok[0] == 'i' && tok[1] == 'b' && isdigit((unsigned char)tok[2])){
        const char *p = tok + 2;
        while(isdigit((unsigned char)*p)) p++;
        if(*p == '.') return true;
    }
    /* Coefficient-name form: <integer>.<varname> — a single-column
     * indicator that appears in coefficient tables and is what
     * predict/margins need to re-materialize.  Examples: "2.country",
     * "2010.year".  We require: starts with digit, contains '.' before
     * any non-digit, and the part after '.' is a valid identifier. */
    if(isdigit((unsigned char)tok[0])){
        const char *p = tok;
        while(isdigit((unsigned char)*p)) p++;
        if(*p == '.' && (isalpha((unsigned char)p[1]) || p[1] == '_')) return true;
    }
    return false;
}

/* ---- atom and term data types ----------------------------------------- */

typedef struct {
    char kind;            /* 'I' = factor expansion, 'C' = continuous,
                           * 'L' = single fixed level indicator (coef-name
                           *       form: "2.g" — no base dropping, just
                           *       a 1/0 indicator for the specified level) */
    char varname[33];
    long base_level;      /* 'I': base level (LONG_MIN = smallest);
                           * 'L': the pinned level for the indicator. */
    int  var_idx;         /* resolved column index in the frame */
} Atom;

#define MAX_ATOMS_PER_TERM 8

typedef struct {
    Atom atoms[MAX_ATOMS_PER_TERM];
    int  n_atoms;
} Term;

/* Parse a single atom from `*p`.  Returns 0 ok, -1 error.  Advances *p
 * past the atom.  Allowed forms: i.var, ib<n>.var, c.var. */
static int parse_atom(const char **p, Atom *out, const char **err)
{
    const char *s = *p;
    memset(out, 0, sizeof *out);
    out->base_level = (long)0x8000000000000000LL;  /* LONG_MIN sentinel */

    if(s[0] == 'i' && s[1] == '.'){
        out->kind = 'I';
        s += 2;
    } else if(s[0] == 'c' && s[1] == '.'){
        out->kind = 'C';
        s += 2;
    } else if(s[0] == 'i' && s[1] == 'b' && isdigit((unsigned char)s[2])){
        out->kind = 'I';
        s += 2;
        long b = 0;
        while(isdigit((unsigned char)*s)){ b = b*10 + (*s - '0'); s++; }
        if(*s != '.'){ *err = "expected '.' after ib<n>"; return -1; }
        s++;
        out->base_level = b;
    } else if(isdigit((unsigned char)s[0])){
        /* Coefficient-name form: <integer>.<varname>.  Single-column
         * indicator (1 if var == integer, 0 otherwise) — no base
         * dropping.  Used by predict/margins to re-materialize a
         * specific dummy column whose name was stored in e->xnames. */
        long b = 0;
        while(isdigit((unsigned char)*s)){ b = b*10 + (*s - '0'); s++; }
        if(*s != '.'){ *err = "expected '.' in coefficient-name token"; return -1; }
        s++;
        out->kind = 'L';
        out->base_level = b;   /* repurposed: the pinned level */
    } else {
        /* Plain variable name inside a #-interaction is treated as
         * continuous (consistent with how Stata interprets bare names
         * inside #).  This lets `c.gdp#c.pop` and `gdp#c.pop` both work. */
        out->kind = 'C';
    }
    /* Read variable name */
    int n = 0;
    while(*s && (isalnum((unsigned char)*s) || *s == '_') && n < 32){
        out->varname[n++] = *s++;
    }
    out->varname[n] = 0;
    if(n == 0){ *err = "missing variable name in factor atom"; return -1; }
    *p = s;
    return 0;
}

/* Parse the full token into one or more terms.  Returns 0 ok, -1 error.
 * Fills terms[0..*n_terms-1].  `##` produces a power-set of products
 * (main effects + all higher-order interactions). */
static int parse_token(const char *token, Term *terms, int *n_terms, const char **err)
{
    *n_terms = 0;
    /* Parse a sequence of atoms separated by '#' or '##'.  We collect
     * them into a flat list with markers indicating whether the
     * preceding '#' was a `##`. */
    Atom flat[MAX_ATOMS_PER_TERM];
    bool double_hash[MAX_ATOMS_PER_TERM] = {false};   /* d_h[i] = true if separator BEFORE atom i was ## */
    int n_flat = 0;

    const char *p = token;
    while(*p == ' ' || *p == '\t') p++;
    while(*p){
        if(n_flat >= MAX_ATOMS_PER_TERM){ *err = "too many atoms in factor term"; return -1; }
        Atom a;
        if(parse_atom(&p, &a, err) < 0) return -1;
        flat[n_flat] = a;
        n_flat++;
        if(!*p) break;
        /* Expect # or ## */
        if(*p == '#'){
            bool is_double = (p[1] == '#');
            p += is_double ? 2 : 1;
            if(n_flat < MAX_ATOMS_PER_TERM)
                double_hash[n_flat] = is_double;
        } else {
            *err = "expected '#' between factor atoms";
            return -1;
        }
    }
    if(n_flat < 1){ *err = "empty factor token"; return -1; }

    /* Single-atom case: one term. */
    if(n_flat == 1){
        terms[0].atoms[0] = flat[0];
        terms[0].n_atoms = 1;
        *n_terms = 1;
        return 0;
    }

    /* Multi-atom case.  If NO `##` markers are present, it's just one
     * term joining all atoms with '#'.  If any `##` marker is present,
     * we generate all 2^n - 1 non-empty subsets (effectively the power
     * set excluding the empty set), since `A##B##C` means main effects +
     * all pairwise + triple. */
    bool any_double = false;
    for(int i = 1; i < n_flat; i++) if(double_hash[i]){ any_double = true; break; }

    if(!any_double){
        terms[0].n_atoms = n_flat;
        for(int i = 0; i < n_flat; i++) terms[0].atoms[i] = flat[i];
        *n_terms = 1;
        return 0;
    }

    /* Power-set unroll.  For n_flat atoms, generate all non-empty
     * subsets in canonical "ascending bitmask" order, which matches
     * Stata's reporting order: main effects first (single-bit subsets),
     * then pairs, etc.
     *
     * Sub-optimization: only treat # as a "subset boundary" if the
     * preceding atom has a `##` marker; otherwise both atoms are forced
     * to appear together.  This handles mixed cases like A#B##C
     * (A#B is a single unit; then ##C means A#B and C and A#B#C).
     *
     * For simplicity in v1.0 we treat every # as flexible when at least
     * one ## is present; this is the most common case (A##B##C) and
     * users wanting the mixed semantics can write them explicitly.
     * (Stata's exact behaviour with mixed # and ## is complex.)
     */
    int n_subsets = (1 << n_flat) - 1;
    if(n_subsets > 32){ *err = "factor expansion too large"; return -1; }
    /* Iterate by popcount then bitmask, so order is main / 2-way / 3-way. */
    int term_idx = 0;
    for(int order = 1; order <= n_flat; order++){
        for(int mask = 1; mask < (1 << n_flat); mask++){
            int pop = 0;
            for(int i = 0; i < n_flat; i++) if(mask & (1<<i)) pop++;
            if(pop != order) continue;
            int na = 0;
            for(int i = 0; i < n_flat; i++){
                if(mask & (1<<i)){
                    if(na >= MAX_ATOMS_PER_TERM){ *err = "too many atoms"; return -1; }
                    terms[term_idx].atoms[na++] = flat[i];
                }
            }
            terms[term_idx].n_atoms = na;
            term_idx++;
        }
    }
    *n_terms = term_idx;
    return 0;
}

/* ---- column materialisation ------------------------------------------- */

/* For each 'I' atom in a term, discover the sorted unique non-missing
 * levels of its variable, and identify which level is the base.  Stores
 * results in atom_levels[a] (sorted asc) and atom_base_idx[a] (index of
 * base in atom_levels[a], or -1 if base is not present).
 *
 * Returns 0 ok, -1 error (with *err set). */
static int discover_levels(Frame *f, Term *t,
                           double **atom_levels, int *atom_n_levels,
                           int *atom_base_idx,
                           const char **err)
{
    for(int a = 0; a < t->n_atoms; a++){
        atom_levels[a] = NULL;
        atom_n_levels[a] = 0;
        atom_base_idx[a] = -1;
        if(t->atoms[a].kind != 'I') continue;
        Variable *v = &f->vars[t->atoms[a].var_idx];
        if(v->type != VT_NUM){
            static char buf[128];
            snprintf(buf, sizeof buf,
                "factor variable i.%s must be numeric (use encode to convert)",
                t->atoms[a].varname);
            *err = buf; return -1;
        }
        /* Collect non-missing values, dedupe via sort+unique. */
        size_t n = f->nobs;
        double *vals = malloc(n * sizeof(double));
        size_t nv = 0;
        for(size_t i = 0; i < n; i++){
            double x = v->num[i];
            if(sv_is_miss(x)) continue;
            /* Must be a non-negative integer per Stata's `i.` rule. */
            if(x < 0 || x != floor(x)){
                static char buf[128];
                snprintf(buf, sizeof buf,
                    "factor variable i.%s contains non-integer or negative values",
                    t->atoms[a].varname);
                *err = buf; free(vals); return -1;
            }
            vals[nv++] = x;
        }
        /* Sort */
        for(size_t i = 1; i < nv; i++){
            double x = vals[i]; size_t j = i;
            while(j > 0 && vals[j-1] > x){ vals[j] = vals[j-1]; j--; }
            vals[j] = x;
        }
        /* Dedupe */
        size_t nu = 0;
        for(size_t i = 0; i < nv; i++){
            if(nu == 0 || vals[i] != vals[nu-1]) vals[nu++] = vals[i];
        }
        if(nu == 0){
            static char buf[128];
            snprintf(buf, sizeof buf,
                "factor variable i.%s has no non-missing observations",
                t->atoms[a].varname);
            *err = buf; free(vals); return -1;
        }
        double *out = malloc(nu * sizeof(double));
        memcpy(out, vals, nu * sizeof(double));
        free(vals);
        atom_levels[a] = out;
        atom_n_levels[a] = (int)nu;
        /* Identify the base level's index */
        long base = t->atoms[a].base_level;
        bool explicit_base = (base != (long)0x8000000000000000LL);
        if(explicit_base){
            int found = -1;
            for(int k = 0; k < (int)nu; k++) if((long)out[k] == base){ found = k; break; }
            if(found < 0){
                static char buf[128];
                snprintf(buf, sizeof buf,
                    "factor variable %s: base level %ld not observed",
                    t->atoms[a].varname, base);
                *err = buf; return -1;
            }
            atom_base_idx[a] = found;
        } else {
            atom_base_idx[a] = 0;  /* smallest is base */
        }
    }
    return 0;
}

/* Append a new numeric temp column to the frame, return its index. */
static int append_temp_column(Frame *f, const char *name)
{
    if(f->nvar == f->cap_var){
        f->cap_var = f->cap_var ? f->cap_var * 2 : 8;
        f->vars = realloc(f->vars, f->cap_var * sizeof *f->vars);
    }
    Variable *v = &f->vars[f->nvar];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->type = VT_NUM;
    snprintf(v->format, sizeof v->format, "%%9.0g");
    v->num = malloc(f->nobs * sizeof(double));
    for(size_t i = 0; i < f->nobs; i++) v->num[i] = SV_MISS;
    f->nvar++;
    return f->nvar - 1;
}

/* Construct the canonical display name for a cell.
 * For each atom in the term:
 *   'I' atom at level k:  "<k>.<varname>"
 *   'C' atom:             "c.<varname>"  (if the user wrote c.x) or
 *                         "<varname>"    (if bare)
 * Atoms joined by '#'. */
static void make_cell_name(const Term *t, const long *level_for_atom,
                           const char *kinds /* same length as n_atoms */,
                           char *out, size_t out_sz)
{
    size_t pos = 0;
    out[0] = 0;
    for(int a = 0; a < t->n_atoms; a++){
        if(a > 0 && pos < out_sz - 1){ out[pos++] = '#'; out[pos] = 0; }
        if(t->atoms[a].kind == 'I'){
            pos += snprintf(out + pos, out_sz - pos, "%ld.%s",
                            level_for_atom[a], t->atoms[a].varname);
        } else {
            /* Continuous.  If the user wrote `c.var` we prefix with "c."
             * (matches Stata's display); if it was a bare name, no prefix.
             * The `kinds` array tells us which the user wrote. */
            if(kinds[a] == 'C')
                pos += snprintf(out + pos, out_sz - pos, "c.%s",
                                t->atoms[a].varname);
            else
                pos += snprintf(out + pos, out_sz - pos, "%s",
                                t->atoms[a].varname);
        }
        if(pos >= out_sz - 1){ out[out_sz - 1] = 0; return; }
    }
}

/* Materialize one term into one or more columns.
 *
 * Returns 0 ok with `out_indices` populated (caller free's array;
 * frame ownership of columns themselves) and *n_added set.
 * On error returns -1, *err set, no columns added (caller cleans up
 * any temp columns added before this call by tracking the frame's
 * nvar growth). */
static int materialize_term(Frame *f, Term *t,
                            int **out_indices, int *n_added,
                            const char *user_kinds,    /* per-atom 'I'/'C' as the user wrote it */
                            const char **err)
{
    *out_indices = NULL;
    *n_added = 0;

    /* Resolve each atom's variable index. */
    for(int a = 0; a < t->n_atoms; a++){
        int vi = var_find(f, t->atoms[a].varname);
        if(vi < 0){
            static char buf[80];
            snprintf(buf, sizeof buf, "variable %s not found", t->atoms[a].varname);
            *err = buf; return -1;
        }
        t->atoms[a].var_idx = vi;
    }

    /* If any atom is 'L' (coefficient-name form like "2.g"), this term
     * is a re-materialisation request — produce exactly one column with
     * the pinned levels.  No level discovery or base dropping needed:
     * just compute the indicator product directly. */
    bool has_L = false;
    for(int a = 0; a < t->n_atoms; a++) if(t->atoms[a].kind == 'L'){ has_L = true; break; }
    if(has_L){
        /* Validate: all non-C atoms must be 'L' (mixing 'L' with 'I' or
         * 'ib' doesn't appear in legitimate workflows). */
        for(int a = 0; a < t->n_atoms; a++){
            if(t->atoms[a].kind == 'I'){
                *err = "cannot mix coefficient-name atoms (2.x) with i.x in same token";
                return -1;
            }
        }
        /* Construct the canonical display name from the atoms as-given. */
        char nm[64]; size_t pos = 0; nm[0] = 0;
        for(int a = 0; a < t->n_atoms; a++){
            if(a > 0 && pos < sizeof(nm) - 1){ nm[pos++] = '#'; nm[pos] = 0; }
            if(t->atoms[a].kind == 'L'){
                pos += snprintf(nm + pos, sizeof(nm) - pos, "%ld.%s",
                                t->atoms[a].base_level, t->atoms[a].varname);
            } else { /* 'C' */
                if(user_kinds[a] == 'C')
                    pos += snprintf(nm + pos, sizeof(nm) - pos, "c.%s",
                                    t->atoms[a].varname);
                else
                    pos += snprintf(nm + pos, sizeof(nm) - pos, "%s",
                                    t->atoms[a].varname);
            }
            if(pos >= sizeof(nm) - 1) break;
        }
        if(strlen(nm) >= 33) nm[32] = 0;

        int col = append_temp_column(f, nm);
        Variable *cv = &f->vars[col];
        for(size_t r = 0; r < f->nobs; r++){
            double val = 1.0;
            bool any_miss = false;
            for(int a = 0; a < t->n_atoms; a++){
                Variable *av = &f->vars[t->atoms[a].var_idx];
                if(av->type != VT_NUM){
                    static char buf[128];
                    snprintf(buf, sizeof buf,
                        "factor variable %s must be numeric", t->atoms[a].varname);
                    *err = buf;
                    factor_drop_temps(f, 1);
                    return -1;
                }
                double xv = av->num[r];
                if(sv_is_miss(xv)){ any_miss = true; break; }
                if(t->atoms[a].kind == 'L'){
                    if((long)xv != t->atoms[a].base_level){ val = 0; break; }
                } else { /* 'C' */
                    val *= xv;
                }
            }
            cv->num[r] = any_miss ? SV_MISS : val;
        }
        int *idx = malloc(sizeof(int));
        idx[0] = col;
        *out_indices = idx;
        *n_added = 1;
        return 0;
    }

    /* Find levels for each 'I' atom. */
    double *atom_levels[MAX_ATOMS_PER_TERM] = {0};
    int atom_n_levels[MAX_ATOMS_PER_TERM] = {0};
    int atom_base_idx[MAX_ATOMS_PER_TERM] = {0};
    int rc = discover_levels(f, t, atom_levels, atom_n_levels, atom_base_idx, err);
    if(rc < 0){
        for(int a = 0; a < t->n_atoms; a++) free(atom_levels[a]);
        return -1;
    }

    /* Enumerate cells: cross product of non-base levels of each 'I' atom.
     * For 'C' atoms there's only one "level" (no enumeration).
     * cell_count = product over 'I' atoms of (n_levels - 1). */
    long cell_count = 1;
    for(int a = 0; a < t->n_atoms; a++){
        if(t->atoms[a].kind == 'I'){
            int non_base = atom_n_levels[a] - 1;
            if(non_base <= 0){
                /* Degenerate: only one level (or zero) — no columns to add. */
                cell_count = 0;
                break;
            }
            cell_count *= non_base;
        }
    }
    if(cell_count <= 0){
        /* Nothing to add; return success with 0 columns. */
        for(int a = 0; a < t->n_atoms; a++) free(atom_levels[a]);
        *out_indices = NULL; *n_added = 0;
        return 0;
    }
    if(cell_count > 10000){
        *err = "factor expansion too large (>10000 columns)";
        for(int a = 0; a < t->n_atoms; a++) free(atom_levels[a]);
        return -1;
    }

    /* Allocate output index array. */
    int *idx = malloc((size_t)cell_count * sizeof(int));
    long emitted = 0;

    /* Walk the cell space.  For each 'I' atom we pick a non-base level
     * index from 0..atom_n_levels[a]-1 (excluding atom_base_idx[a]).
     * We use a digits-style counter `cur_level_idx[a]`. */
    int cur_idx_in_nonbase[MAX_ATOMS_PER_TERM] = {0};  /* iterator over non-base indices */

    long total_cells = cell_count;
    for(long cell = 0; cell < total_cells; cell++){
        /* Decode `cell` into a tuple of non-base indices per 'I' atom. */
        long rest = cell;
        for(int a = 0; a < t->n_atoms; a++){
            if(t->atoms[a].kind != 'I') continue;
            int n_nonbase = atom_n_levels[a] - 1;
            int sub = (int)(rest % n_nonbase);
            rest /= n_nonbase;
            cur_idx_in_nonbase[a] = sub;
        }
        /* Map non-base sub-index to actual level index in atom_levels. */
        long level_for[MAX_ATOMS_PER_TERM] = {0};
        for(int a = 0; a < t->n_atoms; a++){
            if(t->atoms[a].kind != 'I') continue;
            int sub = cur_idx_in_nonbase[a];
            int actual = sub < atom_base_idx[a] ? sub : sub + 1;
            level_for[a] = (long)atom_levels[a][actual];
        }
        /* Build canonical name. */
        char nm[64];
        make_cell_name(t, level_for, user_kinds, nm, sizeof nm);
        /* Truncate to 32 chars to fit Variable.name */
        if(strlen(nm) >= 33){
            /* For very long interaction names, use a hash suffix to keep uniqueness.
             * v1.0: just truncate.  Real users hitting this should rename. */
            nm[32] = 0;
        }
        int col = append_temp_column(f, nm);
        idx[emitted++] = col;
        /* Fill values */
        Variable *cv = &f->vars[col];
        for(size_t r = 0; r < f->nobs; r++){
            double val = 1.0;
            bool any_miss = false;
            for(int a = 0; a < t->n_atoms; a++){
                Variable *av = &f->vars[t->atoms[a].var_idx];
                double xv = av->num[r];
                if(sv_is_miss(xv)){ any_miss = true; break; }
                if(t->atoms[a].kind == 'I'){
                    if((long)xv != level_for[a]){ val = 0; break; }
                    /* else val *= 1 — no-op */
                } else {
                    val *= xv;
                }
            }
            cv->num[r] = any_miss ? SV_MISS : val;
        }
    }

    for(int a = 0; a < t->n_atoms; a++) free(atom_levels[a]);
    *out_indices = idx;
    *n_added = (int)emitted;
    return 0;
}

/* ---- public API ------------------------------------------------------- */

int factor_expand_token(Frame *f, const char *token,
                        int **out_indices, int *n_temps, const char **err)
{
    *err = NULL;
    *out_indices = NULL;
    *n_temps = 0;

    int initial_nvar = f->nvar;

    Term terms[64];
    int n_terms = 0;
    if(parse_token(token, terms, &n_terms, err) < 0) return -1;

    /* Capture per-atom user-written kind ('I' or 'C') for each term to
     * preserve `c.gdp` vs `gdp` in the display name.  Each term has its
     * own atoms[] array (parser copies); we read kind directly from
     * each. */
    int *all_indices = NULL; int total = 0;
    for(int t = 0; t < n_terms; t++){
        char user_kinds[MAX_ATOMS_PER_TERM];
        for(int a = 0; a < terms[t].n_atoms; a++) user_kinds[a] = terms[t].atoms[a].kind;
        int *part = NULL; int n_part = 0;
        if(materialize_term(f, &terms[t], &part, &n_part, user_kinds, err) < 0){
            free(all_indices); free(part);
            /* roll back any temps we added on success this turn */
            factor_drop_temps(f, f->nvar - initial_nvar);
            return -1;
        }
        if(n_part > 0){
            all_indices = realloc(all_indices, (size_t)(total + n_part) * sizeof(int));
            for(int k = 0; k < n_part; k++) all_indices[total + k] = part[k];
            total += n_part;
        }
        free(part);
    }

    *out_indices = all_indices;
    *n_temps = f->nvar - initial_nvar;
    return total;
}

void factor_drop_temps(Frame *f, int n_temps)
{
    if(n_temps <= 0) return;
    for(int j = 0; j < n_temps; j++){
        int idx = f->nvar - 1 - j;
        if(idx < 0) break;
        Variable *v = &f->vars[idx];
        if(v->type == VT_NUM){
            free(v->num);
        } else if(v->str){
            for(size_t r = 0; r < f->nobs; r++) free(v->str[r]);
            free(v->str);
        }
        memset(v, 0, sizeof *v);
    }
    f->nvar -= n_temps;
}
