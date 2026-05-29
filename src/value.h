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
/* value.h — Stata-faithful scalar/missing model.
 *
 * Numeric cells are stored as a single double. Missing values are encoded as
 * quiet NaNs whose low mantissa bits carry a code 0..26:
 *   code 0  -> .      (system missing)
 *   code 1  -> .a
 *   ...
 *   code 26 -> .z
 *
 * Stata semantics this module guarantees:
 *   - missing sorts AFTER every real number; . < .a < .b < ... < .z
 *   - any arithmetic operand missing => result is .  (system missing)
 *   - comparisons treat missing as +infinity-with-code (so `x > 1000` is TRUE
 *     when x is missing, exactly like Stata)
 * Strings have their own missing concept: the empty string "".
 */
#ifndef PSTATA_VALUE_H
#define PSTATA_VALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Build a missing double with code 0..26 (0 == system missing `.`). */
static inline double sv_miss(int code) {
    uint64_t bits = 0x7ff8000000000000ull | ((uint64_t)(code & 0x1f));
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

#define SV_MISS sv_miss(0)

static inline bool sv_is_miss(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    /* our quiet-NaN family: exponent all ones, top mantissa bit set */
    return (bits & 0x7ff8000000000000ull) == 0x7ff8000000000000ull
        && (bits & 0x0007ffffffffffe0ull) == 0; /* only low 5 bits used */
}

/* Missing code 0..26, or -1 if not missing. */
static inline int sv_miss_code(double d) {
    if (!sv_is_miss(d)) return -1;
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    return (int)(bits & 0x1f);
}

/* Total order key used by sort/comparisons: real numbers keep their value;
 * missing maps to a band strictly greater than any finite double, ordered
 * by code so . < .a < ... < .z. */
static inline double sv_order_key(double d) {
    int c = sv_miss_code(d);
    if (c < 0) return d;
    /* 8.9e307 area mirrors Stata; +c*step keeps codes distinct & above data */
    return 8.9884656743115785e307 + (double)c * 1.0e297;
}

static inline int sv_cmp(double a, double b) {
    double ka = sv_order_key(a), kb = sv_order_key(b);
    if (ka < kb) return -1;
    if (ka > kb) return 1;
    return 0;
}

/* ---- arithmetic with Stata missing propagation -------------------------- */
static inline double sv_add(double a, double b){ if(sv_is_miss(a)||sv_is_miss(b))return SV_MISS; return a+b; }
static inline double sv_sub(double a, double b){ if(sv_is_miss(a)||sv_is_miss(b))return SV_MISS; return a-b; }
static inline double sv_mul(double a, double b){ if(sv_is_miss(a)||sv_is_miss(b))return SV_MISS; return a*b; }
static inline double sv_div(double a, double b){ if(sv_is_miss(a)||sv_is_miss(b)||b==0.0)return SV_MISS; return a/b; }
static inline double sv_neg(double a){ if(sv_is_miss(a))return SV_MISS; return -a; }

#endif
