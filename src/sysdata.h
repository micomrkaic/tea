/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * GPLv3; see LICENSE.  GENERATED FILE — edit the CSVs in data/ and rerun
 * tools/gen_sysdata.py instead of editing this by hand.
 * Dataset provenance and citations: data/SOURCES.md
 */
#ifndef TEA_SYSDATA_H
#define TEA_SYSDATA_H
#include <stddef.h>

typedef struct {
    const char          *name;   /* sysuse NAME              */
    const unsigned char *csv;    /* raw CSV bytes            */
    size_t               len;
    const char          *desc;   /* one-liner for sysuse dir */
} SysDataset;

extern const SysDataset SYSDATA[];
extern const int        SYSDATA_N;

#endif
