#include <string.h>
#include <math.h>
#include "mex.h"
#include "matrix.h"
#include "cinterp_common.h"

/* stopping criteria
   1 : stop when any nonzero that would be set would be less than tol^2/2
   2 : stop when the sum, over all possible nonzeros, of the value
       they would be set to, is less than tol/2
*/
#ifndef STOP_TEST
#  define STOP_TEST 2
#endif

/*--------------------------------------------------------------------------
   sparse matrix-vector multiply
   
   y := A * x
   
   the sparse vector x may have unsorted indices
   the sparse output y will have sorted indices
   the return value is nnz(y)
   y is set to 0 where mask >= 0
   
   yi, y assumed big enough to store the result
   mask, sv, flag must be of length A->m
   flag must be 0 on input and will be 0 on output
----------------------------------------------------------------------------
   Implementation:
   
   A non-sparse version of y is built by summing columns of A times the
   corresponding x component. The components of y that get set are added
   to a heap structure, which gets sorted at the end. "flag" records which
   components have already been set, so that the heap will not contain
   duplicates.
--------------------------------------------------------------------------*/
static mwSize sp_mv(mwIndex *yi, double *y, const sp_mat_c *A,
                    mwSize xn, const mwIndex *xi, const double *x,
                    double *sv, char *flag, const mwSignedIndex *mask)
{
  mwSize yn=0; mwIndex *heap = yi-1, hi;
  const mwIndex *const xe=xi+xn;
  for(;xi!=xe;++xi,++x) {
    mwIndex j=*xi; double xj=*x;
    mwIndex Ajc = A->jc[j];
    const mwIndex *Air = &A->ir[Ajc], *const Aie = &A->ir[A->jc[j+1]];
    const double *Apr = &A->pr[Ajc];
    if(fabs(xj) == 0.0) continue;
    for(;Air!=Aie;++Air,++Apr) {
      mwIndex i = *Air;
#ifndef IGNORE_MASK
      if(mask[i]>=0) continue;
#endif
      if(flag[i]==0) {
        mwIndex hole = ++yn;
        while(hole>1) {
          mwIndex parent=hole>>1, ip=heap[parent];
          if(i<ip) break;
          heap[hole]=ip, hole=parent;
        }
        heap[hole]=i;
        flag[i]=1, sv[i]=0;
      }
      sv[i] += (*Apr)*xj;
    }
  }
  for(hi=yn;hi>1;--hi) {
    mwIndex i=heap[hi], hole=1;
    heap[hi] = heap[1];
    /* heap size = hi - 1 */
    for(;;) {
      mwIndex child=hole<<1, r=child+1, ic;
      if(r<hi && heap[r]>heap[child]) child=r;
      if(child>=hi || (ic=heap[child],i>=ic)) break;
      heap[hole]=ic, hole=child;
    }
    heap[hole]=i;
  }
  for(hi=0;hi<yn;++hi) y[hi]=sv[yi[hi]], flag[yi[hi]]=0;
  return yn;
}

/* sets r := x - alpha * y,    with r,x,y all sparse
   also, sets beta := beta + y .* y,
       where beta is not sparse,
       but is only initialized where x is defined
   sets r to 0 where mask >= 0
*/
static mwSize resid_update(mwIndex *ri, double *rp, double *beta,
                           mwSize xn, const mwIndex *xi, const double *xp,
                           double alpha,
                           mwSize yn, const mwIndex *yi, const double *yp,
                           const mwSignedIndex *mask)
{
  const mwIndex *const xe = xi+xn, *const ye = yi+yn;
  mwSize rnz = 0;
  if(xi!=xe && yi!=ye) {
    mwIndex ix=*xi, iy=*yi;
    for(;;) {
      if(ix<iy) {
#ifndef IGNORE_MASK
        if(mask[ix]<0)
#endif        
          ++rnz, *ri++=ix, *rp++ = *xp;
        ++xi,++xp; if(xi==xe) break; ix=*xi;
      } else if(ix>iy) {
        double y = *yp;
        beta[iy] = y*y;
#ifndef IGNORE_MASK
        if(mask[iy]<0)
#endif        
          ++rnz, *ri++=iy, *rp++ = -alpha*y;
        ++yi,++yp; if(yi==ye) break; iy=*yi;
      } else { /* ix==iy */
        double y = *yp;
        beta[iy] += y*y;
#ifndef IGNORE_MASK
        if(mask[iy]<0)
#endif        
          ++rnz, *ri++=iy, *rp++ = *xp - alpha*y;
        ++xi,++xp; ++yi,++yp;
        if(xi==xe || yi==ye) break;
        ix=*xi, iy=*yi;
      }
    }
  }
  while(xi!=xe) {
    mwIndex ix = *xi++; double x = *xp++;
#ifndef IGNORE_MASK
    if(mask[ix]<0)
#endif        
      ++rnz, *ri++ = ix, *rp++ = x;
  }
  while(yi!=ye) {
    mwIndex iy = *yi++; double y = *yp++;
    beta[iy] = y*y;
#ifndef IGNORE_MASK
    if(mask[iy]<0)
#endif        
      ++rnz, *ri++ = iy, *rp++ = -alpha*y;
  }
  return rnz;
}

#if DEBUG_LEVEL > 0
static void print_vec(const char *name, int n, const int *i, const double *p)
{
  int k;
  mexPrintf("%s = [",name);
  for(k=0;k<n;++k) mexPrintf(" (%d, %g)",i[k]+1,p[k]);
  mexPrintf(" ]\n");
}
static void print_veci(const char *name, int n, const int *i)
{
  int k;
  mexPrintf("%s = [",name);
  for(k=0;k<n;++k) mexPrintf(" %d",i[k]+1);
  mexPrintf(" ]\n");
}
#endif

static void heap_sort(mwSize un, mwIndex *uv)
{
  mwIndex *heap = uv-1, i;
  for(i=2;i<=un;++i) if(heap[i]>heap[i>>1]) {
    mwIndex hole=i, v=heap[i];
    do {
      mwIndex parent=hole>>1, vp=heap[parent];
      if(v<vp) break;
      heap[hole]=vp, hole=parent;
    } while(hole>1);
    heap[hole]=v;
  }
  for(i=un;i>1;--i) {
    mwIndex v=heap[i], hole=1;
    heap[i] = heap[1];
    /* heap size = i - 1 */
    for(;;) {
      mwIndex child=hole<<1, r=child+1, vc;
      if(r<i && heap[r]>heap[child]) child=r;
      if(child>=i || (vc=heap[child],v>=vc)) break;
      heap[hole]=vc, hole=child;
    }
    heap[hole]=v;
  }
}

/* computes, column-wise, a sparse minimizer X of
  
   f = .5 X^t A X - B^t X
  
   assumes D = diag(A)
   sets X_skel := sparsity pattern of X
        X_sum := X * u
   tol controls sparsity */
static void interp_skel(sp_log_mat *X_skel, double *X_sum,
                        const sp_mat_c *A, const sp_mat_c *B,
                        const double *D, const double *u, double tol)
{
  mwSize nf = B->m, nc = B->n;
  mwIndex iri, *irp, *jcp; /* irp = &W->ir[iri], jcp = &W->jc[j] */
  mwIndex j;
  
  int max_Q; double *Q; /* local triangular A-orthonormal basis */
  double *beta;         /* beta_i = || Q^t A e_i ||^2,
                           only defined where r is nonzero */
  mwSize rnz; mwIndex *ri; double *rp; /* residual r = (I - A Q Q^t) B e_j */
  mwSize Aqk_nz; mwIndex *Aqk_i; double *Aqk_p; /* A Q e_k .... (almost) */
  mwSize snz; mwIndex *si; double *sp; /* sparse scratch vector */
  mwSignedIndex *map_to_Qi;
    /* the inverse of the map Qi[k], -1 where not defined */
  char *flag; /* used by sp_mv; always zero outside that routine */
  
#if STOP_TEST == 1
  tol *= .5*tol;
#elif STOP_TEST == 2
  tol *= .5;
#endif

  /* initialize the X_skel structure */
  X_skel->m = nf, X_skel->n = nc;
  X_skel->nzmax = 2*B->jc[nc]; /* initial guess: nnz(X_skel) = 2*nnz(B) */
  X_skel->jc = mem_alloc((X_skel->n+1)*sizeof(mwIndex));
  X_skel->ir = mem_alloc(X_skel->nzmax*sizeof(mwIndex));
  
  /* allocate memory */
  beta = mem_alloc(4*nf*sizeof(double));
  rp = beta+nf, Aqk_p=rp+nf, sp=Aqk_p+nf;
  ri = mem_alloc(4*nf*sizeof(mwIndex));
  Aqk_i = ri+nf, si=Aqk_i+nf, map_to_Qi=si+nf;
  flag = mem_alloc(nf*sizeof(char));
  /* initial guess: no column will have nnz > 35 */
  max_Q=35; Q = mem_alloc(max_Q*(max_Q+1)*sizeof(double)/2);
  
  /* initialize work arrays */
  memset(flag,0,nf*sizeof(char));
  for(j=0;j<nf;++j) map_to_Qi[j] = -1;
  for(j=0;j<nf;++j) X_sum[j]=0;

  irp = X_skel->ir, iri=0; jcp = X_skel->jc;
  for(j=0;j<nc;++j) { /* working on column j of X */
    int m; /* multi-use loop index */
    mwIndex mm;
    int k=0; /* working on the (k+1)th nonzero for this column,
                Q is k x k upper-triangular,
                and we are filling in the (k+1)th column */
    mwIndex s;   /* the new nonzero (index) */
    double norm; /* checked against tol to stop */
    double w; /* value of X_sj  (s is chosen to maximize this) */
    mwIndex *Qi = irp; /* list of nonzero indices, Qi[0:k], Qi[k]=s */
    double *qk = Q; /* the (k+1)th column of Q, to be filled in */

    *jcp++ = iri; /* record start of column j in X_skel */
    
    /* initial residual = B e_j */
    rnz = B->jc[j+1]-B->jc[j];
    if(rnz<1) continue;

#ifdef VERBOSE_PROGRESS    
    mexPrintf("  column %10d: ",j);
#endif

    memcpy(ri, &B->ir[B->jc[j]], rnz*sizeof(mwIndex));
    memcpy(rp, &B->pr[B->jc[j]], rnz*sizeof(double));
    /* initialize beta, and use residual to find s, norm */
    s=ri[0], beta[s]=0, w=rp[0]/sqrt(D[s]), norm=fabs(rp[0]/D[s]);
    for(m=1;m<rnz;++m) {
      mwIndex i=ri[m]; double r=rp[m],d=D[i];
      double tw=r/sqrt(d), tn=fabs(r/d);
      beta[i]=0;
      if(fabs(tw)>fabs(w)) w=tw,s=i;
#if STOP_TEST == 1
      if(tn>norm) norm=tn;
#elif STOP_TEST ==2
      norm+=tn;
#endif
    } 
    while(norm>tol) {
#ifdef VERBOSE_PROGRESS    
      mexPrintf(".");
#endif
      /* check if we underestimated nnz(X_skel) */
      if(iri==X_skel->nzmax) {
        ptrdiff_t d = Qi - X_skel->ir;
        X_skel->nzmax*=2;
        X_skel->ir=mem_realloc(X_skel->ir,X_skel->nzmax*sizeof(mwIndex));
        irp=X_skel->ir+iri, Qi=X_skel->ir+d;
      }
      /* check if we underestimated dim(Q) */
      if(k+1>max_Q) {
        ptrdiff_t d = qk-Q;
        max_Q*=2, Q=mem_realloc(Q,max_Q*(max_Q+1)*sizeof(double)/2);
        qk = Q+d;
      }
      /* record new non-zero in W, (which updates Qi), update the inverse map */
      *irp++=s, ++iri; map_to_Qi[s]=k;
#if DEBUG_LEVEL > 2
      mexPrintf("(%d, %d)\n",j+1,s+1);
#endif
      /* calculate Q Q^t A e_s */
      sp_restrict_unsorted(qk, k,map_to_Qi, A->jc[s+1]-A->jc[s],
        &A->ir[A->jc[s]], &A->pr[A->jc[s]]); 
      mv_utt(sp, k,Q, qk);
#if DEBUG_LEVEL > 4
      print_vec("Q^t A e_s",k,Qi,sp);
#endif
      mv_ut(qk, k,Q, sp);
#if DEBUG_LEVEL > 4
      print_vec("Q Q^t A e_s",k,Qi,qk);
#endif
      /* set Q e_k = qk := alpha^{-1} (I - Q Q^t A) e_s */
      { double norm_fac = -1. / sqrt(D[s] - beta[s]);
        for(m=0;m<k;++m) qk[m] *= norm_fac;
        qk[k] = -norm_fac;
      }
#if DEBUG_LEVEL > 3
      print_vec("Q e_k",k+1,Qi,qk);
#endif
      /* X e_j += w Q e_k,  so sum_j u_j X e_j += u_j w Q e_k */
      { double ujw = u[j]*w;
        for(m=0;m<=k;++m) X_sum[Qi[m]] += ujw*qk[m];
      }
      /* calculate A qk */
      /*   note: due to using the mask map_to_Qi,
             e_s^t Aqk = 0,
           whereas
             e_s^t A Q e_k = sqrt(D[s] - beta[s])
           but e_s^t r will be 0, so we don't care
           the other masked components are the previous nonzeros,
             for which A Q e_k is 0 because Q is A-orthogonal */
      Aqk_nz = sp_mv(Aqk_i,Aqk_p, A, k+1,Qi,qk, sp,flag,map_to_Qi);
#if DEBUG_LEVEL > 4
      print_vec("A Q e_k",Aqk_nz,Aqk_i,Aqk_p);
#endif
      /* r := r - w Aqk, beta := beta + Aqk .* Aqk */
      memcpy(si, ri, rnz*sizeof(mwIndex));
      memcpy(sp, rp, rnz*sizeof(double));
      rnz = resid_update(ri,rp, beta, rnz,si,sp, w, Aqk_nz,Aqk_i,Aqk_p,
                         map_to_Qi); /* the mask ensures e_s^t r = 0 */
#if DEBUG_LEVEL > 3
      print_vec("r",rnz,ri,rp);
#endif
      /* find best s, recompute norm */
      if(rnz>0)
        s=ri[0], w=rp[0]/sqrt(D[s]-beta[s]), norm=fabs(rp[0]/(D[s]-beta[s]));
      else
        norm = 0;
      for(mm=1;mm<rnz;++mm) {
        mwIndex i=ri[mm]; double r=rp[mm],d=D[i]-beta[i];
        double tn=fabs(r/d), tw=r/sqrt(d);
        if(fabs(tw)>fabs(w)) w=tw,s=i;
#if STOP_TEST == 1
        if(tn>norm) norm=tn;
#elif STOP_TEST ==2
        norm+=tn;
#endif
      }
      ++k, qk+=k;
    }
    heap_sort(k,Qi); /* sort the non-zero indices for column j */
#if DEBUG_LEVEL > 1
    print_veci("Qi",k,Qi);
#endif
    for(m=0;m<k;++m) map_to_Qi[Qi[m]] = -1;
#ifdef VERBOSE_PROGRESS
    mexPrintf("\n");
#endif
  }
  *jcp++ = iri;
  mem_free(Q); mem_free(flag); mem_free(ri); mem_free(beta);  
}

/* A, B, D, u, tol */
void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  sp_mat_c A,B; const double *D, *u; double tol;
  sp_log_mat X_skel; mwSize nz;
  if(nrhs!=5) { mexWarnMsgTxt("Five inputs required."); return; }
  if(nlhs!=2) { mexWarnMsgTxt("Two outputs required."); return; }
  if(!mxIsSparse(prhs[0]) || !mxIsDouble(prhs[0]) || mxIsComplex(prhs[0])) {
    mexWarnMsgTxt("First input not a sparse, double, real matrix."); return;
  }
  if(!mxIsSparse(prhs[1]) || !mxIsDouble(prhs[1]) || mxIsComplex(prhs[1])) {
    mexWarnMsgTxt("Second input not a sparse, double, real matrix."); return;
  }
  if(mxIsSparse(prhs[2]) || !mxIsDouble(prhs[2]) || mxIsComplex(prhs[2])) {
    mexWarnMsgTxt("Third input not a full, double, real array."); return;
  }
  if(mxIsSparse(prhs[3]) || !mxIsDouble(prhs[3]) || mxIsComplex(prhs[3])) {
    mexWarnMsgTxt("Fourth input not a full, double, real array."); return;
  }
  if(mxIsSparse(prhs[4]) || !mxIsDouble(prhs[4]) || mxIsComplex(prhs[4])
   ||mxGetM(prhs[4])!=1  || mxGetN(prhs[4])!=1) {
    mexWarnMsgTxt("Fifth input not a double real scalar."); return;
  }
  A.m = mxGetM(prhs[0]), A.n = mxGetN(prhs[0]);
  A.ir = mxGetIr(prhs[0]), A.jc = mxGetJc(prhs[0]), A.pr = mxGetPr(prhs[0]);
  B.m = mxGetM(prhs[1]), B.n = mxGetN(prhs[1]);
  B.ir = mxGetIr(prhs[1]), B.jc = mxGetJc(prhs[1]), B.pr = mxGetPr(prhs[1]);
  D = mxGetPr(prhs[2]);
  u = mxGetPr(prhs[3]);
  tol = mxGetPr(prhs[4])[0];
  if(A.m!=A.n) { mexWarnMsgTxt("A not square."); return; }
  if(A.m!=B.m) { mexWarnMsgTxt("rows(A) != rows(B)"); return; }
  if(mxGetM(prhs[2])!=A.m || mxGetN(prhs[2])!=1) {
    mexWarnMsgTxt("D not a column vector, or rows(D) != rows(A)"); return;
  }
  if(mxGetM(prhs[3])!=B.n || mxGetN(prhs[3])!=1) {
    mexWarnMsgTxt("u not a column vector, or rows(u) != cols(B)"); return;
  }
  plhs[1] = mxCreateDoubleMatrix(B.m,1,mxREAL);
  interp_skel(&X_skel,mxGetPr(plhs[1]),&A,&B,D,u,tol); nz = X_skel.jc[X_skel.n];
  plhs[0] = mxCreateSparseLogicalMatrix(B.m,B.n,nz);
  memcpy(mxGetJc(plhs[0]),X_skel.jc,(X_skel.n+1)*sizeof(mwIndex));
  memcpy(mxGetIr(plhs[0]),X_skel.ir,nz*sizeof(mwIndex));
  { mwIndex i;
    mxLogical *l = mxGetLogicals(plhs[0]); for(i=0;i<nz;++i) l[i]=1; }
  mem_free(X_skel.ir); mem_free(X_skel.jc);
}
