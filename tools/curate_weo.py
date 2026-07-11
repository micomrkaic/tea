#!/usr/bin/env python3
# curate_weo.py — turn the IMF WEO bulk download into data/weo.csv for sysuse.
#
# Input: the "All countries" WEO database file from the public WEO site
# (despite the .xls name it is tab-delimited text; recent vintages also
# offer .csv — both work here).  Format: one row per country x indicator,
# years as columns, with WEO Subject Codes, "n/a"/"--" markers, and an
# "Estimates Start After" column.
#
# Output: long panel  country iso year  + one column per indicator below.
#
#   python3 tools/curate_weo.py WEOApr2026all.xls "April 2026"
#
# then add the weo entry to tools/gen_sysdata.py's REGISTRY, rerun it,
# and rebuild.  Update the vintage line in data/SOURCES.md.

import sys, csv, io, os

# Full-dataset mode: every WEO subject code in the file becomes a variable,
# named as the lowercased code (ngdp_rpch, pcpipch, ...) — the codes are the
# IMF's own unambiguous names and the target audience reads them natively.
# The code -> descriptor/units table is written to data/weo_codes.txt for
# SOURCES.md.  All year columns present are kept, forecasts included.
YEAR_MIN, YEAR_MAX = 1900, 2100

def curate_portal(rows, vintage):
    """New IMF data-portal export: one row per SERIES_CODE 'ISO.CODE.A',
    wide years, descriptors inline, UTF-8 BOM."""
    hdr = rows[0]
    col = {h.strip(): i for i, h in enumerate(hdr)}
    year_cols = [(int(h), i) for i, h in enumerate(hdr) if h.strip().isdigit()]
    if vintage == "unknown vintage":
        for r in rows[1:]:
            v = r[col["PUBLICATION_DATE"]].strip() if "PUBLICATION_DATE" in col and len(r) > col["PUBLICATION_DATE"] else ""
            if v: vintage = v[:10] + " publication"; break
    data, names, codes = {}, {}, {}
    kept = 0
    for r in rows[1:]:
        if len(r) <= col["SERIES_CODE"]:
            continue
        parts = r[col["SERIES_CODE"]].split(".")
        if len(parts) < 3:
            continue
        iso, code = parts[0], ".".join(parts[1:-1])
        if len(iso) != 3 or not iso.isalpha():
            continue                        # aggregates use G-codes
        names[iso] = r[col["COUNTRY"]].strip()
        desc  = r[col["INDICATOR"]].strip() if "INDICATOR" in col else ""
        units = r[col["UNIT"]].strip() if "UNIT" in col else ""
        scale = r[col["SCALE"]].strip() if "SCALE" in col else ""
        codes.setdefault(code, (desc, units, scale))
        var = code.lower()
        for yr, ci in year_cols:
            if ci >= len(r): continue
            v = r[ci].strip().replace(",", "")
            if v in ("", "n/a", "--", "-"): continue
            try: v = float(v)
            except ValueError: continue
            data.setdefault((iso, yr), {})[var] = round(v, 3)
            kept += 1
    varnames = [c.lower() for c in sorted(codes)]
    return data, names, codes, varnames, kept, vintage

def curate_portal(text, vintage):
    rows = list(csv.reader(io.StringIO(text)))
    hdr = rows[0]; ix = {h: i for i, h in enumerate(hdr)}
    ycols = [(int(h), i) for i, h in enumerate(hdr)
             if h.strip().isdigit() and YEAR_MIN <= int(h) <= YEAR_MAX]
    if vintage == "unknown vintage" and "PUBLICATION_DATE" in ix and len(rows) > 1:
        vintage = "WEO published " + rows[1][ix["PUBLICATION_DATE"]][:10]

    data, names, agg, codes = {}, {}, {}, {}
    kept = 0
    for r in rows[1:]:
        parts = r[ix["SERIES_CODE"]].split(".")
        if len(parts) != 3 or parts[2] != "A":
            continue
        iso, code = parts[0], parts[1]
        names[iso] = r[ix["COUNTRY"]]
        agg[iso] = 0 if len(iso) == 3 else 1     # G-codes are aggregates
        codes.setdefault(code, (r[ix.get("INDICATOR", 4)],
                                r[ix.get("UNIT", 7)] if "UNIT" in ix else "",
                                r[ix.get("SCALE", 6)] if "SCALE" in ix else ""))
        var = code.lower()
        for yr, i in ycols:
            v = r[i].strip() if i < len(r) else ""
            if not v:
                continue
            try:
                v = float(v)
            except ValueError:
                continue
            data.setdefault((iso, yr), {})[var] = v
            kept += 1

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "data", "weo.csv")
    varnames = sorted(c.lower() for c in codes)
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["country", "iso", "year", "aggregate"] + varnames)
        for (iso, yr) in sorted(data, key=lambda k: (agg[k[0]], k[0], k[1])):
            d = data[(iso, yr)]
            w.writerow([names[iso], iso, yr, agg[iso]]
                       + [d.get(v, "") for v in varnames])

    tbl = os.path.join(root, "data", "weo_codes.txt")
    with open(tbl, "w") as f:
        f.write(f"WEO subject codes in the bundled extract ({vintage}):\n")
        f.write("(aggregate==1 marks World/regional groups; "
                "keep if aggregate==0 for country panels)\n\n")
        for c in sorted(codes):
            d, u, s = codes[c]
            su = f", {s}" if s else ""
            f.write(f"  {c.lower():<16} {d} ({u}{su})\n")

    print(f"wrote data/weo.csv: {len(data)} unit-years, {kept} values, "
          f"{sum(1 for a in agg.values() if a==0)} countries + "
          f"{sum(1 for a in agg.values() if a==1)} aggregates, "
          f"{len(codes)} indicators, vintage: {vintage}")
    print(f"size: {os.path.getsize(out)} bytes")
    print("wrote data/weo_codes.txt")
    return

def main():
    if len(sys.argv) < 2:
        sys.exit("usage: curate_weo.py WEO-all-countries-file [vintage-label]")
    src = sys.argv[1]
    vintage = sys.argv[2] if len(sys.argv) > 2 else "unknown vintage"

    raw = open(src, "rb").read()
    # WEO files are commonly UTF-16LE ('.xls' vintages) or latin-1/utf-8 (.csv)
    for enc in ("utf-16", "utf-8-sig", "latin-1"):
        try:
            text = raw.decode(enc)
            first = text.splitlines()[0]
            if "WEO Subject Code" in first or "SERIES_CODE" in first:
                break
        except (UnicodeDecodeError, IndexError):
            continue
    else:
        sys.exit("could not decode / recognize the WEO file header")

    # ---- new IMF data-portal export (SDMX-style): SERIES_CODE = ISO.CODE.FREQ
    if "SERIES_CODE" in text.splitlines()[0]:
        return curate_portal(text, vintage)

    delim = "\t" if "\t" in text.splitlines()[0] else ","
    rows = list(csv.reader(io.StringIO(text), delimiter=delim))
    hdr = rows[0]
    col = {h.strip(): i for i, h in enumerate(hdr)}

    if "SERIES_CODE" in col:            # new IMF data-portal export
        data, names, codes, varnames, kept, vintage = curate_portal(rows, vintage)
        write_out(data, names, codes, varnames, kept, vintage)
        return

    for need in ("ISO", "WEO Subject Code", "Country"):
        if need not in col:
            sys.exit(f"missing expected column: {need}")
    year_cols = [(int(h), i) for i, h in enumerate(hdr)
                 if h.strip().isdigit() and YEAR_MIN <= int(h) <= YEAR_MAX]

    # discover the indicator set from the file itself
    codes = {}   # subject code -> (descriptor, units, scale)
    for r in rows[1:]:
        if len(r) <= col["WEO Subject Code"]:
            continue
        code = r[col["WEO Subject Code"]].strip()
        if code and r[col["ISO"]].strip():
            desc  = r[col.get("Subject Descriptor", 4)].strip() if len(r) > col.get("Subject Descriptor", 4) else ""
            units = r[col.get("Units", 5)].strip() if len(r) > col.get("Units", 5) else ""
            scale = r[col.get("Scale", 6)].strip() if len(r) > col.get("Scale", 6) else ""
            codes.setdefault(code, (desc, units, scale))
    INDICATORS = {c: c.lower() for c in sorted(codes)}

    # data[(iso, year)] = {var: value}, names[iso] = country
    data, names = {}, {}
    kept = 0
    for r in rows[1:]:
        if len(r) <= col["WEO Subject Code"]:
            continue                      # footnote/blank tail rows
        code = r[col["WEO Subject Code"]].strip()
        if code not in INDICATORS:
            continue
        iso = r[col["ISO"]].strip()
        if not iso or len(iso) != 3:
            continue                      # aggregates use non-ISO codes
        names[iso] = r[col["Country"]].strip()
        var = INDICATORS[code]
        for yr, i in year_cols:
            if i >= len(r):
                continue
            v = r[i].strip().replace(",", "")
            if v in ("", "n/a", "--", "-"):
                continue
            try:
                v = float(v)
            except ValueError:
                continue
            data.setdefault((iso, yr), {})[var] = round(v, 3)
            kept += 1

    varnames = list(INDICATORS.values())
    write_out(data, names, codes, varnames, kept, vintage)

def write_out(data, names, codes, varnames, kept, vintage):
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "data", "weo.csv")
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["country", "iso", "year"] + varnames)
        for (iso, yr) in sorted(data):
            d = data[(iso, yr)]
            w.writerow([names[iso], iso, yr] + [d.get(v, "") for v in varnames])

    # write the code table for SOURCES.md
    tbl = os.path.join(os.path.dirname(out), "weo_codes.txt")
    with open(tbl, "w") as f:
        f.write(f"WEO subject codes in the bundled extract ({vintage}):\n\n")
        for c in sorted(codes):
            d, u, s = codes[c]
            su = f" [{s}]" if s and s.lower() not in ("units", "") else ""
            f.write(f"  {c.lower():<16} {d} ({u}{su})\n")

    n = len(data)
    print(f"wrote data/weo.csv: {n} country-years, {kept} values, "
          f"{len(names)} countries, {len(codes)} indicators, vintage: {vintage}")
    print("wrote data/weo_codes.txt (code -> descriptor table for SOURCES.md)")
    print(f"size: {os.path.getsize(out)} bytes")
    print("next: add weo to tools/gen_sysdata.py REGISTRY, rerun it, "
          "update data/SOURCES.md vintage, make && make test")

if __name__ == "__main__":
    main()
