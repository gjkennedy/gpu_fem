#include "_plate_utils.h"
#include "chrono"
#include "linalg/linalg.h"
#include "shell/shell.h"

/**
 solve on CPU with eigen for debugging
 **/

int main(void) {
    using T = double;

    using Quad = QuadLinearQuadrature<T>;
    using Director = LinearizedRotation<T>;
    using Basis = ShellQuadBasis<T, Quad, 2>;
    using Geo = Basis::Geo;

    constexpr bool has_ref_axis = false;
    using Data = ShellIsotropicData<T, has_ref_axis>;
    using Physics = IsotropicShell<T, Data>;

    using ElemGroup = ShellElementGroup<T, Director, Basis, Physics>;
    using Assembler = ElementAssembler<T, ElemGroup, VecType, BsrMat>;

    int nxe = 10;
    int nye = nxe;
    double Lx = 2.0, Ly = 1.0, E = 70e9, nu = 0.3, thick = 0.005;
    printf("pre create plate assembler\n");
    auto assembler =
        createPlateAssembler<Assembler>(nxe, nye, Lx, Ly, E, nu, thick);
    printf("post create plate assembler\n");

    // init variables u;
    auto vars = assembler.createVarsVec(nullptr, true);
    printf("post create vars vec\n");
    assembler.set_variables(vars);
    printf("post set vars vec\n");

    // setup matrix & vecs
    auto res = assembler.createVarsVec();
    auto soln = assembler.createVarsVec();
    auto kmat = createBsrMat<Assembler, VecType<T>>(assembler);
    printf("post create kmat\n");

    auto start = std::chrono::high_resolution_clock::now();
    assembler.add_jacobian(res, kmat);
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    printf("post assemble res, kmat\n");

    assembler.apply_bcs(res);
    assembler.apply_bcs(kmat);
    printf("post apply bcs\n");

    // check kmat here
    printf("kmat: ");
    printVec<double>(24, kmat.getPtr());

    // set the rhs for this problem
    double Q = 1.0; // load magnitude
    // T *my_loads = getPlateLoads<T, Physics>(nxe, nye, Lx, Ly, Q);
    printf("pre loads\n");
    T *my_loads = getPlatePointLoad<T, Physics>(nxe, nye, Lx, Ly, Q);
    // printf("my_loads: ");
    // printVec<T>(24, my_loads);
    auto loads = assembler.createVarsVec(my_loads);
    assembler.apply_bcs(loads);

    // also printout stiffness matrix
    auto kmat_vec = kmat.getVec();
    write_to_csv<double>(kmat_vec.getPtr(), kmat_vec.getSize(),
                         "csv/plate_kmat.csv");

    // now do cusparse solve on linear static analysis
    printf("pre linear solve\n");
    auto start2 = std::chrono::high_resolution_clock::now();
    EIGEN::iterative_CG_solve<T>(kmat, loads, soln, false);
    auto stop2 = std::chrono::high_resolution_clock::now();
    auto duration2 =
        std::chrono::duration_cast<std::chrono::microseconds>(stop2 - start2);
    printf("post linear solve\n");

    // compute total direc derivative of analytic residual

    // print some of the data of host residual
    auto h_soln = soln.createHostVec();
    auto h_loads = loads.createHostVec();

    // printf("took %d microseconds to run add jacobian\n",
    // (int)duration.count()); printf("took %d microseconds to run cusparse
    // solve\n",
    //        (int)duration2.count());

    // write the solution to binary file so I can read it in in python
    auto bsr_data = kmat.getBsrData();
    write_to_csv<double>(h_loads.getPtr(), h_loads.getSize(),
                         "csv/plate_loads.csv");
    write_to_csv<double>(h_soln.getPtr(), h_soln.getSize(),
                         "csv/plate_soln.csv");

    // debug kmat
    write_to_csv<int>(bsr_data.rowPtr, bsr_data.nnodes + 1,
                      "csv/plate_rowPtr.csv");
    write_to_csv<int>(bsr_data.colPtr, bsr_data.nnzb, "csv/plate_colPtr.csv");

    delete[] my_loads;

    return 0;
};