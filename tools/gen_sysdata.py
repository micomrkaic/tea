#!/usr/bin/env python3
# gen_sysdata.py — embed data/*.csv into src/sysdata.c for `sysuse`.
#
# tea ships its practice datasets inside the binary (single-binary
# philosophy: the program IS the complete practice environment).  This
# script turns each CSV in the REGISTRY into a C byte array plus a table
# the sysuse command walks.  The generated file is committed, so normal
# builds never need Python; rerun this only when data/ changes.
#
#   python3 tools/gen_sysdata.py     (from the repo root)

import os, sys

# name, csv file, one-line description shown by `sysuse dir`
REGISTRY = [
    ("grunfeld", "grunfeld.csv",
     "Grunfeld (1958) investment panel: 10 US firms x 1935-1954 (xtreg, hausman)"),
    ("airline",  "airline.csv",
     "Box-Jenkins airline passengers, monthly 1949-1960 (tsset, arima)"),
    ("longley",  "longley.csv",
     "Longley (1967) US macro, 16 obs: famously ill-conditioned (regress)"),
    ("nmes1988", "nmes1988.csv",
     "Deb-Trivedi (1997) medical care demand, 4406 persons 66+ (poisson, logit)"),
    ("pwt",      "pwt.csv",
     "Penn World Table 10.0 sample: 22 countries x 1950-2019, CC BY 4.0 (growth)"),
    ("weo",      "weo.csv",
     "IMF World Economic Outlook, April 2026: 197 economies x 1980-2031, 44 indicators"),
]

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out_c = os.path.join(root, "src", "sysdata.c")
out_h = os.path.join(root, "src", "sysdata.h")

LICENSE = """\
/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * GPLv3; see LICENSE.  GENERATED FILE — edit the CSVs in data/ and rerun
 * tools/gen_sysdata.py instead of editing this by hand.
 * Dataset provenance and citations: data/SOURCES.md
 */
"""

def c_bytes(data, per_line=20):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append(",".join(str(b) for b in data[i:i+per_line]))
    return ",\n  ".join(lines)

with open(out_c, "w") as c:
    c.write(LICENSE)
    c.write('#include "sysdata.h"\n#include <stddef.h>\n\n')
    for name, fn, desc in REGISTRY:
        path = os.path.join(root, "data", fn)
        data = open(path, "rb").read()
        c.write(f"static const unsigned char {name}_csv[] = {{\n  {c_bytes(data)}\n}};\n\n")
    c.write("const SysDataset SYSDATA[] = {\n")
    for name, fn, desc in REGISTRY:
        path = os.path.join(root, "data", fn)
        n = os.path.getsize(path)
        esc = desc.replace('"', '\\"')
        c.write(f'    {{"{name}", {name}_csv, {n}, "{esc}"}},\n')
    c.write("};\n")
    c.write(f"const int SYSDATA_N = {len(REGISTRY)};\n")

with open(out_h, "w") as h:
    h.write(LICENSE)
    h.write("""#ifndef TEA_SYSDATA_H
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
""")

total = sum(os.path.getsize(os.path.join(root, "data", fn)) for _, fn, _ in REGISTRY)
print(f"generated src/sysdata.c: {len(REGISTRY)} datasets, {total} bytes embedded")
