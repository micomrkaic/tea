/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * linalg.h — thin wrapper over OpenBLAS (CBLAS) and LAPACKE.  All linalg
 * calls in tea route through here so the backend can be swapped later
 * (Apple Accelerate, MKL, ...) by changing this one file.
 */
#ifndef TEA_LINALG_H
#define TEA_LINALG_H

#include <cblas.h>
#include <lapacke.h>

#endif
