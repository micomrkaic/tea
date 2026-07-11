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

# Output: long panel  country iso year aggregate  + one lowercased-subject-
# code column per indicator.  World/regional groups (non-ISO G-codes) are
# kept with aggregate==1, so `keep if aggregate==0` yields the country
# panel and `keep if aggregate==1` the groups.  All year columns are kept,
# projections included; note the vintage in data/SOURCES.md.

def write_output(data, names, codes, vintage):
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "data", "weo.csv")
    varnames = [c.lower() for c in sorted(codes)]
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["country", "iso", "year", "aggregate"] + varnames)
        for (iso, yr) in sorted(data):
            d = data[(iso, yr)]
            agg = 0 if len(iso) == 3 else 1
            w.writerow([names[iso], iso, yr, agg] + [d.get(v, "") for v in varnames])
    tbl = os.path.join(os.path.dirname(out), "weo_codes.txt")
    with open(tbl, "w") as f:
        f.write(f"WEO subject codes in the bundled extract ({vintage}):\n\n")
        for c in sorted(codes):
            d, u, s = codes[c]
            su = f" [{s}]" if s else ""
            f.write(f"  {c.lower():<16} {d} ({u}{su})\n")
    print(f"wrote data/weo.csv: {len(data)} country-years, "
          f"{len(names)} countries, {len(codes)} indicators, vintage: {vintage}")
    print(f"size: {os.path.getsize(out)} bytes")

def parse_value(v):
    v = v.strip().replace(",", "")
    if v in ("", "n/a", "--", "-", "NA"): return None
    try: return round(float(v), 3)
    except ValueError: return None

# ---- new data-portal export (data.imf.org): SERIES_CODE = ISO.CODE.A ----
def curate_portal(path, vintage):
    with open(path, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.reader(f))
    hdr = rows[0]
    col = {h.strip(): i for i, h in enumerate(hdr)}
    year_cols = [(int(h), i) for i, h in enumerate(hdr) if h.strip().isdigit()]
    data, names, codes = {}, {}, {}
    for r in rows[1:]:
        if len(r) <= col["SERIES_CODE"]: continue
        parts = r[col["SERIES_CODE"]].split(".")
        if len(parts) != 3 or parts[2] != "A": continue
        iso, code = parts[0], parts[1]
        names[iso] = r[col["COUNTRY"]].strip()
        codes.setdefault(code, (r[col["INDICATOR"]].strip(),
                                r[col["UNIT"]].strip(),
                                r[col["SCALE"]].strip()))
        var = code.lower()
        for yr, i in year_cols:
            if i >= len(r): continue
            v = parse_value(r[i])
            if v is None: continue
            data.setdefault((iso, yr), {})[var] = v
    write_output(data, names, codes, vintage)

# ---- classic bulk file (WEO<vintage>all.xls): tab-delimited, UTF-16 ----
def curate_classic(path, vintage):
    raw = open(path, "rb").read()
    for enc in ("utf-16", "utf-8-sig", "latin-1"):
        try:
            text = raw.decode(enc)
            if "WEO Subject Code" in text.splitlines()[0]: break
        except (UnicodeDecodeError, IndexError): continue
    else:
        sys.exit("could not decode / recognize the WEO file header")
    delim = "\t" if "\t" in text.splitlines()[0] else ","
    rows = list(csv.reader(io.StringIO(text), delimiter=delim))
    hdr = rows[0]
    col = {h.strip(): i for i, h in enumerate(hdr)}
    year_cols = [(int(h), i) for i, h in enumerate(hdr) if h.strip().isdigit()]
    data, names, codes = {}, {}, {}
    for r in rows[1:]:
        if len(r) <= col["WEO Subject Code"]: continue
        code = r[col["WEO Subject Code"]].strip()
        iso  = r[col["ISO"]].strip()
        if not code or not iso: continue
        names[iso] = r[col["Country"]].strip()
        codes.setdefault(code, (r[col["Subject Descriptor"]].strip(),
                                r[col["Units"]].strip(),
                                r[col["Scale"]].strip() if "Scale" in col else ""))
        var = code.lower()
        for yr, i in year_cols:
            if i >= len(r): continue
            v = parse_value(r[i])
            if v is None: continue
            data.setdefault((iso, yr), {})[var] = v
    write_output(data, names, codes, vintage)

def main():
    if len(sys.argv) < 2:
        sys.exit("usage: curate_weo.py WEO-file [vintage-label]")
    src, vintage = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "unknown vintage")
    head = open(src, "rb").read(4096).decode("utf-8", errors="replace")
    if "SERIES_CODE" in head: curate_portal(src, vintage)
    else:                     curate_classic(src, vintage)

if __name__ == "__main__":
    main()
