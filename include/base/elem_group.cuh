
#include "cuda_utils.h"

// base class methods to launch kernel depending on how many elements per block
// may override these in some base classes

// add_residual kernel
// -----------------------

template <typename T, class ElemGroup, class Data, int32_t elems_per_block = 1>
__GLOBAL__ void add_energy_gpu(int32_t num_elements, int32_t *geo_conn,
                               int32_t *vars_conn, T *xpts, T *vars,
                               Data *physData, T *Uenergy) {}

template <typename T, class ElemGroup, class Data, int32_t elems_per_block = 1,
          template <typename> class Vec>
__GLOBAL__ void
add_residual_gpu(const int32_t num_elements, const Vec<int32_t> geo_conn,
                 const Vec<int32_t> vars_conn, const Vec<T> xpts,
                 const Vec<T> vars, Vec<Data> physData, Vec<T> res) {
    
    // note in the above : CPU code passes Vec<> objects by reference
    // GPU kernel code cannot do so for complex objects otherwise weird behavior occurs

    using Geo = typename ElemGroup::Geo;
    using Basis = typename ElemGroup::Basis;
    using Phys = typename ElemGroup::Phys;

    // if you want to precompute some things?
    // __SHARED__ T geo_data[elems_per_block][Geo::geo_data_size];
    // __SHARED__ T basis_data[elems_per_block][Geo::geo_data_size];

    int local_elem = threadIdx.x;
    int global_elem = local_elem + blockDim.x * blockIdx.x;
    bool active_thread = global_elem < num_elements;
    int local_thread = (blockDim.x * blockDim.y) * threadIdx.z +
                       blockDim.x * threadIdx.y + threadIdx.x;

    const int nxpts_per_elem = Geo::num_nodes * Geo::spatial_dim;
    const int vars_per_elem = Basis::num_nodes * Phys::vars_per_node;

    const int32_t *_geo_conn = geo_conn.getPtr();
    const int32_t *_vars_conn = vars_conn.getPtr();
    const Data *_phys_data = physData.getPtr();

    __SHARED__ T block_xpts[elems_per_block][nxpts_per_elem];
    __SHARED__ T block_vars[elems_per_block][vars_per_elem];
    __SHARED__ T block_res[elems_per_block][vars_per_elem];
    __SHARED__ Data block_data[elems_per_block];

    // load data into block shared mem using some subset of threads
    const int32_t *geo_elem_conn = &_geo_conn[global_elem * Geo::num_nodes];
    xpts.copyValuesToShared(active_thread, threadIdx.y, blockDim.y,
                            Geo::spatial_dim, Geo::num_nodes, geo_elem_conn,
                            &block_xpts[local_elem][0]);

    const int32_t *vars_elem_conn = &_vars_conn[global_elem * Basis::num_nodes];
    vars.copyValuesToShared(active_thread, threadIdx.y, blockDim.y,
                            Phys::vars_per_node, Basis::num_nodes, vars_elem_conn,
                            &block_vars[local_elem][0]);

    if (active_thread) {
        memset(&block_res[local_elem][0], 0.0, vars_per_elem * sizeof(T));

        if (local_thread < elems_per_block) {
            int global_elem_thread = local_thread + blockDim.x * blockIdx.x;
            block_data[local_thread] = _phys_data[global_elem_thread];
        }
    }
    __syncthreads();

    // printf("<<<res GPU kernel>>>\n");

    int iquad = threadIdx.y;

    T local_res[vars_per_elem];
    memset(local_res, 0.0, sizeof(T) * vars_per_elem);

    // accessing value crashes the kernel.. (block_xpts not initialized right)
    // printf("block_xpts:");
    // printVec<T>(12,&block_xpts[local_elem][0]);

    ElemGroup::template add_element_quadpt_residual<Data>(
        active_thread, iquad, block_xpts[local_elem], block_vars[local_elem],
        block_data[local_elem], local_res);

    Vec<T>::copyLocalToShared(active_thread, vars_per_elem, &local_res[0],
                              &block_res[local_elem][0]);

    res.addValuesFromShared(active_thread, threadIdx.y, blockDim.y,
                            Phys::vars_per_node, Basis::num_nodes, vars_elem_conn,
                            &block_res[local_elem][0]);
} // end of add_residual_gpu kernel

// add jacobian kernel
// -------------------
template <typename T, class ElemGroup, class Data, int32_t elems_per_block = 1>
__GLOBAL__ static void add_jacobian_gpu(int32_t vars_num_nodes,
                                        int32_t num_elements, int32_t *geo_conn,
                                        int32_t *vars_conn, T *xpts, T *vars,
                                        Data *physData, T *residual, T *mat) {
    using Geo = typename ElemGroup::Geo;
    using Basis = typename ElemGroup::Basis;
    using Phys = typename ElemGroup::Phys;

    // if you want to precompute some things?
    // __SHARED__ T geo_data[elems_per_block][Geo::geo_data_size];
    // __SHARED__ T basis_data[elems_per_block][Geo::geo_data_size];

    int local_elem = threadIdx.x;
    int global_elem = local_elem + blockDim.x * blockIdx.x;
    bool active_thread = global_elem < num_elements;
    int local_thread = (blockDim.x * blockDim.y) * threadIdx.z +
                       blockDim.x * threadIdx.y + threadIdx.x;

    const int nxpts_per_elem = Geo::num_nodes * Geo::spatial_dim;
    const int vars_per_elem = Basis::num_nodes * Phys::vars_per_node;
    const int vars_per_elem2 = vars_per_elem * vars_per_elem;
    const int num_vars = vars_num_nodes * Phys::vars_per_node;

    // currently using 1416 T values per block on this element (want to be below
    // 6000 doubles shared mem)
    __SHARED__ T block_xpts[elems_per_block][nxpts_per_elem];
    __SHARED__ T block_vars[elems_per_block][vars_per_elem];
    __SHARED__ T block_res[elems_per_block][vars_per_elem];
    __SHARED__ T block_mat[elems_per_block][vars_per_elem2];
    __SHARED__ Data block_data[elems_per_block];

    int nthread_yz = blockDim.y * blockDim.z;
    int thread_yz = threadIdx.y * blockDim.z + threadIdx.z;

    if (active_thread) { // this version copies same data 3 times (improve
        // this..)

        // would be better if memory access is more in adjacent in memory
        for (int ixpt = thread_yz; ixpt < nxpts_per_elem; ixpt += nthread_yz) {
            int local_inode = ixpt / Geo::spatial_dim;
            int local_idim = ixpt % Geo::spatial_dim;
            const int global_node_ind =
                geo_conn[global_elem * Geo::num_nodes + local_inode];
            int global_ixpt = Geo::spatial_dim * global_node_ind + local_idim;
            block_xpts[local_elem][ixpt] = xpts[global_ixpt];
        }

        for (int idof = thread_yz; idof < vars_per_elem; idof += nthread_yz) {
            int local_inode = idof / Phys::vars_per_node;
            int local_idof = idof % Phys::vars_per_node;
            const int global_node_ind =
                vars_conn[global_elem * Basis::num_nodes + local_inode];
            int global_idof =
                Phys::vars_per_node * global_node_ind + local_idof;

            block_vars[local_elem][idof] = vars[global_idof];
            block_res[local_elem][idof] = 0.0;
        }

        for (int idof2 = thread_yz; idof2 < vars_per_elem2;
             idof2 += nthread_yz) {
            block_mat[local_elem][idof2] = 0.0;
        }

        if (local_thread < elems_per_block) {
            int global_elem_thread = local_thread + blockDim.x * blockIdx.x;
            block_data[local_thread] = physData[global_elem_thread];
        }
    }

    __syncthreads();

    int ideriv = threadIdx.y;
    int iquad = threadIdx.z;

    // TODO : should I remove this memory? going over registers with this?
    T local_res[vars_per_elem];
    memset(local_res, 0.0, sizeof(T) * vars_per_elem);
    T local_mat_col[vars_per_elem];
    memset(local_mat_col, 0.0, sizeof(T) * vars_per_elem);

    // call the device function to get one column of the element stiffness
    // matrix at one quadrature point
    if (active_thread) {
        ElemGroup::template add_element_quadpt_jacobian_col<Data>(
            iquad, ideriv, block_xpts[local_elem], block_vars[local_elem],
            block_data[local_elem], local_res, local_mat_col);
    }
    __syncthreads();

    if (active_thread) {

        for (int idof = 0; idof < vars_per_elem; idof++) {
            // add the local residual into shared memory
            //  computes same local_res blockDim.y or nderivs times (so divide
            //  out)
            atomicAdd(&block_res[local_elem][idof],
                      local_res[idof] / blockDim.y);

            // add this matrix column into shared memory
            atomicAdd(&block_mat[local_elem][vars_per_elem * idof + ideriv],
                      local_mat_col[idof]);
        }
    }
    __syncthreads();

    // atomic add into global res and matrix
    // NOTE : currently adding into dense global matrix (TODO sparse vs)
    if (active_thread) {
        for (int idof = thread_yz; idof < vars_per_elem; idof += nthread_yz) {
            int local_inode = idof / Phys::vars_per_node;
            int local_idof = idof % Phys::vars_per_node;
            const int global_node_ind_row =
                vars_conn[global_elem * Geo::num_nodes + local_inode];
            int global_idof =
                Phys::vars_per_node * global_node_ind_row + local_idof;

            // add shared memory block_res into global memory
            atomicAdd(&residual[global_idof], block_res[local_elem][idof]);

            for (int jdof = 0; jdof < vars_per_elem; jdof++) {
                int local_jnode = jdof / Phys::vars_per_node;
                int local_jdof = jdof % Phys::vars_per_node;
                const int global_node_ind_col =
                    vars_conn[global_elem * Geo::num_nodes + local_jnode];
                int global_jdof =
                    Phys::vars_per_node * global_node_ind_col + local_jdof;

                atomicAdd(&mat[num_vars * global_idof + global_jdof],
                          block_mat[local_elem][vars_per_elem * idof + jdof]);
            } // end of jdof loop
        }     // end of idof loop
    } // end of active thread check if thread's element is past num_elements

} // end of add_jacobian_gpu