#pragma once
#include "cuda_utils.h"
#include "stdlib.h"
#include "vec.h"
#include <algorithm>
#include <vector>

// pre declaration for readability and use in the BsrData class below
__HOST__ void get_row_col_ptrs(const int &nelems, const int &nnodes,
                               const int *conn, const int &nodes_per_elem,
                               int &nnzb, int *&rowPtr, int *&colPtr);

__HOST__ void get_elem_ind_map(const int &nelems, const int &nnodes,
                               const int *conn, const int &nodes_per_elem,
                               const int &nnzb, int *&rowPtr, int *&colPtr,
                               int *&elemIndMap);

class BsrData {
  public:
    BsrData() = default;
    __HOST__ BsrData(const int nelems, const int nnodes,
                     const int &nodes_per_elem, const int &block_dim,
                     const int32_t *conn)
        : nelems(nelems), nnodes(nnodes), nodes_per_elem(nodes_per_elem),
          block_dim(block_dim) {
        get_row_col_ptrs(nelems, nnodes, conn, nodes_per_elem, nnzb, rowPtr,
                         colPtr);
        get_elem_ind_map(nelems, nnodes, conn, nodes_per_elem, nnzb, rowPtr,
                         colPtr, elemIndMap);
    }

    __HOST__ BsrData createDeviceBsrData() { // deep copy each array onto the
        BsrData new_bsr;
        new_bsr.nnzb = this->nnzb;
        new_bsr.nodes_per_elem = this->nodes_per_elem;
        new_bsr.block_dim = this->block_dim;
        new_bsr.nelems = this->nelems;
        new_bsr.nnodes = this->nnodes;

        // make host vec copies of these pointers
        HostVec<int> h_rowPtr(this->nnodes + 1, this->rowPtr);
        HostVec<int> h_colPtr(nnzb, this->colPtr);
        int nodes_per_elem2 = this->nodes_per_elem * this->nodes_per_elem;
        int n_elemIndMap = this->nelems * nodes_per_elem2;
        HostVec<int> h_elemIndMap(n_elemIndMap, this->elemIndMap);

        // now use the Vec utils to create device pointers of them
        auto d_rowPtr = h_rowPtr.createDeviceVec();
        auto d_colPtr = h_colPtr.createDeviceVec();
        auto d_elemIndMap = h_elemIndMap.createDeviceVec();

        // now copy to the new BSR structure
        new_bsr.rowPtr = d_rowPtr.getPtr();
        new_bsr.colPtr = d_colPtr.getPtr();
        new_bsr.elemIndMap = d_elemIndMap.getPtr();

        return new_bsr;
    }

    __HOST_DEVICE__ int32_t getNumValues() const {
        // length of values array
        return nnzb * block_dim * block_dim;
    }

    // private: (keep public for now?)
    int32_t nnzb; // num nonzero blocks (in full matrix)
    int32_t nelems, nnodes;
    int32_t nodes_per_elem; // kelem : nodes_per_elem^2 dense matrix
    int32_t block_dim;      // equiv to vars_per_node (each block is 6x6)
    int *rowPtr, *colPtr, *elemIndMap;
};

// main utils
// ---------------------------------------------------

__HOST_DEVICE__ bool node_in_elem_conn(const int &nodes_per_elem,
                                       const int inode, const int *elem_conn) {
    for (int i = 0; i < nodes_per_elem; i++) {
        if (elem_conn[i] == inode) {
            return true;
        }
    }
    return false;
}

__HOST__ void get_row_col_ptrs(
    const int &nelems, const int &nnodes, const int *conn,
    const int &nodes_per_elem,
    int &nnzb,    // num nonzero blocks
    int *&rowPtr, // array of len nnodes+1 for how many cols in each row
    int *&colPtr  // array of len nnzb of the column indices for each block
) {

    // could launch a kernel to do this somewhat in parallel?

    nnzb = 0;
    std::vector<int> _rowPtr(nnodes + 1, 0);
    std::vector<int> _colPtr;

    // loop over each block row checking nz values
    for (int inode = 0; inode < nnodes; inode++) {
        std::vector<int> temp;
        for (int ielem = 0; ielem < nelems; ielem++) {
            const int *elem_conn = &conn[ielem * nodes_per_elem];
            if (node_in_elem_conn(nodes_per_elem, inode, elem_conn)) {
                for (int in = 0; in < nodes_per_elem; in++) {
                    temp.push_back(elem_conn[in]);
                }
            }
        }

        // first sort the vector
        std::sort(temp.begin(), temp.end());

        // remove duplicates
        auto last = std::unique(temp.begin(), temp.end());
        temp.erase(last, temp.end());

        // add this to _colPtr
        _colPtr.insert(_colPtr.end(), temp.begin(), temp.end());

        // add num non zeros to nnzb for this row, also update
        nnzb += temp.size();
        _rowPtr[inode + 1] = nnzb;
    }

    // copy data to output pointers (deep copy)
    rowPtr = new int[nnodes + 1];
    std::copy(_rowPtr.begin(), _rowPtr.end(), rowPtr);
    colPtr = new int[nnzb];
    std::copy(_colPtr.begin(), _colPtr.end(), colPtr);
}

__HOST__ void get_elem_ind_map(
    const int &nelems, const int &nnodes, const int *conn,
    const int &nodes_per_elem,
    const int &nnzb, // num nonzero blocks
    int *&rowPtr,    // array of len nnodes+1 for how many cols in each row
    int *&colPtr,    // array of len nnzb of the column indices for each block
    int *&elemIndMap) {

    // determine where each global_node node of this elem
    // should be added into the ind of colPtr as map
    int nodes_per_elem2 = nodes_per_elem * nodes_per_elem;
    elemIndMap = new int[nelems * nodes_per_elem2];
    // elemIndMap is nelems x 4 x 4 array for nodes_per_elem = 4
    // shows how to add each block matrix into global
    for (int ielem = 0; ielem < nelems; ielem++) {

        // loop over each block row
        for (int n = 0; n < nodes_per_elem; n++) {
            int global_node_row = conn[nodes_per_elem * ielem + n];
            // get the block col range of colPtr for this block row
            int col_istart = rowPtr[global_node_row];
            int col_iend = rowPtr[global_node_row + 1];

            // loop over each block col
            for (int m = 0; m < nodes_per_elem; m++) {
                int global_node_col = conn[nodes_per_elem * ielem + m];

                // find the matching indices in colPtr for the global_node_col
                // of this elem_conn
                for (int i = col_istart; i < col_iend; i++) {
                    if (colPtr[i] == global_node_col) {
                        // add this component of block kelem matrix into
                        // elem_ind_map
                        int nm = nodes_per_elem * n + m;
                        elemIndMap[nodes_per_elem2 * ielem + nm] = i;
                    }
                }
            }
        }
    }
}

// nice reference utility (may not use this)
// template <typename T, int block_dim, int nodes_per_elem, int dof_per_elem>
// void assemble_sparse_bsr_matrix(
//     const int &nelems, const int &nnodes, const int &vars_per_node,
//     const int *conn,
//     int &nnzb,        // num nonzero blocks
//     int *&rowPtr,     // array of len nnodes+1 for how many cols in each row
//     int *&colPtr,     // array of len nnzb of the column indices for each
//     block int *&elemIndMap, // map of rowPtr, colPtr assembly locations for
//     kelem int &nvalues,     // length of values array = block_dim^2 * nnzb T
//     *&values) {
//     // get nnzb, rowPtr, colPtr
//     get_row_col_ptrs<nodes_per_elem>(nelems, nnodes, conn, nnzb, rowPtr,
//                                      colPtr);

//     // get elemIndMap to know how to add into the values array
//     get_elem_ind_map<T, nodes_per_elem>(nelems, nnodes, conn, nnzb, rowPtr,
//                                         colPtr, elemIndMap);

//     // create values array (T*& because we can't initialize it outside this
//     // method so
//     //     pointer itself changes, normally T* is enough but not here)
//     nvalues = block_dim * block_dim * nnzb;
//     int nnz_per_block = block_dim * block_dim;
//     int blocks_per_elem = nodes_per_elem * nodes_per_elem;
//     values = new T[nnz_per_block * nnzb];

//     // now add each kelem into values array as part of assembly process
//     for (int ielem = 0; ielem < nelems; ielem++) {
//         T kelem[dof_per_elem * dof_per_elem];
//         get_fake_kelem<T, dof_per_elem>(0.0, kelem);

//         // now use elemIndxMap to add into values
//         for (int elem_block = 0; elem_block < blocks_per_elem; elem_block++)
//         {
//             int istart = nnz_per_block *
//                          elemIndMap[blocks_per_elem * ielem + elem_block];
//             T *val = &values[istart];
//             int elem_block_row = elem_block / nodes_per_elem;
//             int elem_block_col = elem_block % nodes_per_elem;
//             for (int inz = 0; inz < nnz_per_block; inz++) {
//                 int inner_row = inz / block_dim;
//                 int row = vars_per_node * elem_block_row + inner_row;
//                 int inner_col = inz % block_dim;
//                 int col = vars_per_node * elem_block_col + inner_col;

//                 val[inz] += kelem[dof_per_elem * row + col];
//             }
//         }
//     }
// }