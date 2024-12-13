#pragma once

#include "../base/elem_group.h"
#include "a2dcore.h"
#include "basis.h"
#include "data.h"
#include "director.h"
#include "physics.h"
#include "shell_utils.h"
#include "a2dshell.h"

template <typename T, class Director_, class Basis_, class Phys_>
class ShellElementGroup
    : public BaseElementGroup<ShellElementGroup<T, Director_, Basis_, Phys_>, T,
                              typename Basis_::Geo, Basis_, Phys_> {
 public:
  using Director = Director_;
  using Basis = Basis_;
  using Geo = typename Basis::Geo;
  using Phys = Phys_;
  using ElemGroup = ShellElementGroup<T, Director_, Basis_, Phys_>;
  using Base = BaseElementGroup<ElemGroup, T, Geo, Basis_, Phys_>;
  using Quadrature = typename Basis::Quadrature;
  using FADType = typename A2D::ADScalar<T, 1>;

  static constexpr int32_t xpts_per_elem = Base::xpts_per_elem;
  static constexpr int32_t dof_per_elem = Base::dof_per_elem;
  static constexpr int32_t num_quad_pts = Base::num_quad_pts;
  static constexpr int32_t num_nodes = Basis::num_nodes;
  static constexpr int32_t vars_per_node = Phys::vars_per_node;

// TODO : way to make this more general if num_quad_pts is not a multiple of 3?
// some if constexpr stuff on type of Basis?
#ifdef USE_GPU
  static constexpr dim3 energy_block(32, num_quad_pts, 1);
  static constexpr dim3 res_block(32, num_quad_pts, 1);
  static constexpr dim3 jac_block(8, dof_per_elem, num_quad_pts);
#endif // USE_GPU

  template <class Data>
  __HOST_DEVICE__ static void add_element_quadpt_energy(
      const int iquad, const T xpts[xpts_per_elem], const T vars[dof_per_elem],
      const Data physData, T& Uelem) {
    // keep in mind max of ~256 floats on single thread

    // data to store in forwards + backwards section
    T fn[3*num_nodes]; // node normals
    T pt[2]; // quadrature point
    T weight = Quadrature::getQuadraturePoint(iquad, pt);

    // in-out of forward & backwards section
    A2D::ADObj<A2D::Mat<T, 3, 3>> u0x, u1x;
    A2D::ADObj<A2D::SymMat<T, 3>> e0ty;
    A2D::ADObj<A2D::Vec<T, 1>> et;
    A2D::ADObj<T> _Uelem;
    
    // forward scope block for strain energy
    // ------------------------------------------------
    {  
      // compute node normals fn
      ShellComputeNodeNormals<T, Basis>(xpts, fn);

      // compute the interpolated drill strain
      ShellComputeDrillStrain<T, vars_per_node, Data, Basis, Director>(
          pt, physData.refAxis, xpts, vars, fn, et.value().get_data());

      // compute directors
      T d[3 * num_nodes];
      Director::template computeDirector<vars_per_node, num_nodes>(vars, fn, d);

      // compute tying strain
      T ety[Basis::num_all_tying_points];
      Phys::template computeTyingStrain<Basis>(xpts, fn, vars, d, ety);

      // compute all shell displacement gradients
      T detXd = ShellComputeDispGrad<T, vars_per_node, Basis, Data>(
            pt, physData.refAxis, xpts, vars, fn, d, ety, 
            u0x.value().get_data(), u1x.value().get_data(), e0ty.value());
      
      // get the scale for disp grad sens of the energy
      T scale = detXd * weight;

      // compute energy + energy-dispGrad sensitivites with physics
      Phys::template computeStrainEnergy<T>(physData, scale, u0x, u1x, e0ty, et, _Uelem);

    } // end of forward scope block for strain energy
    // ------------------------------------------------

    Uelem += _Uelem.value();
  }

  template <class Data>
  __HOST_DEVICE__ static void add_element_quadpt_residual(
      const int iquad, const T xpts[xpts_per_elem], const T vars[dof_per_elem],
      const Data physData, T res[dof_per_elem]) {
    // keep in mind max of ~256 floats on single thread

    // data to store in forwards + backwards section
    T fn[3*num_nodes]; // node normals
    T pt[2]; // quadrature point
    T weight = Quadrature::getQuadraturePoint(iquad, pt);

    // in-out of forward & backwards section
    A2D::ADObj<A2D::Mat<T, 3, 3>> u0x, u1x;
    A2D::ADObj<A2D::SymMat<T, 3>> e0ty;
    A2D::ADObj<A2D::Vec<T, 1>> et;
    
    // forward scope block for strain energy
    // ------------------------------------------------
    {  
      // compute node normals fn
      ShellComputeNodeNormals<T, Basis>(xpts, fn);

      // compute the interpolated drill strain
      ShellComputeDrillStrain<T, vars_per_node, Data, Basis, Director>(
          pt, physData.refAxis, xpts, vars, fn, et.value().get_data());

      // compute directors
      T d[3 * num_nodes];
      Director::template computeDirector<vars_per_node, num_nodes>(vars, fn, d);

      // compute tying strain
      T ety[Basis::num_all_tying_points];
      Phys::template computeTyingStrain<Basis>(xpts, fn, vars, d, ety);

      // compute all shell displacement gradients
      T detXd = ShellComputeDispGrad<T, vars_per_node, Basis, Data>(
            pt, physData.refAxis, xpts, vars, fn, d, ety, 
            u0x.value().get_data(), u1x.value().get_data(), e0ty.value());
      
      // get the scale for disp grad sens of the energy
      T scale = detXd * weight;

      // compute energy + energy-dispGrad sensitivites with physics
      Phys::template computeWeakRes<T>(physData, scale, u0x, u1x, e0ty, et);

    } // end of forward scope block for strain energy
    // ------------------------------------------------

    // beginning of backprop section to final residual derivatives
    // -----------------------------------------------------

    // compute disp grad sens u0x_bar, u1x_bar, e0ty_bar => res, d_bar, ety_bar
    A2D::Vec<T, 3*num_nodes> d_bar;
    // T d_bar[3*num_nodes],
    T ety_bar[Basis::num_all_tying_points];
    ShellComputeDispGradSens<T, vars_per_node, Basis, Data>(
          pt, physData.refAxis, xpts, vars, fn, 
          u0x.bvalue().get_data(), u1x.bvalue().get_data(), e0ty.bvalue(), 
          res, d_bar.get_data(), ety_bar
    );

    // drill strain sens
    ShellComputeDrillStrainSens<T, vars_per_node, Data, Basis, Director>(
          pt, physData.refAxis, xpts, vars, fn, et.bvalue().get_data(), res);

    // if (iquad == 0) {
    //   for (int i = 0; i < 24; i++) {
    //     printf("res[%d] = %.8e\n", i, res[i]);
    //   }
    //   for (int i = 0; i < 12; i++) {
    //     printf("d_bar[%d] = %.8e\n", i, d_bar[i]);
    //   }
    // }

    // backprop tying strain sens ety_bar to d_bar and res
    Phys::template computeTyingStrainSens<Basis>(xpts, fn, ety_bar, d_bar.get_data(),
                                                   res);

    if (iquad == 0) {
      for (int i = 0; i < 24; i++) {
        printf("res[%d] = %.8e\n", i, res[i]);
      }
      for (int i = 0; i < 12; i++) {
        printf("d_bar[%d] = %.8e\n", i, d_bar[i]);
      }
    }

    // directors back to residuals
    Director::template computeDirectorSens<vars_per_node, num_nodes>(d_bar.get_data(), fn,
                                                                     res);

    // TODO : rotation constraint sens for some director classes (zero for linear rotation)

  }  // end of method add_element_quadpt_residual

  template <class Data>
  __HOST_DEVICE__ static void add_element_quadpt_jacobian_col(
      const int iquad, const int ideriv, const T xpts[xpts_per_elem],
      const T vars[dof_per_elem], const Data physData, T res[dof_per_elem],
      T matCol[dof_per_elem]) {
    // TODO
  }  // add_element_quadpt_jacobian_col
};