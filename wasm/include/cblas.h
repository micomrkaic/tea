/* tea WASM build — minimal CBLAS-compatible interface (col-major only).
 * Part of the tea distribution, GPLv3.  Copyright (C) 2026 Mico Mrkaic */
#ifndef TEA_WASM_CBLAS_H
#define TEA_WASM_CBLAS_H
typedef enum {CblasRowMajor=101, CblasColMajor=102} CBLAS_ORDER;
typedef enum {CblasNoTrans=111, CblasTrans=112, CblasConjTrans=113} CBLAS_TRANSPOSE;
void cblas_dgemm(CBLAS_ORDER order, CBLAS_TRANSPOSE ta, CBLAS_TRANSPOSE tb,
                 int m, int n, int k, double alpha, const double *a, int lda,
                 const double *b, int ldb, double beta, double *c, int ldc);
void cblas_dgemv(CBLAS_ORDER order, CBLAS_TRANSPOSE ta, int m, int n,
                 double alpha, const double *a, int lda, const double *x, int incx,
                 double beta, double *y, int incy);
#endif
