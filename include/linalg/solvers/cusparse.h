#pragma once

#ifdef USE_GPU

#include "../../cuda_utils.h"
#include "cublas_v2.h"
#include <assert.h>
#include <cuda_runtime.h>
#include <cusparse_v2.h>
#include <iostream>

namespace CUSPARSE {

template <typename T>
void direct_LU_solve(BsrMat<DeviceVec<T>> &mat, DeviceVec<T> &rhs,
                     DeviceVec<T> &soln) {

    // copy important inputs for Bsr structure out of BsrMat
    // TODO : was trying to make some of these const but didn't accept it in
    // final solve
    BsrData bsr_data = mat.getBsrData();
    int mb = bsr_data.nnodes;
    int nnzb = bsr_data.nnzb;
    int blockDim = bsr_data.block_dim;
    int *d_rowPtr = bsr_data.rowPtr;
    int *d_colPtr = bsr_data.colPtr;
    T *d_rhs = rhs.getPtr();
    T *d_soln = soln.getPtr();
    DeviceVec<T> temp = DeviceVec<T>(soln.getSize());
    T *d_temp = temp.getPtr();

    // https://developer.nvidia.com/blog/accelerated-solution-sparse-linear-systems/

    // copy kmat data vec since gets modified during LU
    // otherwise we can't compute residual properly K * u - f
    T *d_values = mat.getVec().copyVec().getPtr();

    /*
    Cusparse documentation
    The function cusparseSpSM_bufferSize() returns the size of the workspace
    needed by cusparseSpSM_analysis() and cusparseSpSM_solve(). The function
    cusparseSpSM_analysis() performs the analysis phase, while
    cusparseSpSM_solve() executes the solve phase for a sparse triangular linear
    system. The opaque data structure spsmDescr is used to share information
    among all functions. The function cusparseSpSM_updateMatrix() updates
    spsmDescr with new matrix values.
    */

    // Initialize cuSPARSE handle
    cusparseHandle_t handle;
    CHECK_CUSPARSE(cusparseCreate(&handle));

    // Create a cuSPARSE matrix descriptor
    cusparseSpMatDescr_t matA;
    CHECK_CUSPARSE(cusparseCreateBsr(
        &matA, mb, mb, nnzb, blockDim, blockDim, d_rowPtr, d_colPtr, d_values,
        CUSPARSE_INDEX_32I, CUSPARSE_INDEX_32I, CUSPARSE_INDEX_BASE_ZERO,
        CUDA_R_64F, CUSPARSE_ORDER_ROW));

    // TODO : need to manually convert from BSR to CSR myself before doing
    // this.. temporarily convert to CSR format
    cusparseSpMatDescr_t matA_CSR;
    CHECK_CUSPARSE(cusparseCreateCsr(&matA_CSR, brows, bcols, nnz,
                                     csrRowOffsets, csrColInd, csrValues,
                                     CUSPARSE_INDEX_32I, CUSPARSE_INDEX_32I,
                                     CUSPARSE_INDEX_BASE_ZERO, CUDA_R_64F);)

    // temporarily convert the matrix to CSR for factorization?
    // I suppose we could do that here instead of C++ later..
    cusparseSpMatDescr_t matA_CSR;

    // Create a dense matrix descriptor for the right-hand side vector
    cusparseDnMatDescr_t matB;
    CHECK_CUSPARSE(cusparseCreateDnMat(&matB, mb, 1, mb, d_rhs, CUDA_R_64F,
                                       CUSPARSE_ORDER_ROW));

    // Create a dense matrix descriptor for the result vector
    cusparseDnMatDescr_t matC;
    CHECK_CUSPARSE(cusparseCreateDnMat(&matC, mb, 1, mb, d_soln, CUDA_R_64F,
                                       CUSPARSE_ORDER_ROW));

    // Create sparse matrix solve descriptor
    cusparseSpSMDescr_t spsmDescr;
    CHECK_CUSPARSE(cusparseSpSM_createDescr(&spsmDescr));

    // Choose algorithm for sparse matrix solve
    cusparseSpSMAlg_t alg = CUSPARSE_SPSM_ALG_DEFAULT;

    // create buffer size for LU factorization
    size_t bufferSize;
    double alpha = 1.0;
    const void *alpha_ptr = reinterpret_cast<const void *>(&alpha);

    CHECK_CUSPARSE(cusparseSpSM_bufferSize(
        handle, CUSPARSE_OPERATION_NON_TRANSPOSE,
        CUSPARSE_OPERATION_NON_TRANSPOSE, alpha_ptr, matA, matB, matC,
        CUDA_R_64F, alg, spsmDescr, &bufferSize));

    // create buffer for sparse matrix solve
    void *d_buffer;
    CHECK_CUDA(cudaMalloc(&d_buffer, bufferSize));

    // do analysis to get in A in LU format
    cusparseSpSM_analysis(handle, CUSPARSE_OPERATION_NON_TRANSPOSE,
                          CUSPARSE_OPERATION_NON_TRANSPOSE, alpha_ptr, matA,
                          matB, matC, CUDA_R_64F, alg, spsmDescr, d_buffer);

    CHECK_CUSPARSE(cusparseSpSM_solve(handle, CUSPARSE_OPERATION_NON_TRANSPOSE,
                                      CUSPARSE_OPERATION_NON_TRANSPOSE,
                                      alpha_ptr, matA, matB, matC, CUDA_R_64F,
                                      alg, spsmDescr));

    // CHECK_CUSPARSE(cusparseSpSM_analysis(
    //     handle, CUSPARSE_OPERATION_NON_TRANSPOSE,
    //     CUSPARSE_OPERATION_NON_TRANSPOSE, matA, nullptr, CUDA_R_64F,
    //     CUSPARSE_SPSM_ALG_DEFAULT, spSMDescr,
    //     &bufferSizeSM)); // spSMDescr, &bufferSizeSM) // nullptr,
    //     &bufferSizeSM)

    // // Allocate buffer for analysis
    // void *d_bufferSM;
    // CHECK_CUDA(cudaMalloc(&d_bufferSM, bufferSizeSM));

    // // LU analysis step
    // CHECK_CUSPARSE(cusparseSpSM_analysis(
    //     handle, CUSPARSE_OPERATION_NON_TRANSPOSE,
    //     CUSPARSE_OPERATION_NON_TRANSPOSE, matA, nullptr, CUDA_R_64F,
    //     CUSPARSE_SPSM_ALG_DEFAULT, spSMDescr, d_bufferSM));

    // // Create descriptors for L and U (triangular structure)
    // cusparseSpMatDescr_t matL, matU;

    // // Lower triangular matrix (L)
    // CHECK_CUSPARSE(cusparseCreateBlockedSparseMat(
    //     &matL, mb, mb, nnzb, d_rowPtr, d_colPtr, d_values, blockDim,
    //     CUSPARSE_INDEX_32I, CUSPARSE_INDEX_BASE_ZERO, CUDA_R_64F));
    // CHECK_CUSPARSE(cusparseSpMatSetAttribute(matL, CUSPARSE_SPMAT_TRIANGULAR,
    //                                          CUSPARSE_SPMAT_TRIANGULAR_LOWER));

    // // Upper triangular matrix (U)
    // CHECK_CUSPARSE(cusparseCreateBlockedSparseMat(
    //     &matU, mb, mb, nnzb, d_rowPtr, d_colPtr, d_values, blockDim,
    //     CUSPARSE_INDEX_32I, CUSPARSE_INDEX_BASE_ZERO, CUDA_R_64F));
    // CHECK_CUSPARSE(cusparseSpMatSetAttribute(matU, CUSPARSE_SPMAT_TRIANGULAR,
    //                                          CUSPARSE_SPMAT_TRIANGULAR_UPPER));

    // // Solution for L*y = f  (y is d_temp, f is d_rhs)
    // // Solution for U*x = y  (x is d_soln, y is d_rhs)

    // // Perform LU factorization (in place update of d_values)
    // CHECK_CUSPARSE(cusparseSpSM_solve(
    //     handle, CUSPARSE_OPERATION_NON_TRANSPOSE,
    //     CUSPARSE_OPERATION_NON_TRANSPOSE, matL, d_rhs, d_temp, CUDA_R_64F,
    //     CUSPARSE_SPSM_ALG_DEFAULT, spSMDescr, d_bufferSM));

    // CHECK_CUSPARSE(cusparseSpSM_solve(
    //     handle, CUSPARSE_OPERATION_NON_TRANSPOSE,
    //     CUSPARSE_OPERATION_NON_TRANSPOSE, matU, d_temp, d_soln, CUDA_R_64F,
    //     CUSPARSE_SPSM_ALG_DEFAULT, spSMDescr, d_bufferSM));

    // Cleanup
    CHECK_CUSPARSE(cusparseSpSM_destroyDescr(spsmDescr));
    CHECK_CUSPARSE(cusparseDestroyDnMat(matB));
    CHECK_CUSPARSE(cusparseDestroyDnMat(matC));
    CHECK_CUSPARSE(cusparseDestroySpMat(matA));
    CHECK_CUDA(cudaFree(d_buffer));

    // Destroy cuSPARSE handle
    CHECK_CUSPARSE(cusparseDestroy(handle));
}

void direct_LU_solve_old(BsrMat<DeviceVec<T>> &mat, DeviceVec<T> &rhs,
                         DeviceVec<T> &soln) {

    // copy important inputs for Bsr structure out of BsrMat
    // TODO : was trying to make some of these const but didn't accept it in
    // final solve
    BsrData bsr_data = mat.getBsrData();
    int mb = bsr_data.nnodes;
    int nnzb = bsr_data.nnzb;
    int blockDim = bsr_data.block_dim;
    int *d_bsrRowPtr = bsr_data.rowPtr;
    int *d_bsrColPtr = bsr_data.colPtr;
    T *d_rhs = rhs.getPtr();
    T *d_soln = soln.getPtr();
    DeviceVec<T> temp = DeviceVec<T>(soln.getSize());
    T *d_temp = temp.getPtr();

    // copy kmat data vec since gets modified during LU
    // otherwise we can't compute residual properly K * u - f
    // T *d_bsrVal = mat.getPtr();
    T *d_bsrVal = mat.getVec().copyVec().getPtr();
    // alternatively could maybe not use & in mat above so it copies
    // automatically? T *d_bsrVal = d_kmat_vec_copy.getPtr();

    // Initialize the cuda cusparse handle
    cusparseHandle_t handle;
    cusparseCreate(&handle);
    cusparseStatus_t status;

    // Constant scalar coefficienct
    const double alpha = 1.0;

    cusparseMatDescr_t descr_M = 0;
    cusparseMatDescr_t descr_L = 0;
    cusparseMatDescr_t descr_U = 0;
    bsrilu02Info_t info_M = 0;
    bsrsv2Info_t info_L = 0;
    bsrsv2Info_t info_U = 0;
    int pBufferSize_M;
    int pBufferSize_L;
    int pBufferSize_U;
    int pBufferSize;
    void *pBuffer = 0;
    int structural_zero;
    int numerical_zero;
    const cusparseSolvePolicy_t policy_M = CUSPARSE_SOLVE_POLICY_NO_LEVEL;
    const cusparseSolvePolicy_t policy_L = CUSPARSE_SOLVE_POLICY_NO_LEVEL;
    const cusparseSolvePolicy_t policy_U = CUSPARSE_SOLVE_POLICY_USE_LEVEL;
    const cusparseOperation_t trans_L = CUSPARSE_OPERATION_NON_TRANSPOSE;
    const cusparseOperation_t trans_U = CUSPARSE_OPERATION_NON_TRANSPOSE;
    const cusparseDirection_t dir = CUSPARSE_DIRECTION_ROW;

    // step 1: create a descriptor which contains
    cusparseCreateMatDescr(&descr_M);
    cusparseSetMatIndexBase(descr_M, CUSPARSE_INDEX_BASE_ZERO);
    cusparseSetMatType(descr_M, CUSPARSE_MATRIX_TYPE_GENERAL);
    cusparseCreateMatDescr(&descr_L);
    cusparseSetMatIndexBase(descr_L, CUSPARSE_INDEX_BASE_ZERO);
    cusparseSetMatType(descr_L, CUSPARSE_MATRIX_TYPE_GENERAL);
    cusparseSetMatFillMode(descr_L, CUSPARSE_FILL_MODE_LOWER);
    cusparseSetMatDiagType(descr_L, CUSPARSE_DIAG_TYPE_UNIT);
    cusparseCreateMatDescr(&descr_U);
    cusparseSetMatIndexBase(descr_U, CUSPARSE_INDEX_BASE_ZERO);
    cusparseSetMatType(descr_U, CUSPARSE_MATRIX_TYPE_GENERAL);
    cusparseSetMatFillMode(descr_U, CUSPARSE_FILL_MODE_UPPER);
    cusparseSetMatDiagType(descr_U, CUSPARSE_DIAG_TYPE_NON_UNIT);

    // step 2: create a empty info structure
    // we need one info for bsrilu02 and two info's for bsrsv2
    cusparseCreateBsrilu02Info(&info_M);
    cusparseCreateBsrsv2Info(&info_L);
    cusparseCreateBsrsv2Info(&info_U);

    // step 3: query how much memory used in bsrilu02 and bsrsv2, and allocate
    // the buffer
    cusparseDbsrilu02_bufferSize(handle, dir, mb, nnzb, descr_M, d_bsrVal,
                                 d_bsrRowPtr, d_bsrColPtr, blockDim, info_M,
                                 &pBufferSize_M);
    cusparseDbsrsv2_bufferSize(handle, dir, trans_L, mb, nnzb, descr_L,
                               d_bsrVal, d_bsrRowPtr, d_bsrColPtr, blockDim,
                               info_L, &pBufferSize_L);
    cusparseDbsrsv2_bufferSize(handle, dir, trans_U, mb, nnzb, descr_U,
                               d_bsrVal, d_bsrRowPtr, d_bsrColPtr, blockDim,
                               info_U, &pBufferSize_U);
    pBufferSize = max(pBufferSize_M, max(pBufferSize_L, pBufferSize_U));
    // pBuffer returned by cudaMalloc is automatically aligned to 128 bytes.
    cudaMalloc((void **)&pBuffer, pBufferSize);

    // step 4: perform analysis of incomplete LU factorization on M
    //     perform analysis of triangular solve on L
    //     perform analysis of triangular solve on U
    //     The lower(upper) triangular part of M has the same sparsity pattern
    //     as L(U), we can do analysis of bsrilu0 and bsrsv2 simultaneously.
    //
    // Notes:
    // bsrilu02_analysis() ->
    //   Executes the 0 fill-in ILU with no pivoting
    //
    // cusparseXbsrilu02_zeroPivot() ->
    //   is a blocking call. It calls
    //   cudaDeviceSynchronize() to make sure all previous kernels are done.
    //
    // cusparseDbsrsv2_analysis() ->
    //   output is the info structure filled with information collected
    //   during he analysis phase (that should be passed to the solve phase
    //   unchanged).
    //
    // The variable "info" contains the structural zero or numerical zero

    cusparseDbsrilu02_analysis(handle, dir, mb, nnzb, descr_M, d_bsrVal,
                               d_bsrRowPtr, d_bsrColPtr, blockDim, info_M,
                               policy_M, pBuffer);
    status = cusparseXbsrilu02_zeroPivot(handle, info_M, &structural_zero);
    if (CUSPARSE_STATUS_ZERO_PIVOT == status) {
        printf("A(%d,%d) is missing\n", structural_zero, structural_zero);
    }
    cusparseDbsrsv2_analysis(handle, dir, trans_L, mb, nnzb, descr_L, d_bsrVal,
                             d_bsrRowPtr, d_bsrColPtr, blockDim, info_L,
                             policy_L, pBuffer);
    cusparseDbsrsv2_analysis(handle, dir, trans_U, mb, nnzb, descr_U, d_bsrVal,
                             d_bsrRowPtr, d_bsrColPtr, blockDim, info_U,
                             policy_U, pBuffer);

    // step 5: M = L * U
    cusparseDbsrilu02(handle, dir, mb, nnzb, descr_M, d_bsrVal, d_bsrRowPtr,
                      d_bsrColPtr, blockDim, info_M, policy_M, pBuffer);
    status = cusparseXbsrilu02_zeroPivot(handle, info_M, &numerical_zero);
    if (CUSPARSE_STATUS_ZERO_PIVOT == status) {
        printf("block U(%d,%d) is not invertible\n", numerical_zero,
               numerical_zero);
    }

    // step 6: solve L*z = x
    cusparseDbsrsv2_solve(handle, dir, trans_L, mb, nnzb, &alpha, descr_L,
                          d_bsrVal, d_bsrRowPtr, d_bsrColPtr, blockDim, info_L,
                          d_rhs, d_temp, policy_L, pBuffer);

    // step 7: solve U*y = z
    cusparseDbsrsv2_solve(handle, dir, trans_U, mb, nnzb, &alpha, descr_U,
                          d_bsrVal, d_bsrRowPtr, d_bsrColPtr, blockDim, info_U,
                          d_temp, d_soln, policy_U, pBuffer);

    // print out d_soln
    // cudaMemcpy

    // step 8: free resources
    cudaFree(pBuffer);
    cusparseDestroyMatDescr(descr_M);
    cusparseDestroyMatDescr(descr_L);
    cusparseDestroyMatDescr(descr_U);
    cusparseDestroyBsrilu02Info(info_M);
    cusparseDestroyBsrsv2Info(info_L);
    cusparseDestroyBsrsv2Info(info_U);
    cusparseDestroy(handle);
}

template <typename T>
T get_resid(BsrMat<DeviceVec<T>> mat, DeviceVec<T> rhs, DeviceVec<T> soln) {

    BsrData bsr_data = mat.getBsrData();
    int mb = bsr_data.nnodes;
    int nnzb = bsr_data.nnzb;
    int blockDim = bsr_data.block_dim;
    int *d_bsrRowPtr = bsr_data.rowPtr;
    int *d_bsrColPtr = bsr_data.colPtr;
    T *d_rhs = rhs.getPtr();
    T *d_soln = soln.getPtr();
    DeviceVec<T> temp = DeviceVec<T>(soln.getSize());
    T *d_temp = temp.getPtr();
    T *d_bsrVal = mat.getPtr();

    // init cublas handle, etc.
    // cudaError_t cudaStat;
    cublasStatus_t blas_stat;
    cublasHandle_t cublas_handle;
    blas_stat = cublasCreate(&cublas_handle);

    // also make cusparse handle
    cusparseHandle_t cusparse_handle;
    cusparseCreate(&cusparse_handle);
    cusparseStatus_t cusparse_status;

    // Descriptor for the BSR matrix
    cusparseMatDescr_t descr;
    cusparseCreateMatDescr(&descr);
    cusparseSetMatType(descr, CUSPARSE_MATRIX_TYPE_GENERAL);
    cusparseSetMatIndexBase(descr, CUSPARSE_INDEX_BASE_ZERO);

    // Step 1: Perform the matrix-vector product: d_temp = K * u
    double alpha = 1.0, beta = 0.0;
    cusparse_status = cusparseDbsrmv(
        cusparse_handle, CUSPARSE_DIRECTION_ROW,
        CUSPARSE_OPERATION_NON_TRANSPOSE, mb, mb, nnzb, &alpha, descr, d_bsrVal,
        d_bsrRowPtr, d_bsrColPtr, blockDim, d_soln, &beta, d_temp);

    if (cusparse_status != CUSPARSE_STATUS_SUCCESS) {
        printf("CUSPARSE bsrmv failed!\n");
        return EXIT_FAILURE;
    }

    // Step 2: Compute the residual: d_temp = d_temp - f
    double alpha2 = -1.0;
    cublasDaxpy(cublas_handle, rhs.getSize(), &alpha2, d_rhs, 1, d_temp, 1);

    // Step 3: Compute max residual
    int maxIndex;
    double maxResidual;
    cublasIdamax(cublas_handle, rhs.getSize(), d_temp, 1, &maxIndex);

    int zeroBasedIndex = maxIndex - 1; // Idamax uses 1-based for some reason..
    cudaMemcpy(&maxResidual, d_temp + zeroBasedIndex, sizeof(double),
               cudaMemcpyDeviceToHost);

    // Optionally zero out the temp array
    // cudaMemset(d_temp, 0, numRows * sizeof(float));

    // Free resources
    cudaFree(d_temp);
    cusparseDestroyMatDescr(descr);

    return maxResidual;
}

}; // namespace CUSPARSE

#endif // USE_GPU