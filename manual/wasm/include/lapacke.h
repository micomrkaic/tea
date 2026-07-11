/* tea WASM build — minimal LAPACKE-compatible interface.
 * Backs the routines tea uses with reference CLAPACK (f2c) code.
 * Column-major only.  Part of the tea distribution, GPLv3.
 * Copyright (C) 2026 Mico Mrkaic */
#ifndef TEA_WASM_LAPACKE_H
#define TEA_WASM_LAPACKE_H
typedef int lapack_int;
#define LAPACK_ROW_MAJOR 101
#define LAPACK_COL_MAJOR 102
lapack_int LAPACKE_dgels (int layout, char trans, lapack_int m, lapack_int n, lapack_int nrhs, double *a, lapack_int lda, double *b, lapack_int ldb);
lapack_int LAPACKE_dgelsd(int layout, lapack_int m, lapack_int n, lapack_int nrhs, double *a, lapack_int lda, double *b, lapack_int ldb, double *s, double rcond, lapack_int *rank);
lapack_int LAPACKE_dgeqp3(int layout, lapack_int m, lapack_int n, double *a, lapack_int lda, lapack_int *jpvt, double *tau);
lapack_int LAPACKE_dgesdd(int layout, char jobz, lapack_int m, lapack_int n, double *a, lapack_int lda, double *s, double *u, lapack_int ldu, double *vt, lapack_int ldvt);
lapack_int LAPACKE_dgesv (int layout, lapack_int n, lapack_int nrhs, double *a, lapack_int lda, lapack_int *ipiv, double *b, lapack_int ldb);
lapack_int LAPACKE_dgetrf(int layout, lapack_int m, lapack_int n, double *a, lapack_int lda, lapack_int *ipiv);
lapack_int LAPACKE_dgetri(int layout, lapack_int n, double *a, lapack_int lda, const lapack_int *ipiv);
lapack_int LAPACKE_dpotrf(int layout, char uplo, lapack_int n, double *a, lapack_int lda);
lapack_int LAPACKE_dpotri(int layout, char uplo, lapack_int n, double *a, lapack_int lda);
#endif
