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
/* wasm_linalg.c — WASM backend for linalg.h.
 *
 * The native build links OpenBLAS + LAPACKE.  Neither exists for WASM, so
 * this file implements the exact LAPACKE_/cblas_ entry points tea uses on
 * top of reference CLAPACK (f2c-translated LAPACK + BLAS), declaring the
 * f2c symbols with their true signatures (f2c subroutines return int and
 * take integer* == 32-bit on wasm32).  This avoids the signature-mismatch
 * traps that linking the stock LAPACKE C wrappers against f2c code causes
 * under wasm-ld's strict function typing.
 *
 * Column-major only — every call site in tea uses LAPACK_COL_MAJOR, so
 * the row-major transpose machinery is dead weight we simply don't carry.
 */
#ifdef __EMSCRIPTEN__
#include <stdlib.h>
#include "../wasm/include/lapacke.h"
#include "../wasm/include/cblas.h"

typedef int  f2c_int;      /* f2c 'integer' == long == 32-bit on wasm32 */
typedef double f2c_dbl;

/* f2c prototypes (true signatures: int return, all args by pointer) */
extern int dgels_ (char *trans, f2c_int *m, f2c_int *n, f2c_int *nrhs, f2c_dbl *a, f2c_int *lda, f2c_dbl *b, f2c_int *ldb, f2c_dbl *work, f2c_int *lwork, f2c_int *info);
extern int dgelsd_(f2c_int *m, f2c_int *n, f2c_int *nrhs, f2c_dbl *a, f2c_int *lda, f2c_dbl *b, f2c_int *ldb, f2c_dbl *s, f2c_dbl *rcond, f2c_int *rank, f2c_dbl *work, f2c_int *lwork, f2c_int *iwork, f2c_int *info);
extern int dgeqp3_(f2c_int *m, f2c_int *n, f2c_dbl *a, f2c_int *lda, f2c_int *jpvt, f2c_dbl *tau, f2c_dbl *work, f2c_int *lwork, f2c_int *info);
extern int dgesdd_(char *jobz, f2c_int *m, f2c_int *n, f2c_dbl *a, f2c_int *lda, f2c_dbl *s, f2c_dbl *u, f2c_int *ldu, f2c_dbl *vt, f2c_int *ldvt, f2c_dbl *work, f2c_int *lwork, f2c_int *iwork, f2c_int *info);
extern int dgesv_ (f2c_int *n, f2c_int *nrhs, f2c_dbl *a, f2c_int *lda, f2c_int *ipiv, f2c_dbl *b, f2c_int *ldb, f2c_int *info);
extern int dgetrf_(f2c_int *m, f2c_int *n, f2c_dbl *a, f2c_int *lda, f2c_int *ipiv, f2c_int *info);
extern int dgetri_(f2c_int *n, f2c_dbl *a, f2c_int *lda, f2c_int *ipiv, f2c_dbl *work, f2c_int *lwork, f2c_int *info);
extern int dpotrf_(char *uplo, f2c_int *n, f2c_dbl *a, f2c_int *lda, f2c_int *info);
extern int dpotri_(char *uplo, f2c_int *n, f2c_dbl *a, f2c_int *lda, f2c_int *info);
extern int dgemm_ (char *transa, char *transb, f2c_int *m, f2c_int *n, f2c_int *k, f2c_dbl *alpha, f2c_dbl *a, f2c_int *lda, f2c_dbl *b, f2c_int *ldb, f2c_dbl *beta, f2c_dbl *c, f2c_int *ldc);
extern int dgemv_ (char *trans, f2c_int *m, f2c_int *n, f2c_dbl *alpha, f2c_dbl *a, f2c_int *lda, f2c_dbl *x, f2c_int *incx, f2c_dbl *beta, f2c_dbl *y, f2c_int *incy);

#define REQUIRE_COLMAJOR(layout) do{ if((layout)!=LAPACK_COL_MAJOR) return -1; }while(0)

lapack_int LAPACKE_dgels(int layout, char trans, lapack_int m, lapack_int n,
                         lapack_int nrhs, double *a, lapack_int lda,
                         double *b, lapack_int ldb)
{
    REQUIRE_COLMAJOR(layout);
    f2c_int info=0, lwork=-1; f2c_dbl wq;
    dgels_(&trans,&m,&n,&nrhs,a,&lda,b,&ldb,&wq,&lwork,&info);
    if(info) return info;
    lwork=(f2c_int)wq;
    f2c_dbl *work=malloc((size_t)lwork*sizeof *work);
    if(!work) return -1010;      /* LAPACKE work-alloc failure code */
    dgels_(&trans,&m,&n,&nrhs,a,&lda,b,&ldb,work,&lwork,&info);
    free(work);
    return info;
}

lapack_int LAPACKE_dgelsd(int layout, lapack_int m, lapack_int n,
                          lapack_int nrhs, double *a, lapack_int lda,
                          double *b, lapack_int ldb, double *s, double rcond,
                          lapack_int *rank)
{
    REQUIRE_COLMAJOR(layout);
    f2c_int info=0, lwork=-1, iwq=0; f2c_dbl wq;
    dgelsd_(&m,&n,&nrhs,a,&lda,b,&ldb,s,&rcond,rank,&wq,&lwork,&iwq,&info);
    if(info) return info;
    lwork=(f2c_int)wq;
    /* iwork: query fills iwq on modern LAPACK; belt-and-braces lower bound
     * 3*minmn*nlvl + 11*minmn with nlvl capped generously at 32. */
    f2c_int minmn = m<n?m:n; if(minmn<1) minmn=1;
    f2c_int liwork = iwq>0 ? iwq : 3*minmn*32 + 11*minmn;
    f2c_dbl *work = malloc((size_t)lwork*sizeof *work);
    f2c_int *iwork = malloc((size_t)liwork*sizeof *iwork);
    if(!work||!iwork){ free(work); free(iwork); return -1010; }
    dgelsd_(&m,&n,&nrhs,a,&lda,b,&ldb,s,&rcond,rank,work,&lwork,iwork,&info);
    free(work); free(iwork);
    return info;
}

lapack_int LAPACKE_dgeqp3(int layout, lapack_int m, lapack_int n, double *a,
                          lapack_int lda, lapack_int *jpvt, double *tau)
{
    REQUIRE_COLMAJOR(layout);
    f2c_int info=0, lwork=-1; f2c_dbl wq;
    dgeqp3_(&m,&n,a,&lda,jpvt,tau,&wq,&lwork,&info);
    if(info) return info;
    lwork=(f2c_int)wq;
    f2c_dbl *work=malloc((size_t)lwork*sizeof *work);
    if(!work) return -1010;
    dgeqp3_(&m,&n,a,&lda,jpvt,tau,work,&lwork,&info);
    free(work);
    return info;
}

lapack_int LAPACKE_dgesdd(int layout, char jobz, lapack_int m, lapack_int n,
                          double *a, lapack_int lda, double *s, double *u,
                          lapack_int ldu, double *vt, lapack_int ldvt)
{
    REQUIRE_COLMAJOR(layout);
    f2c_int info=0, lwork=-1; f2c_dbl wq;
    f2c_int minmn = m<n?m:n; if(minmn<1) minmn=1;
    f2c_int *iwork = malloc((size_t)(8*minmn)*sizeof *iwork);
    if(!iwork) return -1010;
    dgesdd_(&jobz,&m,&n,a,&lda,s,u,&ldu,vt,&ldvt,&wq,&lwork,iwork,&info);
    if(info){ free(iwork); return info; }
    lwork=(f2c_int)wq;
    f2c_dbl *work=malloc((size_t)lwork*sizeof *work);
    if(!work){ free(iwork); return -1010; }
    dgesdd_(&jobz,&m,&n,a,&lda,s,u,&ldu,vt,&ldvt,work,&lwork,iwork,&info);
    free(work); free(iwork);
    return info;
}

lapack_int LAPACKE_dgesv(int layout, lapack_int n, lapack_int nrhs, double *a,
                         lapack_int lda, lapack_int *ipiv, double *b,
                         lapack_int ldb)
{
    REQUIRE_COLMAJOR(layout);
    f2c_int info=0;
    dgesv_(&n,&nrhs,a,&lda,ipiv,b,&ldb,&info);
    return info;
}

lapack_int LAPACKE_dgetrf(int layout, lapack_int m, lapack_int n, double *a,
                          lapack_int lda, lapack_int *ipiv)
{
    REQUIRE_COLMAJOR(layout);
    f2c_int info=0;
    dgetrf_(&m,&n,a,&lda,ipiv,&info);
    return info;
}

lapack_int LAPACKE_dgetri(int layout, lapack_int n, double *a, lapack_int lda,
                          const lapack_int *ipiv)
{
    REQUIRE_COLMAJOR(layout);
    f2c_int info=0, lwork=-1; f2c_dbl wq;
    dgetri_(&n,a,&lda,(f2c_int*)ipiv,&wq,&lwork,&info);
    if(info) return info;
    lwork=(f2c_int)wq;
    f2c_dbl *work=malloc((size_t)lwork*sizeof *work);
    if(!work) return -1010;
    dgetri_(&n,a,&lda,(f2c_int*)ipiv,work,&lwork,&info);
    free(work);
    return info;
}

lapack_int LAPACKE_dpotrf(int layout, char uplo, lapack_int n, double *a,
                          lapack_int lda)
{
    REQUIRE_COLMAJOR(layout);
    f2c_int info=0;
    dpotrf_(&uplo,&n,a,&lda,&info);
    return info;
}

lapack_int LAPACKE_dpotri(int layout, char uplo, lapack_int n, double *a,
                          lapack_int lda)
{
    REQUIRE_COLMAJOR(layout);
    f2c_int info=0;
    dpotri_(&uplo,&n,a,&lda,&info);
    return info;
}

/* ---- CBLAS ------------------------------------------------------------ */
static char trans_char(CBLAS_TRANSPOSE t){ return t==CblasNoTrans ? 'N' : 'T'; }

void cblas_dgemm(CBLAS_ORDER order, CBLAS_TRANSPOSE ta, CBLAS_TRANSPOSE tb,
                 int m, int n, int k, double alpha, const double *a, int lda,
                 const double *b, int ldb, double beta, double *c, int ldc)
{
    if(order!=CblasColMajor) return;             /* tea never uses row-major */
    char ca=trans_char(ta), cb=trans_char(tb);
    f2c_int fm=m,fn=n,fk=k,flda=lda,fldb=ldb,fldc=ldc;
    dgemm_(&ca,&cb,&fm,&fn,&fk,&alpha,(f2c_dbl*)a,&flda,(f2c_dbl*)b,&fldb,&beta,c,&fldc);
}

void cblas_dgemv(CBLAS_ORDER order, CBLAS_TRANSPOSE ta, int m, int n,
                 double alpha, const double *a, int lda, const double *x,
                 int incx, double beta, double *y, int incy)
{
    if(order!=CblasColMajor) return;
    char ca=trans_char(ta);
    f2c_int fm=m,fn=n,flda=lda,fincx=incx,fincy=incy;
    dgemv_(&ca,&fm,&fn,&alpha,(f2c_dbl*)a,&flda,(f2c_dbl*)x,&fincx,&beta,y,&fincy);
}
#endif /* __EMSCRIPTEN__ */

/* keep the translation unit non-empty in native builds */
typedef int tea_wasm_linalg_placeholder;
