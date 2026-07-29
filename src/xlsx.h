/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TEA_XLSX_H
#define TEA_XLSX_H
/* Native xlsx -> CSV conversion (no external tools; works on WASM).
 * sheet_name: "" or NULL = first sheet.  Returns 0, or 601/603. */
int xlsx_to_csv(const char *xlsx_path, const char *sheet_name,
                const char *csv_path);
#endif
