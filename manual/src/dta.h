/* dta.h — Stata .dta file format I/O, backed by readstat.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) Mico Mrkaic.
 *
 * Tea reads .dta files (any version 104-119, covering every modern Stata
 * release through Stata 18) and writes .dta files (default format 118,
 * compatible with Stata 14 and later).
 *
 * Storage-type handling per COMPATIBILITY.md:
 *   - on read, all numeric storage types are upcast to IEEE double,
 *     and string types to tea's variable-length string columns;
 *   - on write, each numeric column is compressed to the smallest
 *     storage type that fits losslessly (byte/int/long/float/double),
 *     mirroring Stata's `compress` command.
 *
 * The public API is in tea's vocabulary (Frame*) — readstat types do
 * not leak through, so callers can include this header without pulling
 * in <readstat.h>.
 */
#ifndef PSTATA_DTA_H
#define PSTATA_DTA_H

#include "dataset.h"

/* Read a Stata .dta file into the supplied frame.
 *
 *   f    — destination frame.  Must be freshly cleared by the caller
 *          (frame_clear or equivalent); existing variables are NOT
 *          overwritten.
 *   ws   — workspace, used as the home for value-label sets read from
 *          the file.  Required (NULL → error).
 *   path — filesystem path to the .dta file.
 *   err  — out: on nonzero return, points to a static error string
 *          suitable for tea_err().  Set to NULL on success.
 *
 * Returns 0 on success, a Stata-style return code (601=file not found,
 * 610=file is not Stata, 198=other parse error) otherwise.
 */
int dta_read(Frame *f, Workspace *ws, const char *path, const char **err);

/* Write the supplied frame to a Stata .dta file.
 *
 *   f       — source frame.  Must not be NULL.
 *   ws      — workspace; if non-NULL, value-label sets referenced by
 *             variables are emitted.  Pass NULL to skip value labels.
 *   path    — filesystem path; existing file is overwritten.
 *   version — DTA format version (104..119).  Pass 0 for the default
 *             (118, Stata 14+).
 *   err     — out: on nonzero return, points to a static error string.
 *
 * Returns 0 on success, nonzero return code otherwise.
 */
int dta_write(const Frame *f, const Workspace *ws, const char *path,
              int version, const char **err);

#endif /* PSTATA_DTA_H */
