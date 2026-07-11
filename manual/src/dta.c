/* dta.c — Stata .dta file I/O backed by readstat.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) Mico Mrkaic.
 *
 * Status: dta_read implemented (step 3 of the v0.6 roadmap);
 *         dta_write still a stub (step 4).
 *
 * Storage-type handling per COMPATIBILITY.md:
 *   - on read, all numeric storage types (byte/int/long/float/double) are
 *     upcast to IEEE double, and string types (str#/strL) to tea's
 *     variable-length string columns.  No precision is lost; we just use
 *     more RAM than Stata would for compressible columns.
 *   - missing-value codes preserved: system-missing (.) -> sv_miss(0);
 *     tagged-missing (.a-.z) -> sv_miss(1..26).
 *
 * Value labels: the value-label set NAME attached to each variable is
 * captured into Variable.vallab[33], but the actual labels themselves
 * (value -> text mappings) are not yet wired through.  This is a
 * deliberate, documented gap for the step-3 cut.
 */

#include "dta.h"
#include "value.h"
#include <readstat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Static error buffer.  Tea is single-threaded; one buffer is fine.
 * Each call to dta_read/dta_write resets this. */
static char g_err_buf[256];

/* ---- read path -------------------------------------------------------- */

typedef struct {
    Frame     *frame;
    Workspace *ws;          /* destination for value-label sets */
    int    nvars_total;     /* from metadata; -1 until set */
    long   nobs_total;      /* from metadata; -1 until set */
    bool   sized;           /* frame_set_nobs called yet? */
    int    abort_rc;        /* nonzero -> abort the parse */
} ReadCtx;

/* Translate a readstat tagged-missing tag ('a'..'z') into a tea sv_miss
 * code (1..26).  Anything else returns 0 (system missing). */
static int tag_to_code(char tag)
{
    if (tag >= 'a' && tag <= 'z') return tag - 'a' + 1;
    if (tag >= 'A' && tag <= 'Z') return tag - 'A' + 1;
    return 0;
}

/* Translate readstat type to tea VarType. */
static VarType rs_to_vt(readstat_type_t t)
{
    switch (t) {
        case READSTAT_TYPE_STRING:
        case READSTAT_TYPE_STRING_REF:
            return VT_STR;
        default:
            return VT_NUM;
    }
}

/* metadata_handler: called once before any variable/value handlers.
 * Captures the dataset's row/var counts and the file-level dataset label
 * (Stata's `label data` text). */
static int meta_handler(readstat_metadata_t *meta, void *ctx_v)
{
    ReadCtx *ctx = ctx_v;
    ctx->nobs_total  = (long)readstat_get_row_count(meta);
    ctx->nvars_total = readstat_get_var_count(meta);
    if (ctx->nobs_total < 0) {
        snprintf(g_err_buf, sizeof g_err_buf,
                 "dta_read: file does not declare row count (not supported)");
        ctx->abort_rc = 198;
        return READSTAT_HANDLER_ABORT;
    }
    if (ctx->nvars_total < 0) {
        snprintf(g_err_buf, sizeof g_err_buf,
                 "dta_read: file does not declare variable count");
        ctx->abort_rc = 198;
        return READSTAT_HANDLER_ABORT;
    }
    const char *flabel = readstat_get_file_label(meta);
    if (flabel && *flabel) {
        snprintf(ctx->frame->data_label, sizeof ctx->frame->data_label,
                 "%s", flabel);
    }
    return READSTAT_HANDLER_OK;
}

/* variable_handler: called once per variable, in declaration order.
 * Registers the variable in the frame and captures its metadata. */
static int var_handler(int index, readstat_variable_t *variable,
                       const char *val_labels, void *ctx_v)
{
    ReadCtx *ctx = ctx_v;
    const char *name = readstat_variable_get_name(variable);
    if (!name || !*name) {
        snprintf(g_err_buf, sizeof g_err_buf,
                 "dta_read: variable #%d has no name", index);
        ctx->abort_rc = 198;
        return READSTAT_HANDLER_ABORT;
    }
    VarType vt = rs_to_vt(readstat_variable_get_type(variable));
    Variable *v = var_add(ctx->frame, name, vt);
    if (!v) {
        snprintf(g_err_buf, sizeof g_err_buf,
                 "dta_read: failed to register variable '%s'", name);
        ctx->abort_rc = 198;
        return READSTAT_HANDLER_ABORT;
    }

    const char *label = readstat_variable_get_label(variable);
    if (label && *label) snprintf(v->vlabel, sizeof v->vlabel, "%s", label);

    const char *format = readstat_variable_get_format(variable);
    if (format && *format) snprintf(v->format, sizeof v->format, "%s", format);
    /* default if file didn't specify: %9.0g for numerics, %#s for strings */
    if (!v->format[0]) {
        if (vt == VT_NUM) snprintf(v->format, sizeof v->format, "%%9.0g");
        else              snprintf(v->format, sizeof v->format, "%%%zus",
                                   readstat_variable_get_storage_width(variable));
    }

    if (val_labels && *val_labels) {
        snprintf(v->vallab, sizeof v->vallab, "%s", val_labels);
    }

    return READSTAT_HANDLER_OK;
}

/* value_label_handler: called for every (label_set_name, value, text)
 * triple in the file's value-label section.  Routes each into the
 * appropriate VLabel set in the workspace.  Only numeric and tagged
 * values are expected for .dta; string-keyed labels (a SPSS thing) are
 * ignored. */
static int value_label_handler(const char *val_labels,
                               readstat_value_t value, const char *label,
                               void *ctx_v)
{
    ReadCtx *ctx = ctx_v;
    if (!ctx->ws || !val_labels || !val_labels[0] || !label) {
        return READSTAT_HANDLER_OK;
    }
    VLabel *L = vlabel_ensure(ctx->ws, val_labels);
    if (!L) return READSTAT_HANDLER_OK;

    double v;
    if (readstat_value_is_tagged_missing(value)) {
        v = sv_miss(tag_to_code(readstat_value_tag(value)));
    } else if (readstat_value_is_system_missing(value)) {
        v = SV_MISS;
    } else {
        switch (readstat_value_type(value)) {
            case READSTAT_TYPE_INT8:   v = (double)readstat_int8_value(value); break;
            case READSTAT_TYPE_INT16:  v = (double)readstat_int16_value(value); break;
            case READSTAT_TYPE_INT32:  v = (double)readstat_int32_value(value); break;
            case READSTAT_TYPE_FLOAT:  v = (double)readstat_float_value(value); break;
            case READSTAT_TYPE_DOUBLE: v = readstat_double_value(value); break;
            default: return READSTAT_HANDLER_OK;  /* string label - skip */
        }
    }
    vlabel_put(L, v, label);
    return READSTAT_HANDLER_OK;
}

/* fweight_handler: called when the file marks a variable as the default
 * frequency weight (Stata's `[fweight=v]` annotation baked into the .dta).
 * We capture the name; commands don't auto-apply it (Stata doesn't either),
 * but it round-trips through save. */
static int fweight_handler(readstat_variable_t *variable, void *ctx_v)
{
    ReadCtx *ctx = ctx_v;
    const char *name = readstat_variable_get_name(variable);
    if (name && *name && ctx->frame) {
        snprintf(ctx->frame->fweight_var, sizeof ctx->frame->fweight_var, "%s", name);
    }
    return READSTAT_HANDLER_OK;
}

/* value_handler: called row-major for every cell.  On the first call we
 * lazily call frame_set_nobs to allocate all columns at once; this is
 * cheaper than growing per-row and avoids needing a separate
 * "all-variables-done" callback. */
static int value_handler(int obs_index, readstat_variable_t *variable,
                         readstat_value_t value, void *ctx_v)
{
    ReadCtx *ctx = ctx_v;

    if (!ctx->sized) {
        frame_set_nobs(ctx->frame, (size_t)ctx->nobs_total);
        ctx->sized = true;
    }

    int var_idx = readstat_variable_get_index(variable);
    if (var_idx < 0 || var_idx >= ctx->frame->nvar
        || obs_index < 0 || (size_t)obs_index >= ctx->frame->nobs) {
        snprintf(g_err_buf, sizeof g_err_buf,
                 "dta_read: out-of-range cell at row %d, var %d", obs_index, var_idx);
        ctx->abort_rc = 198;
        return READSTAT_HANDLER_ABORT;
    }
    Variable *v = &ctx->frame->vars[var_idx];

    if (v->type == VT_NUM) {
        double out;
        if (readstat_value_is_system_missing(value)) {
            out = SV_MISS;
        } else if (readstat_value_is_tagged_missing(value)) {
            out = sv_miss(tag_to_code(readstat_value_tag(value)));
        } else {
            /* readstat exposes type-specific extractors; readstat_double_value
             * only returns a meaningful value for DOUBLE-typed cells.  Promote
             * each smaller numeric type to double explicitly. */
            switch (readstat_value_type(value)) {
                case READSTAT_TYPE_INT8:
                    out = (double)readstat_int8_value(value);  break;
                case READSTAT_TYPE_INT16:
                    out = (double)readstat_int16_value(value); break;
                case READSTAT_TYPE_INT32:
                    out = (double)readstat_int32_value(value); break;
                case READSTAT_TYPE_FLOAT:
                    out = (double)readstat_float_value(value); break;
                case READSTAT_TYPE_DOUBLE:
                    out = readstat_double_value(value);        break;
                default:
                    /* a string value in a numeric column would be a readstat
                     * bug; defensive only */
                    out = SV_MISS;
                    break;
            }
        }
        v->num[obs_index] = out;
    } else {
        /* string column */
        if (readstat_value_is_system_missing(value)) {
            str_set(v, (size_t)obs_index, "");
        } else {
            const char *s = readstat_string_value(value);
            str_set(v, (size_t)obs_index, s ? s : "");
        }
    }
    return READSTAT_HANDLER_OK;
}

/* Translate readstat error codes to Stata-style return codes. */
static int rs_err_to_rc(readstat_error_t e)
{
    switch (e) {
        case READSTAT_OK:                 return 0;
        case READSTAT_ERROR_OPEN:         return 601;  /* file not found */
        case READSTAT_ERROR_READ:         return 691;  /* I/O error */
        case READSTAT_ERROR_MALLOC:       return 909;  /* out of memory */
        case READSTAT_ERROR_USER_ABORT:   return 198;  /* our callback aborted */
        case READSTAT_ERROR_PARSE:        return 610;  /* file is not Stata */
        case READSTAT_ERROR_UNSUPPORTED_FILE_FORMAT_VERSION: return 610;
        default:                          return 198;
    }
}

int dta_read(Frame *f, Workspace *ws, const char *path, const char **err)
{
    *err = NULL;
    g_err_buf[0] = 0;

    if (!f) {
        *err = "dta_read: NULL frame";
        return 198;
    }
    if (f->nvar > 0 || f->nobs > 0) {
        *err = "no; data in memory would be lost";
        return 4;
    }
    if (!path || !*path) {
        *err = "dta_read: empty path";
        return 198;
    }

    ReadCtx ctx = {0};
    ctx.frame = f;
    ctx.ws    = ws;
    ctx.nvars_total = -1;
    ctx.nobs_total  = -1;

    readstat_parser_t *parser = readstat_parser_init();
    if (!parser) {
        *err = "dta_read: failed to initialise readstat parser";
        return 909;
    }
    readstat_set_metadata_handler(parser, meta_handler);
    readstat_set_variable_handler(parser, var_handler);
    readstat_set_value_handler(parser, value_handler);
    readstat_set_fweight_handler(parser, fweight_handler);
    if (ws) {
        readstat_set_value_label_handler(parser, value_label_handler);
    }

    readstat_error_t e = readstat_parse_dta(parser, path, &ctx);
    readstat_parser_free(parser);

    if (e != READSTAT_OK) {
        /* If our callback aborted, the more specific message is already in
         * g_err_buf.  Otherwise translate readstat's own error message. */
        if (e != READSTAT_ERROR_USER_ABORT) {
            snprintf(g_err_buf, sizeof g_err_buf,
                     "dta_read: %s", readstat_error_message(e));
        }
        *err = g_err_buf;
        /* leave whatever we managed to read in place; caller may clear */
        return ctx.abort_rc ? ctx.abort_rc : rs_err_to_rc(e);
    }

    /* Edge case: empty dataset (no rows).  The value_handler never fired,
     * so frame_set_nobs was never called.  Size it to zero now. */
    if (!ctx.sized) {
        frame_set_nobs(f, (size_t)ctx.nobs_total);
    }

    return 0;
}

/* ---- write path ------------------------------------------------------- */

/* Default DTA format version we emit.  118 = Stata 14, the safest mainstream
 * default: readable by every Stata version from 14 onward (Sept 2015),
 * supports UTF-8 names/labels and strL.  Override later via an option if
 * users need 117 (Stata 13) or 119 (Stata 15+ with >32k variables). */
#define TEA_DTA_DEFAULT_VERSION 118

/* Pick the smallest Stata numeric storage type that fits the column's
 * non-missing values losslessly.  Mirrors Stata's `compress` command.
 *
 * Stata storage ranges (the part not reserved for missing-value codes):
 *   byte: [-127, 100]       (101..127 reserve ., .a-.z)
 *   int:  [-32767, 32740]   (similarly)
 *   long: [-2147483647, 2147483620]
 *   float: ~ ±3.4e38, ~7 digits precision
 *   double: full IEEE
 *
 * All-missing column defaults to byte (matches Stata). */
static readstat_type_t pick_numeric_type(const Variable *v, size_t nobs)
{
    bool   all_integer = true;
    bool   any_value   = false;
    double lo = 0, hi = 0;

    for (size_t i = 0; i < nobs; i++) {
        double x = v->num[i];
        if (sv_is_miss(x)) continue;
        if (!any_value) { lo = hi = x; any_value = true; }
        else {
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        /* trunc() returns the integral part; equal iff x is integer-valued */
        if (x != (double)(long long)x || x < -9.2e18 || x > 9.2e18) {
            /* outside long long range or fractional */
            all_integer = false;
        }
    }
    if (!any_value) return READSTAT_TYPE_INT8;

    if (all_integer) {
        if (lo >= -127.0           && hi <= 100.0)        return READSTAT_TYPE_INT8;
        if (lo >= -32767.0         && hi <= 32740.0)      return READSTAT_TYPE_INT16;
        if (lo >= -2147483647.0    && hi <= 2147483620.0) return READSTAT_TYPE_INT32;
    }

    /* Try float: every value exact under float round-trip. */
    bool float_ok = true;
    for (size_t i = 0; i < nobs && float_ok; i++) {
        double x = v->num[i];
        if (sv_is_miss(x)) continue;
        if ((double)(float)x != x) float_ok = false;
    }
    return float_ok ? READSTAT_TYPE_FLOAT : READSTAT_TYPE_DOUBLE;
}

/* Max strlen across the column.  Capped at 2045 (the str# limit in
 * dta-117+); longer columns use strL via STRING_REF.  Minimum 1, since
 * readstat rejects width-0 string variables. */
static size_t pick_string_width(const Variable *v, size_t nobs)
{
    size_t maxlen = 1;
    for (size_t i = 0; i < nobs; i++) {
        const char *s = v->str[i];
        if (!s) continue;
        size_t L = strlen(s);
        if (L > maxlen) maxlen = L;
    }
    if (maxlen > 2045) maxlen = 2045;
    return maxlen;
}

/* The data_writer callback readstat invokes for every byte it produces. */
static ssize_t dta_write_cb(const void *data, size_t len, void *ctx)
{
    FILE *fp = ctx;
    size_t n = fwrite(data, 1, len, fp);
    if (n != len) return -1;
    return (ssize_t)len;
}

/* Insert a single non-missing numeric value via the type-specific API.
 * The cast for INT8/INT16/INT32 is safe because pick_numeric_type already
 * confirmed all values fit the target range. */
static readstat_error_t insert_numeric(readstat_writer_t *w,
                                       readstat_variable_t *rv,
                                       readstat_type_t rt, double x)
{
    switch (rt) {
        case READSTAT_TYPE_INT8:
            return readstat_insert_int8_value (w, rv, (int8_t)x);
        case READSTAT_TYPE_INT16:
            return readstat_insert_int16_value(w, rv, (int16_t)x);
        case READSTAT_TYPE_INT32:
            return readstat_insert_int32_value(w, rv, (int32_t)x);
        case READSTAT_TYPE_FLOAT:
            return readstat_insert_float_value(w, rv, (float)x);
        case READSTAT_TYPE_DOUBLE:
            return readstat_insert_double_value(w, rv, x);
        default:
            return READSTAT_ERROR_VALUE_TYPE_MISMATCH;
    }
}

/* Insert a missing or tagged-missing for a numeric column.  code 0 is
 * system missing (.); 1..26 are .a..z. */
static readstat_error_t insert_missing(readstat_writer_t *w,
                                       readstat_variable_t *rv, int code)
{
    if (code <= 0 || code > 26)
        return readstat_insert_missing_value(w, rv);
    return readstat_insert_tagged_missing_value(w, rv, (char)('a' + code - 1));
}

typedef struct {
    readstat_type_t       rt;
    size_t                width;     /* for strings; 0 for numerics */
    readstat_variable_t  *rv;        /* readstat handle */
} ColPlan;

/* Walk variables; for each distinct .vallab name, register a readstat
 * label set populated from the workspace's matching VLabel.  Returns an
 * array of (name, readstat_label_set_t*) pairs and its length.
 * Caller frees the array. */
typedef struct { const char *name; readstat_label_set_t *rls; } LabelSetEntry;

static int build_label_sets(readstat_writer_t *w, const Frame *f,
                            const Workspace *ws,
                            LabelSetEntry **out_entries, int *out_n)
{
    *out_entries = NULL; *out_n = 0;
    if (!ws) return 0;

    /* unique vallab names referenced by any variable */
    LabelSetEntry *ents = calloc((size_t)f->nvar, sizeof *ents);
    if (!ents) return 909;
    int n = 0;
    for (int j = 0; j < f->nvar; j++) {
        const Variable *v = &f->vars[j];
        if (!v->vallab[0]) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) {
            if (!strcmp(ents[k].name, v->vallab)) { dup = true; break; }
        }
        if (dup) continue;
        /* must exist in workspace to emit */
        VLabel *L = vlabel_get((Workspace*)ws, v->vallab);
        if (!L) continue;
        readstat_label_set_t *rls =
            readstat_add_label_set(w, READSTAT_TYPE_DOUBLE, v->vallab);
        if (!rls) { free(ents); return 909; }
        for (VLItem *it = L->items; it; it = it->next) {
            readstat_label_double_value(rls, it->val, it->txt);
        }
        ents[n].name = v->vallab;
        ents[n].rls  = rls;
        n++;
    }
    *out_entries = ents;
    *out_n = n;
    return 0;
}

int dta_write(const Frame *f, const Workspace *ws, const char *path,
              int version, const char **err)
{
    *err = NULL;
    g_err_buf[0] = 0;

    if (!f) { *err = "dta_write: NULL frame"; return 198; }
    if (!path || !*path) { *err = "dta_write: empty path"; return 198; }
    if (f->nvar == 0) {
        *err = "dta_write: cannot write a frame with zero variables";
        return 198;
    }
    if (version != 0 && (version < 104 || version > 119)) {
        snprintf(g_err_buf, sizeof g_err_buf,
                 "dta_write: unsupported DTA version %d (valid: 104-119)", version);
        *err = g_err_buf;
        return 198;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        snprintf(g_err_buf, sizeof g_err_buf,
                 "dta_write: cannot open '%s' for writing", path);
        *err = g_err_buf;
        return 603;   /* file could not be opened */
    }

    readstat_writer_t *w = readstat_writer_init();
    if (!w) {
        fclose(fp); unlink(path);
        *err = "dta_write: readstat writer init failed";
        return 909;
    }
    readstat_set_data_writer(w, dta_write_cb);
    readstat_writer_set_file_format_version(w,
        version != 0 ? (uint8_t)version : (uint8_t)TEA_DTA_DEFAULT_VERSION);
    if (f->data_label[0]) {
        readstat_writer_set_file_label(w, f->data_label);
    }

    ColPlan *cols = calloc((size_t)f->nvar, sizeof *cols);
    if (!cols) {
        readstat_writer_free(w); fclose(fp); unlink(path);
        *err = "dta_write: out of memory";
        return 909;
    }

    /* Build label sets first (they're referenced by variables). */
    LabelSetEntry *lsets = NULL; int n_lsets = 0;
    int ls_rc = build_label_sets(w, f, ws, &lsets, &n_lsets);
    if (ls_rc) {
        free(cols); readstat_writer_free(w); fclose(fp); unlink(path);
        *err = "dta_write: failed to emit label sets";
        return ls_rc;
    }

    /* Pass 1: per-column compress + declare variable + attach label set. */
    for (int j = 0; j < f->nvar; j++) {
        Variable *v = &f->vars[j];
        if (v->type == VT_NUM) {
            cols[j].rt    = pick_numeric_type(v, f->nobs);
            cols[j].width = 0;
        } else {
            cols[j].width = pick_string_width(v, f->nobs);
            cols[j].rt    = (cols[j].width > 2045)
                            ? READSTAT_TYPE_STRING_REF
                            : READSTAT_TYPE_STRING;
        }
        cols[j].rv = readstat_add_variable(w, v->name, cols[j].rt, cols[j].width);
        if (!cols[j].rv) {
            snprintf(g_err_buf, sizeof g_err_buf,
                     "dta_write: failed to declare variable '%s'", v->name);
            *err = g_err_buf;
            free(cols); free(lsets); readstat_writer_free(w); fclose(fp); unlink(path);
            return 198;
        }
        if (v->vlabel[0]) readstat_variable_set_label(cols[j].rv, v->vlabel);
        if (v->format[0]) readstat_variable_set_format(cols[j].rv, v->format);
        /* attach value-label set if one is registered and referenced */
        if (v->vallab[0]) {
            for (int k = 0; k < n_lsets; k++) {
                if (!strcmp(lsets[k].name, v->vallab)) {
                    readstat_variable_set_label_set(cols[j].rv, lsets[k].rls);
                    break;
                }
            }
        }
    }

    /* If a frequency-weight variable is declared on the frame, mark it.
     * Locate the matching declared variable; if it isn't present in the
     * frame (e.g. dropped before save) we silently skip. */
    if (f->fweight_var[0]) {
        for (int j = 0; j < f->nvar; j++) {
            if (!strcmp(f->vars[j].name, f->fweight_var)) {
                readstat_writer_set_fweight_variable(w, cols[j].rv);
                break;
            }
        }
    }

    readstat_error_t e = readstat_begin_writing_dta(w, fp, (long)f->nobs);
    if (e != READSTAT_OK) {
        snprintf(g_err_buf, sizeof g_err_buf,
                 "dta_write: begin_writing_dta failed: %s",
                 readstat_error_message(e));
        *err = g_err_buf;
        free(cols); free(lsets); readstat_writer_free(w); fclose(fp); unlink(path);
        return rs_err_to_rc(e);
    }

    /* Pass 2: row by row, insert values. */
    for (size_t i = 0; i < f->nobs; i++) {
        e = readstat_begin_row(w);
        if (e != READSTAT_OK) goto write_err;
        for (int j = 0; j < f->nvar; j++) {
            Variable *v = &f->vars[j];
            if (v->type == VT_NUM) {
                double x = v->num[i];
                if (sv_is_miss(x)) {
                    e = insert_missing(w, cols[j].rv, sv_miss_code(x));
                } else {
                    e = insert_numeric(w, cols[j].rv, cols[j].rt, x);
                }
            } else {
                const char *s = v->str[i];
                e = readstat_insert_string_value(w, cols[j].rv, s ? s : "");
            }
            if (e != READSTAT_OK) goto write_err;
        }
        e = readstat_end_row(w);
        if (e != READSTAT_OK) goto write_err;
    }

    e = readstat_end_writing(w);
    if (e != READSTAT_OK) goto write_err;

    free(cols); free(lsets);
    readstat_writer_free(w);
    fclose(fp);
    return 0;

write_err:
    snprintf(g_err_buf, sizeof g_err_buf,
             "dta_write: %s", readstat_error_message(e));
    *err = g_err_buf;
    free(cols); free(lsets);
    readstat_writer_free(w);
    fclose(fp);
    unlink(path);
    return rs_err_to_rc(e);
}
