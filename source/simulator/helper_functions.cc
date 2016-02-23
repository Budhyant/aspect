/*
  Copyright (C) 2011 - 2015 by the authors of the ASPECT code.

  This file is part of ASPECT.

  ASPECT is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  ASPECT is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ASPECT; see the file doc/COPYING.  If not see
  <http://www.gnu.org/licenses/>.
*/


#include <aspect/simulator.h>
#include <aspect/melt.h>
#include <aspect/global.h>


#include <deal.II/base/index_set.h>
#include <deal.II/base/conditional_ostream.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/lac/block_sparsity_pattern.h>
#include <deal.II/grid/grid_tools.h>

#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_dgq.h>
#include <deal.II/fe/fe_dgp.h>
#include <deal.II/fe/fe_values.h>

#include <deal.II/numerics/error_estimator.h>
#include <deal.II/numerics/vector_tools.h>

#include <deal.II/distributed/solution_transfer.h>
#include <deal.II/distributed/grid_refinement.h>

#include <fstream>
#include <iostream>
#include <iomanip>
#include <locale>
#include <string>


namespace aspect
{

  template <int dim>
  Simulator<dim>::AdvectionField::
  AdvectionField (const FieldType field_type,
                  const unsigned int compositional_variable)
    :
    field_type (field_type),
    compositional_variable (compositional_variable)
  {
    if (field_type == temperature_field)
      Assert (compositional_variable == numbers::invalid_unsigned_int,
              ExcMessage ("You can't specify a compositional variable if you "
                          "have in fact selected the temperature."));
  }



  template <int dim>
  typename Simulator<dim>::AdvectionField
  Simulator<dim>::AdvectionField::temperature ()
  {
    return AdvectionField(temperature_field);
  }



  template <int dim>
  typename Simulator<dim>::AdvectionField
  Simulator<dim>::AdvectionField::composition (const unsigned int compositional_variable)
  {
    return AdvectionField(compositional_field,
                          compositional_variable);
  }


  template <int dim>
  bool
  Simulator<dim>::AdvectionField::is_temperature() const
  {
    return (field_type == temperature_field);
  }


  template <int dim>
  bool
  Simulator<dim>::AdvectionField::is_porosity(const Introspection<dim> &introspection) const
  {
    if (field_type != compositional_field)
      return false;
    else
      return (introspection.name_for_compositional_index(compositional_variable) == "porosity");
  }


  template <int dim>
  unsigned int
  Simulator<dim>::AdvectionField::block_index(const Introspection<dim> &introspection) const
  {
    if (this->is_temperature())
      return introspection.block_indices.temperature;
    else
      return introspection.block_indices.compositional_fields[compositional_variable];
  }

  template <int dim>
  unsigned int
  Simulator<dim>::AdvectionField::component_index(const Introspection<dim> &introspection) const
  {
    if (this->is_temperature())
      return introspection.component_indices.temperature;
    else
      return introspection.component_indices.compositional_fields[compositional_variable];
  }

  template <int dim>
  unsigned int
  Simulator<dim>::AdvectionField::base_element(const Introspection<dim> &introspection) const
  {
    if (this->is_temperature())
      return introspection.base_elements.temperature;
    else
      return introspection.base_elements.compositional_fields;
  }


  template <int dim>
  void Simulator<dim>::output_program_stats()
  {
    if (!aspect::output_parallel_statistics)
      return;

    Utilities::System::MemoryStats stats;
    Utilities::System::get_memory_stats(stats);
    pcout << "VmPeak (proc0): " << stats.VmPeak/1024 << " mb" << std::endl;

    // memory consumption:
    const double mb = 1024*1024; //convert from bytes into mb
    pcout << "memory in MB:" << std::endl
          << "* tria " << triangulation.memory_consumption()/mb << std::endl
          << "  - p4est " << triangulation.memory_consumption_p4est()/mb << std::endl
          << "* DoFHandler " << dof_handler.memory_consumption()/mb <<std::endl
          << "* ConstraintMatrix " << constraints.memory_consumption()/mb << std::endl
          << "* current_constraints " << current_constraints.memory_consumption()/mb << std::endl
          << "* Matrix " << system_matrix.memory_consumption()/mb << std::endl
          << "* 5 Vectors " << 5*solution.memory_consumption()/mb << std::endl
          << "* preconditioner " << (system_preconditioner_matrix.memory_consumption()
//                                     + Amg_preconditioner->memory_consumption()
                                     /*+Mp_preconditioner->memory_consumption()
                                                                      +T_preconditioner->memory_consumption()*/)/mb
          << std::endl
          << "  - matrix " << system_preconditioner_matrix.memory_consumption()/mb << std::endl
//          << "  - prec vel " << Amg_preconditioner->memory_consumption()/mb << std::endl
          << "  - prec mass " << 0/*Mp_preconditioner->memory_consumption()/mb*/ << std::endl
          << "  - prec T " << 0/*T_preconditioner->memory_consumption()/mb*/ << std::endl
          << std::endl;
  }



  namespace
  {
    /**
     * A function that writes the statistics object into a file.
     *
     * @param stat_file_name The name of the file into which the result
     * should go
     * @param copy_of_table A copy of the table that we're to write. Since
     * this function is called in the background on a separate thread,
     * the actual table might be modified while we are about to write
     * it, so we need to work on a copy. This copy is deleted at the end
     * of this function.
     */
    void do_output_statistics (const std::string stat_file_name,
                               const TableHandler *copy_of_table)
    {
      // write into a temporary file for now so that we don't
      // interrupt anyone who might want to look at the real
      // statistics file while the program is still running
      const std::string tmp_file_name = stat_file_name + " tmp";

      std::ofstream stat_file (tmp_file_name.c_str());
      copy_of_table->write_text (stat_file,
                                 TableHandler::table_with_separate_column_description);
      stat_file.close();

      // now move the temporary file into place
      std::rename(tmp_file_name.c_str(), stat_file_name.c_str());

      // delete the copy now:
      delete copy_of_table;
    }
  }


  template <int dim>
  void Simulator<dim>::output_statistics()
  {
    // only write the statistics file from processor zero
    if (Utilities::MPI::this_mpi_process(mpi_communicator)!=0)
      return;

    if (parameters.convert_to_years == true)
      {
        statistics.set_scientific("Time (years)", true);
        statistics.set_scientific("Time step size (years)", true);
      }
    else
      {
        statistics.set_scientific("Time (seconds)", true);
        statistics.set_scientific("Time step size (seconds)", true);
      }

    // formatting the table we're about to output and writing the
    // actual file may take some time, so do it on a separate
    // thread. we pass a pointer to a copy of the statistics
    // object which the called function then has to destroy
    //
    // before we can start working on a new thread, we need to
    // make sure that the previous thread is done or they'll
    // stomp on each other's feet
    output_statistics_thread.join();
    output_statistics_thread = Threads::new_thread (&do_output_statistics,
                                                    parameters.output_directory+"statistics",
                                                    new TableHandler(statistics));
  }



  /**
   * Find the largest velocity throughout the domain.
   **/
  template <int dim>
  double Simulator<dim>::get_maximal_velocity (
    const LinearAlgebra::BlockVector &solution) const
  {
    // use a quadrature formula that has one point at
    // the location of each degree of freedom in the
    // velocity element
    const QIterated<dim> quadrature_formula (QTrapez<1>(),
                                             parameters.stokes_velocity_degree);
    const unsigned int n_q_points = quadrature_formula.size();


    FEValues<dim> fe_values (mapping, finite_element, quadrature_formula, update_values);
    std::vector<Tensor<1,dim> > velocity_values(n_q_points);

    double max_local_velocity = 0;

    // loop over all locally owned cells and evaluate the velocities at each
    // quadrature point (i.e. each node). keep a running tally of the largest
    // such velocity
    typename DoFHandler<dim>::active_cell_iterator
    cell = dof_handler.begin_active(),
    endc = dof_handler.end();
    for (; cell!=endc; ++cell)
      if (cell->is_locally_owned())
        {
          fe_values.reinit (cell);
          fe_values[introspection.extractors.velocities].get_function_values (solution,
                                                                              velocity_values);

          for (unsigned int q=0; q<n_q_points; ++q)
            max_local_velocity = std::max (max_local_velocity,
                                           velocity_values[q].norm());
        }

    // return the largest value over all processors
    return Utilities::MPI::max (max_local_velocity, mpi_communicator);
  }



  template <int dim>
  std::pair<double,bool> Simulator<dim>::compute_time_step () const
  {
    const QIterated<dim> quadrature_formula (QTrapez<1>(),
                                             parameters.stokes_velocity_degree);
    const unsigned int n_q_points = quadrature_formula.size();

    FEValues<dim> fe_values (mapping, finite_element, quadrature_formula,
                             update_values | update_gradients |
                             ((parameters.use_conduction_timestep || parameters.include_melt_transport)
                              ?
                              update_quadrature_points
                              :
                              update_default));
    std::vector<Tensor<1,dim> > velocity_values(n_q_points), fluid_velocity_values(n_q_points);
    std::vector<Tensor<1,dim> > pressure_gradients(n_q_points);
    std::vector<double> pressure_values(n_q_points), temperature_values(n_q_points);
    std::vector<std::vector<double> > composition_values (parameters.n_compositional_fields,std::vector<double> (n_q_points));
    std::vector<double> composition_values_at_q_point (parameters.n_compositional_fields);

    double new_time_step;
    bool convection_dominant;

    double max_local_speed_over_meshsize = 0;
    double min_local_conduction_timestep = std::numeric_limits<double>::max(), min_conduction_timestep;

    typename DoFHandler<dim>::active_cell_iterator
    cell = dof_handler.begin_active(),
    endc = dof_handler.end();
    for (; cell!=endc; ++cell)
      if (cell->is_locally_owned())
        {
          fe_values.reinit (cell);
          fe_values[introspection.extractors.velocities].get_function_values (solution,
                                                                              velocity_values);

          double max_local_velocity = 0;
          for (unsigned int q=0; q<n_q_points; ++q)
            max_local_velocity = std::max (max_local_velocity,
                                           velocity_values[q].norm());

          if (parameters.include_melt_transport)
            {
              FEValuesExtractors::Vector ex_u_f = introspection.variable("fluid velocity").extractor_vector();
              fe_values[ex_u_f].get_function_values (solution,fluid_velocity_values);

              for (unsigned int q=0; q<n_q_points; ++q)
                max_local_velocity = std::max (max_local_velocity,
                                               fluid_velocity_values[q].norm());
            }

          max_local_speed_over_meshsize = std::max(max_local_speed_over_meshsize,
                                                   max_local_velocity
                                                   /
                                                   cell->minimum_vertex_distance());

          if (parameters.use_conduction_timestep)
            {
              fe_values[introspection.extractors.pressure].get_function_values (solution,
                                                                                pressure_values);
              fe_values[introspection.extractors.temperature].get_function_values (solution,
                                                                                   temperature_values);
              fe_values[introspection.extractors.pressure].get_function_gradients (solution,
                                                                                   pressure_gradients);
              for (unsigned int c=0; c<parameters.n_compositional_fields; ++c)
                fe_values[introspection.extractors.compositional_fields[c]].get_function_values (solution,
                    composition_values[c]);

              MaterialModel::MaterialModelInputs<dim> in(n_q_points, parameters.n_compositional_fields);
              MaterialModel::MaterialModelOutputs<dim> out(n_q_points, parameters.n_compositional_fields);

              in.strain_rate.resize(0);// we are not reading the viscosity

              for (unsigned int q=0; q<n_q_points; ++q)
                {
                  for (unsigned int k=0; k < composition_values_at_q_point.size(); ++k)
                    composition_values_at_q_point[k] = composition_values[k][q];

                  in.position[q] = fe_values.quadrature_point(q);
                  in.temperature[q] = temperature_values[q];
                  in.pressure[q] = pressure_values[q];
                  in.velocity[q] = velocity_values[q];
                  in.pressure_gradient[q] = pressure_gradients[q];
                  for (unsigned int c=0; c<parameters.n_compositional_fields; ++c)
                    in.composition[q][c] = composition_values_at_q_point[c];
                }
              in.cell = &cell;

              material_model->evaluate(in, out);

              // Evaluate thermal diffusivity at each quadrature point and
              // calculate the corresponding conduction timestep, if applicable
              for (unsigned int q=0; q<n_q_points; ++q)
                {
                  const double k = out.thermal_conductivities[q];
                  const double rho = out.densities[q];
                  const double c_p = out.specific_heat[q];

                  const double thermal_diffusivity = k/(rho*c_p);

                  if (thermal_diffusivity > 0)
                    {
                      min_local_conduction_timestep = std::min(min_local_conduction_timestep,
                                                               parameters.CFL_number*pow(cell->minimum_vertex_distance(),2)
                                                               / thermal_diffusivity);
                    }
                }
            }

        }

    const double max_global_speed_over_meshsize
      = Utilities::MPI::max (max_local_speed_over_meshsize, mpi_communicator);
    if (parameters.use_conduction_timestep)
      MPI_Allreduce (&min_local_conduction_timestep, &min_conduction_timestep, 1, MPI_DOUBLE, MPI_MIN, mpi_communicator);
    else
      min_conduction_timestep = std::numeric_limits<double>::max();

    if ((max_global_speed_over_meshsize != 0.0) ||
        (min_conduction_timestep < std::numeric_limits<double>::max()))
      {
        new_time_step = std::min(min_conduction_timestep,
                                 (parameters.CFL_number / (parameters.temperature_degree * max_global_speed_over_meshsize)));
        convection_dominant = (new_time_step < min_conduction_timestep);
      }
    else
      {
        // If the velocity is zero and we either do not compute the conduction
        // timestep or do not have any conduction, then it is somewhat
        // arbitrary what time step we should choose. In that case, do as if
        // the velocity was one
        new_time_step = (parameters.CFL_number /
                         (parameters.temperature_degree * 1));
        convection_dominant = false;
      }


    return std::make_pair(new_time_step, convection_dominant);
  }



  template <int dim>
  std::pair<double,double>
  Simulator<dim>::
  get_extrapolated_advection_field_range (const AdvectionField &advection_field) const
  {
    const QIterated<dim> quadrature_formula (QTrapez<1>(),
                                             (advection_field.is_temperature() ?
                                              parameters.temperature_degree :
                                              parameters.composition_degree));

    const unsigned int n_q_points = quadrature_formula.size();

    const FEValuesExtractors::Scalar field
      = (advection_field.is_temperature()
         ?
         introspection.extractors.temperature
         :
         introspection.extractors.compositional_fields[advection_field.compositional_variable]
        );

    FEValues<dim> fe_values (mapping, finite_element, quadrature_formula,
                             update_values);
    std::vector<double> old_field_values(n_q_points);
    std::vector<double> old_old_field_values(n_q_points);

    // This presets the minimum with a bigger
    // and the maximum with a smaller number
    // than one that is going to appear. Will
    // be overwritten in the cell loop or in
    // the communication step at the
    // latest.
    double min_local_field = std::numeric_limits<double>::max(),
           max_local_field = -std::numeric_limits<double>::max();

    if (timestep_number > 1)
      {
        typename DoFHandler<dim>::active_cell_iterator
        cell = dof_handler.begin_active(),
        endc = dof_handler.end();
        for (; cell!=endc; ++cell)
          if (cell->is_locally_owned())
            {
              fe_values.reinit (cell);
              fe_values[field].get_function_values (old_solution,
                                                    old_field_values);
              fe_values[field].get_function_values (old_old_solution,
                                                    old_old_field_values);

              for (unsigned int q=0; q<n_q_points; ++q)
                {
                  const double extrapolated_field =
                    (1. + time_step/old_time_step) * old_field_values[q]-
                    time_step/old_time_step * old_old_field_values[q];

                  min_local_field = std::min (min_local_field,
                                              extrapolated_field);
                  max_local_field = std::max (max_local_field,
                                              extrapolated_field);
                }
            }
      }
    else
      {
        typename DoFHandler<dim>::active_cell_iterator
        cell = dof_handler.begin_active(),
        endc = dof_handler.end();
        for (; cell!=endc; ++cell)
          if (cell->is_locally_owned())
            {
              fe_values.reinit (cell);
              fe_values[field].get_function_values (old_solution,
                                                    old_field_values);

              for (unsigned int q=0; q<n_q_points; ++q)
                {
                  const double extrapolated_field = old_field_values[q];

                  min_local_field = std::min (min_local_field,
                                              extrapolated_field);
                  max_local_field = std::max (max_local_field,
                                              extrapolated_field);
                }
            }
      }

    return std::make_pair(Utilities::MPI::min (min_local_field,
                                               mpi_communicator),
                          Utilities::MPI::max (max_local_field,
                                               mpi_communicator));
  }


  template <int dim>
  void Simulator<dim>::interpolate_onto_velocity_system(const TensorFunction<1,dim> &func,
                                                        LinearAlgebra::Vector &vec)
  {
    ConstraintMatrix hanging_constraints(introspection.index_sets.system_relevant_set);
    DoFTools::make_hanging_node_constraints(dof_handler, hanging_constraints);
    hanging_constraints.close();

    Assert(introspection.block_indices.velocities == 0, ExcNotImplemented());
    const std::vector<Point<dim> > mesh_support_points = finite_element.base_element(introspection.base_elements.velocities).get_unit_support_points();
    FEValues<dim> mesh_points (mapping, finite_element, mesh_support_points, update_quadrature_points);
    std::vector<types::global_dof_index> cell_dof_indices (finite_element.dofs_per_cell);

    typename DoFHandler<dim>::active_cell_iterator cell = dof_handler.begin_active(),
                                                   endc = dof_handler.end();
    for (; cell != endc; ++cell)
      if (cell->is_locally_owned())
        {
          mesh_points.reinit(cell);
          cell->get_dof_indices (cell_dof_indices);
          for (unsigned int j=0; j<finite_element.base_element(introspection.base_elements.velocities).dofs_per_cell; ++j)
            for (unsigned int dir=0; dir<dim; ++dir)
              {
                unsigned int support_point_index
                  = finite_element.component_to_system_index(/*velocity component=*/ introspection.component_indices.velocities[dir],
                                                                                     /*dof index within component=*/ j);
                Assert(introspection.block_indices.velocities == 0, ExcNotImplemented());
                vec[cell_dof_indices[support_point_index]] = func.value(mesh_points.quadrature_point(j))[dir];
              }
        }

    vec.compress(VectorOperation::insert);
    hanging_constraints.distribute(vec);
  }

  /*
   * normalize the pressure by calculating the surface integral of the pressure on the outer
   * shell and subtracting this from all pressure nodes.
   */
  template <int dim>
  void Simulator<dim>::normalize_pressure(LinearAlgebra::BlockVector &vector)
  {
    if (parameters.pressure_normalization == "no")
      return;

    const FEValuesExtractors::Scalar &extractor_pressure =
      (parameters.include_melt_transport ?
       introspection.variable("fluid pressure").extractor_scalar()
       : introspection.extractors.pressure);

    double my_pressure = 0.0;
    double my_area = 0.0;
    if (parameters.pressure_normalization == "surface")
      {
        QGauss < dim - 1 > quadrature (parameters.stokes_velocity_degree + 1);

        const unsigned int n_q_points = quadrature.size();
        FEFaceValues<dim> fe_face_values (mapping, finite_element,  quadrature,
                                          update_JxW_values | update_values);

        std::vector<double> pressure_values(n_q_points);

        typename DoFHandler<dim>::active_cell_iterator
        cell = dof_handler.begin_active(),
        endc = dof_handler.end();
        for (; cell != endc; ++cell)
          if (cell->is_locally_owned())
            {
              for (unsigned int face_no = 0; face_no < GeometryInfo<dim>::faces_per_cell; ++face_no)
                {
                  const typename DoFHandler<dim>::face_iterator face = cell->face (face_no);
                  if (face->at_boundary()
                      &&
                      (geometry_model->depth (face->center()) <
                       (face->diameter() / std::sqrt(1.*dim-1) / 3)))
                    {
                      fe_face_values.reinit (cell, face_no);
                      fe_face_values[extractor_pressure].get_function_values(vector,
                                                                             pressure_values);

                      for (unsigned int q = 0; q < n_q_points; ++q)
                        {
                          my_pressure += pressure_values[q]
                                         * fe_face_values.JxW (q);
                          my_area += fe_face_values.JxW (q);
                        }
                    }
                }
            }
      }
    else if (parameters.pressure_normalization=="volume")
      {
        const QGauss<dim> quadrature (parameters.stokes_velocity_degree + 1);

        const unsigned int n_q_points = quadrature.size();
        FEValues<dim> fe_values (mapping, finite_element,  quadrature,
                                 update_JxW_values | update_values);

        std::vector<double> pressure_values(n_q_points);

        typename DoFHandler<dim>::active_cell_iterator
        cell = dof_handler.begin_active(),
        endc = dof_handler.end();
        for (; cell != endc; ++cell)
          if (cell->is_locally_owned())
            {
              fe_values.reinit (cell);
              fe_values[extractor_pressure].get_function_values(vector,
                                                                pressure_values);

              for (unsigned int q = 0; q < n_q_points; ++q)
                {
                  my_pressure += pressure_values[q]
                                 * fe_values.JxW (q);
                  my_area += fe_values.JxW (q);
                }
            }
      }
    else
      AssertThrow (false, ExcMessage("Invalid pressure normalization method: " +
                                     parameters.pressure_normalization));

    pressure_adjustment = 0.0;
    // sum up the integrals from each processor
    {
      const double my_temp[2] = {my_pressure, my_area};
      double temp[2];
      Utilities::MPI::sum (my_temp, mpi_communicator, temp);
      if (parameters.pressure_normalization == "surface")
        pressure_adjustment = -temp[0]/temp[1] + parameters.surface_pressure;
      else if (parameters.pressure_normalization == "volume")
        pressure_adjustment = -temp[0]/temp[1];
      else
        AssertThrow(false, ExcNotImplemented());
    }

    // A complication is that we can't modify individual
    // elements of the solution vector since that one has ghost element.
    // rather, we first need to localize it and then distribute back
    LinearAlgebra::BlockVector distributed_vector (introspection.index_sets.system_partitioning,
                                                   mpi_communicator);
    distributed_vector = vector;

    if (parameters.use_locally_conservative_discretization == false)
      {
        if (introspection.block_indices.velocities != introspection.block_indices.pressure
            && !parameters.include_melt_transport)
          distributed_vector.block(introspection.block_indices.pressure).add(pressure_adjustment);
        else
          {
            // pressure is not in a separate block, so we have to modify the values manually
            const unsigned int pressure_component = (parameters.include_melt_transport ?
                                                     introspection.variable("fluid pressure").first_component_index
                                                     : introspection.component_indices.pressure);
            const unsigned int n_local_pressure_dofs = (parameters.include_melt_transport ?
                                                        finite_element.base_element(introspection.variable("fluid pressure").base_index).dofs_per_cell
                                                        : finite_element.base_element(introspection.base_elements.pressure).dofs_per_cell);
            std::vector<types::global_dof_index> local_dof_indices (finite_element.dofs_per_cell);
            typename DoFHandler<dim>::active_cell_iterator
            cell = dof_handler.begin_active(),
            endc = dof_handler.end();
            for (; cell != endc; ++cell)
              if (cell->is_locally_owned())
                {
                  cell->get_dof_indices (local_dof_indices);
                  for (unsigned int j=0; j<n_local_pressure_dofs; ++j)
                    {
                      unsigned int support_point_index
                        = finite_element.component_to_system_index(pressure_component,
                                                                   /*dof index within component=*/ j);

                      // then adjust its value. Note that because we end up touching
                      // entries more than once, we are not simply incrementing
                      // distributed_vector but copy from the unchanged vector.
                      distributed_vector(local_dof_indices[support_point_index]) = vector(local_dof_indices[support_point_index]) + pressure_adjustment;
                    }
                }
            distributed_vector.compress(VectorOperation::insert);
          }
      }
    else
      {
        // this case is a bit more complicated: if the condition above is false
        // then we use the FE_DGP element for which the shape functions do not
        // add up to one; consequently, adding a constant to all degrees of
        // freedom does not alter the overall function by that constant, but
        // by something different
        //
        // we can work around this by using the documented property of the
        // FE_DGP element that the first shape function is constant.
        // consequently, adding the adjustment to the global function is
        // achieved by adding the adjustment to the first pressure degree
        // of freedom on each cell.
        Assert (dynamic_cast<const FE_DGP<dim>*>(&finite_element.base_element(introspection.base_elements.pressure)) != 0,
                ExcInternalError());
        const unsigned int pressure_component = (parameters.include_melt_transport ?
                                                 introspection.variable("fluid pressure").first_component_index
                                                 : introspection.component_indices.pressure);
        std::vector<types::global_dof_index> local_dof_indices (finite_element.dofs_per_cell);
        typename DoFHandler<dim>::active_cell_iterator
        cell = dof_handler.begin_active(),
        endc = dof_handler.end();
        for (; cell != endc; ++cell)
          if (cell->is_locally_owned())
            {
              // identify the first pressure dof
              cell->get_dof_indices (local_dof_indices);
              const unsigned int first_pressure_dof
                = finite_element.component_to_system_index (pressure_component, 0);

              // make sure that this DoF is really owned by the current processor
              // and that it is in fact a pressure dof
              Assert (dof_handler.locally_owned_dofs().is_element(local_dof_indices[first_pressure_dof]),
                      ExcInternalError());

              // then adjust its value
              distributed_vector(local_dof_indices[first_pressure_dof]) += pressure_adjustment;
            }
        distributed_vector.compress(VectorOperation::insert);
      }

    // now get back to the original vector
    vector = distributed_vector;
  }


  /*
   * inverse to normalize_pressure.
   */
  template <int dim>
  void Simulator<dim>::denormalize_pressure (LinearAlgebra::BlockVector &vector, const LinearAlgebra::BlockVector &relevant_vector)
  {
    if (parameters.pressure_normalization == "no")
      return;

    if (parameters.use_locally_conservative_discretization == false)
      {
        if ((introspection.block_indices.velocities != introspection.block_indices.pressure)
            && !parameters.include_melt_transport)
          vector.block(introspection.block_indices.pressure).add(-1.0 * pressure_adjustment);
        else
          {
            // pressure is not in a separate block so we have to modify the values manually
            const unsigned int pressure_component = (parameters.include_melt_transport ?
                                                     introspection.variable("fluid pressure").first_component_index
                                                     : introspection.component_indices.pressure);
            const unsigned int n_local_pressure_dofs = (parameters.include_melt_transport ?
                                                        finite_element.base_element(introspection.variable("fluid pressure").base_index).dofs_per_cell
                                                        : finite_element.base_element(introspection.base_elements.pressure).dofs_per_cell);

            std::vector<types::global_dof_index> local_dof_indices (finite_element.dofs_per_cell);
            typename DoFHandler<dim>::active_cell_iterator
            cell = dof_handler.begin_active(),
            endc = dof_handler.end();
            for (; cell != endc; ++cell)
              if (cell->is_locally_owned())
                {
                  cell->get_dof_indices (local_dof_indices);
                  for (unsigned int j=0; j<n_local_pressure_dofs; ++j)
                    {
                      const unsigned int local_dof_index
                        = finite_element.component_to_system_index(pressure_component,
                                                                   /*dof index within component=*/ j);

                      // then adjust its value. Note that because we end up touching
                      // entries more than once, we are not simply incrementing
                      // distributed_vector but copy from the unchanged vector.
                      vector(local_dof_indices[local_dof_index])
                        = relevant_vector(local_dof_indices[local_dof_index]) - pressure_adjustment;
                    }
                }
            vector.compress(VectorOperation::insert);
          }
      }
    else
      {
        // this case is a bit more complicated: if the condition above is false
        // then we use the FE_DGP element for which the shape functions do not
        // add up to one; consequently, adding a constant to all degrees of
        // freedom does not alter the overall function by that constant, but
        // by something different
        //
        // we can work around this by using the documented property of the
        // FE_DGP element that the first shape function is constant.
        // consequently, adding the adjustment to the global function is
        // achieved by adding the adjustment to the first pressure degree
        // of freedom on each cell.
        Assert (dynamic_cast<const FE_DGP<dim>*>(&finite_element.base_element(introspection.base_elements.pressure)) != 0,
                ExcInternalError());
        const unsigned int pressure_component = (parameters.include_melt_transport ?
                                                 introspection.variable("fluid pressure").first_component_index
                                                 : introspection.component_indices.pressure);
        Assert(!parameters.include_melt_transport, ExcNotImplemented());
        std::vector<types::global_dof_index> local_dof_indices (finite_element.dofs_per_cell);
        typename DoFHandler<dim>::active_cell_iterator
        cell = dof_handler.begin_active(),
        endc = dof_handler.end();
        for (; cell != endc; ++cell)
          if (cell->is_locally_owned())
            {
              // identify the first pressure dof
              cell->get_dof_indices (local_dof_indices);
              const unsigned int first_pressure_dof
                = finite_element.component_to_system_index (pressure_component, 0);

              // make sure that this DoF is really owned by the current processor
              // and that it is in fact a pressure dof
              Assert (dof_handler.locally_owned_dofs().is_element(local_dof_indices[first_pressure_dof]),
                      ExcInternalError());
              Assert (local_dof_indices[first_pressure_dof] >= vector.block(0).size(),
                      ExcInternalError());

              // then adjust its value
              vector (local_dof_indices[first_pressure_dof]) -= pressure_adjustment;
            }

        vector.compress(VectorOperation::add);
      }
  }



  /**
   * This routine adjusts the second block of the right hand side of the
   * system containing the compressibility, so that the system becomes
   * compatible. See the general documentation of this class for more
   * information.
   */
  template <int dim>
  void Simulator<dim>::make_pressure_rhs_compatible(LinearAlgebra::BlockVector &vector)
  {
    if (parameters.use_locally_conservative_discretization)
      AssertThrow(false, ExcNotImplemented());

    const double global_normal_velocity_integral = 0.0;

    // In the following we integrate the right hand side. This integral is the
    // correction term that needs to be added to the pressure right hand side.
    // (so that the integral of right hand side is set to zero).
    if (!parameters.include_melt_transport && introspection.block_indices.velocities != introspection.block_indices.pressure)
      {
        const double mean       = vector.block(introspection.block_indices.pressure).mean_value();
        const double correction = (global_normal_velocity_integral - mean * vector.block(introspection.block_indices.pressure).size()) / global_volume;
        vector.block(introspection.block_indices.pressure).add(correction, pressure_shape_function_integrals.block(introspection.block_indices.pressure));
      }
    else
      {
        // we need to operate only on p_f not on p_c
        const IndexSet &idxset = parameters.include_melt_transport ?
                                 introspection.index_sets.locally_owned_fluid_pressure_dofs
                                 :
                                 introspection.index_sets.locally_owned_pressure_dofs;
        double pressure_sum = 0.0;

        for (unsigned int i=0; i < idxset.n_elements(); ++i)
          {
            types::global_dof_index idx = idxset.nth_index_in_set(i);
            pressure_sum += vector(idx);
          }

        const double global_pressure_sum = Utilities::MPI::sum(pressure_sum, mpi_communicator);
        const double correction = (global_normal_velocity_integral - global_pressure_sum) / global_volume;

        for (unsigned int i=0; i < idxset.n_elements(); ++i)
          {
            types::global_dof_index idx = idxset.nth_index_in_set(i);
            vector(idx) += correction * pressure_shape_function_integrals(idx);
          }

        vector.compress(VectorOperation::add);
      }
  }


  template <int dim>
  void Simulator<dim>::compute_melt_variables(LinearAlgebra::BlockVector &solution)
  {
    if (!parameters.include_melt_transport)
      return;

    LinearAlgebra::BlockVector distributed_vector (introspection.index_sets.system_partitioning,
                                                   mpi_communicator);

    const unsigned int por_idx = introspection.compositional_index_for_name("porosity");


    // compute fluid_velocity
    // u_f =  u_s - K_D (nabla p_f - rho_f g) / phi  or = 0
    if (true)
      {
        // solve mass matrix problem

        // TODO: lots of cleanup/optimization opportunities here:
        // - store matrix?
        // - reuse system matrix/sparsity pattern?
        // - only construct matrix for specific u_f block?
        // - clean up solver/preconditioner


        LinearAlgebra::BlockSparseMatrix matrix;
        LinearAlgebra::BlockCompressedSparsityPattern sp;
#ifdef ASPECT_USE_PETSC
        sp.reinit (introspection.index_sets.system_relevant_partitioning);
#else
        sp.reinit (introspection.index_sets.system_partitioning,
                   introspection.index_sets.system_partitioning,
                   introspection.index_sets.system_relevant_partitioning,
                   mpi_communicator);
#endif

        const typename Introspection<dim>::ComponentIndices &x
          = introspection.component_indices;

        Table<2,DoFTools::Coupling> coupling (introspection.n_components,
                                              introspection.n_components);
        const unsigned int first_fluid_c_i = introspection.variable("fluid velocity").first_component_index;
        for (unsigned int c=0; c<dim; ++c)
          for (unsigned int d=0; d<dim; ++d)
            coupling[first_fluid_c_i+c][first_fluid_c_i+d] = DoFTools::always;

        DoFTools::make_sparsity_pattern (dof_handler,
                                         coupling, sp,
                                         current_constraints, false,
                                         Utilities::MPI::
                                         this_mpi_process(mpi_communicator));

#ifdef ASPECT_USE_PETSC
        SparsityTools::distribute_sparsity_pattern(sp,
                                                   dof_handler.locally_owned_dofs_per_processor(),
                                                   mpi_communicator, introspection.index_sets.system_relevant_set);

        sp.compress();
        matrix.reinit (system_partitioning, system_partitioning, sp, mpi_communicator);
#else
        sp.compress();
        matrix.reinit (sp);
#endif

        LinearAlgebra::BlockVector rhs, distributed_solution;
        rhs.reinit(introspection.index_sets.system_partitioning, mpi_communicator);
        distributed_solution.reinit(introspection.index_sets.system_partitioning, mpi_communicator);

        const QGauss<dim> quadrature(parameters.stokes_velocity_degree+1);

        FEValues<dim> fe_values (mapping,
                                 finite_element,
                                 quadrature,
                                 update_quadrature_points | update_values | update_gradients);

        const unsigned int dofs_per_cell = fe_values.dofs_per_cell,
                           n_q_points    = fe_values.n_quadrature_points;

        std::vector<unsigned int> cell_dof_indices (dofs_per_cell);
        Vector<double> cell_vector (dofs_per_cell);
        FullMatrix<double> cell_matrix (dofs_per_cell, dofs_per_cell);

        std::vector<double> porosity_values(quadrature.size());
        std::vector<Tensor<1,dim> > grad_p_f_values(quadrature.size());
        std::vector<Tensor<1,dim> > u_s_values(quadrature.size());

        MaterialModel::MaterialModelInputs<dim> in(quadrature.size(), parameters.n_compositional_fields);
        MaterialModel::MaterialModelOutputs<dim> out(quadrature.size(), parameters.n_compositional_fields);
        create_melt_material_outputs(out);

        typename DoFHandler<dim>::active_cell_iterator cell = dof_handler.begin_active(),
                                                       endc = dof_handler.end();
        for (; cell!=endc; ++cell)
          if (cell->is_locally_owned())
            {
              cell_vector = 0;
              cell_matrix = 0;
              cell->get_dof_indices (cell_dof_indices);
              fe_values.reinit (cell);

              fe_values[introspection.extractors.compositional_fields[por_idx]].get_function_values (
                solution, porosity_values);
              fe_values[introspection.extractors.velocities].get_function_values (
                solution, u_s_values);

              fe_values[introspection.variable("fluid pressure").extractor_scalar()].get_function_gradients (
                solution, grad_p_f_values);
              compute_material_model_input_values (solution,
                                                   fe_values,
                                                   cell,
                                                   true, // TODO: use rebuild_stokes_matrix here?
                                                   in);

              material_model->evaluate(in, out);

              MaterialModel::MeltOutputs<dim> *melt_outputs = out.template get_additional_output<MaterialModel::MeltOutputs<dim> >();
              Assert(melt_outputs != NULL, ExcMessage("Need MeltOutputs from the material model for computing the melt variables."));

              for (unsigned int q=0; q<n_q_points; ++q)
                for (unsigned int i=0; i<dofs_per_cell; ++i)
                  {
                    for (unsigned int j=0; j<dofs_per_cell; ++j)
                      cell_matrix(i,j) += fe_values[introspection.variable("fluid velocity").extractor_vector()].value(j,q) *
                                          fe_values[introspection.variable("fluid velocity").extractor_vector()].value(i,q) *
                                          fe_values.JxW(q);

                    const double phi = std::max(0.0, porosity_values[q]);

                    // u_f =  u_s - K_D (nabla p_f - rho_f g) / phi  or = 0
                    if (phi > parameters.melt_transport_threshold)
                      {
                        const double K_D = melt_outputs->permeabilities[q] / melt_outputs->fluid_viscosities[q];
                        const Tensor<1,dim>  gravity = this->gravity_model->gravity_vector(in.position[q]);
                        cell_vector(i) += (u_s_values[q] - K_D * (grad_p_f_values[q] - melt_outputs->fluid_densities[q]*gravity) / phi)
                                          * fe_values[introspection.variable("fluid velocity").extractor_vector()].value(i,q)
                                          * fe_values.JxW(q);
                      }

                  }

              current_constraints.distribute_local_to_global (cell_matrix, cell_vector,
                                                              cell_dof_indices, matrix, rhs, false);
            }

        rhs.compress (VectorOperation::add);
        matrix.compress (VectorOperation::add);



        LinearAlgebra::PreconditionAMG preconditioner;
        LinearAlgebra::PreconditionAMG::AdditionalData Amg_data;
#ifdef ASPECT_USE_PETSC
        Amg_data.symmetric_operator = false;
#else
        //Amg_data.constant_modes = constant_modes;
        Amg_data.elliptic = true;
        Amg_data.higher_order_elements = false;
        Amg_data.smoother_sweeps = 2;
        Amg_data.aggregation_threshold = 0.02;
#endif
        const unsigned int block_idx = introspection.variable("fluid velocity").block_index;
        preconditioner.initialize(matrix.block(block_idx, block_idx));

        SolverControl solver_control(5*rhs.size(), 1e-8*rhs.block(block_idx).l2_norm());
        SolverCG<LinearAlgebra::Vector> cg(solver_control);

        cg.solve (matrix.block(block_idx, block_idx), distributed_solution.block(block_idx), rhs.block(block_idx), preconditioner);
        pcout << "   Solving for u_f in " << solver_control.last_step() <<" iterations."<< std::endl;

        current_constraints.distribute (distributed_solution);
        solution.block(block_idx) = distributed_solution.block(block_idx);
      }
    else
      {
        // u_f =  u_s - K_D (nabla p_f - rho_f g) / phi  or = 0

        const Quadrature<dim> quadrature(finite_element.base_element(
                                           introspection.variable("fluid velocity").base_index).get_unit_support_points());

        MaterialModel::MaterialModelInputs<dim> in(quadrature.size(), parameters.n_compositional_fields);
        MaterialModel::MaterialModelOutputs<dim> out(quadrature.size(), parameters.n_compositional_fields);
        create_melt_material_outputs(out);

        std::vector<double> porosity_values(quadrature.size());
        std::vector<Tensor<1,dim> > grad_p_f_values(quadrature.size());
        std::vector<Tensor<1,dim> > u_s_values(quadrature.size());

        FEValues<dim> fe_values (mapping,
                                 finite_element,
                                 quadrature,
                                 update_quadrature_points | update_values | update_gradients);
        std::vector<types::global_dof_index> local_dof_indices (finite_element.dofs_per_cell);
        typename DoFHandler<dim>::active_cell_iterator
        cell = dof_handler.begin_active(),
        endc = dof_handler.end();
        for (; cell != endc; ++cell)
          if (cell->is_locally_owned())
            {
              fe_values.reinit(cell);
              cell->get_dof_indices (local_dof_indices);
              fe_values[introspection.extractors.compositional_fields[por_idx]].get_function_values (
                solution, porosity_values);
              fe_values[introspection.extractors.velocities].get_function_values (
                solution, u_s_values);
              fe_values[introspection.variable("fluid pressure").extractor_scalar()].get_function_gradients (
                solution, grad_p_f_values);
              compute_material_model_input_values (solution,
                                                   fe_values,
                                                   cell,
                                                   true, // TODO: use rebuild_stokes_matrix here?
                                                   in);

              material_model->evaluate(in, out);

              MaterialModel::MeltOutputs<dim> *melt_outputs = out.template get_additional_output<MaterialModel::MeltOutputs<dim> >();
              Assert(melt_outputs != NULL, ExcMessage("Need MeltOutputs from the material model for computing the melt variables."));

              for (unsigned int j=0; j < finite_element.dofs_per_cell; ++j)
                {
                  std::pair<unsigned int, unsigned int> base_index
                    = finite_element.system_to_base_index(j).first;
                  if (base_index.first != introspection.variable("fluid velocity").base_index)
                    continue;
                  const unsigned int q = finite_element.system_to_base_index(j).second;
                  const unsigned int d = base_index.second;
                  Assert(q < quadrature.size(), ExcInternalError());

                  // skip entries that are not locally owned:
                  if (!dof_handler.locally_owned_dofs().is_element(local_dof_indices[j]))
                    continue;

                  const double phi = std::max(0.0, porosity_values[q]);

                  double value = 0.0;
                  // u_f =  u_s - K_D (nabla p_f - rho_f g) / phi  or = 0
                  if (phi > parameters.melt_transport_threshold)
                    {
                      const double K_D = melt_outputs->permeabilities[q] / melt_outputs->fluid_viscosities[q];
                      const double gravity_d = this->gravity_model->gravity_vector(in.position[q])[d];
                      // v_f =  v_s - K_D (nabla p_f - rho_f g) / phi
                      value = u_s_values[q][d] - K_D * (grad_p_f_values[q][d] - melt_outputs->fluid_densities[q] * gravity_d) / phi;
                    }

                  distributed_vector(local_dof_indices[j]) = value;
                }
            }
        distributed_vector.block(introspection.variable("fluid velocity").block_index).compress(VectorOperation::insert);
        solution.block(introspection.variable("fluid velocity").block_index)
          = distributed_vector.block(introspection.variable("fluid velocity").block_index);
      }

    //compute solid pressure
    {
      const unsigned int block_p = introspection.block_indices.pressure;
      // current_constraints.distribute.

      // Think what we need to do if the pressure is not an FE_Q...
      Assert(parameters.use_locally_conservative_discretization == false, ExcNotImplemented());
      const Quadrature<dim> quadrature(finite_element.base_element(introspection.base_elements.pressure).get_unit_support_points());
      std::vector<double> porosity_values(quadrature.size());
      std::vector<double> p_c_values(quadrature.size());
      std::vector<double> p_f_values(quadrature.size());
      FEValues<dim> fe_values (mapping,
                               finite_element,
                               quadrature,
                               update_quadrature_points | update_values);

      std::vector<types::global_dof_index> local_dof_indices (finite_element.dofs_per_cell);
      typename DoFHandler<dim>::active_cell_iterator
      cell = dof_handler.begin_active(),
      endc = dof_handler.end();
      for (; cell != endc; ++cell)
        if (cell->is_locally_owned())
          {
            fe_values.reinit(cell);
            cell->get_dof_indices (local_dof_indices);
            fe_values[introspection.extractors.compositional_fields[por_idx]].get_function_values (
              solution, porosity_values);
            fe_values[introspection.variable("compaction pressure").extractor_scalar()].get_function_values (
              solution, p_c_values);
            fe_values[introspection.variable("fluid pressure").extractor_scalar()].get_function_values (
              solution, p_f_values);

            for (unsigned int j=0; j<finite_element.base_element(introspection.base_elements.pressure).dofs_per_cell; ++j)
              {
                const unsigned int pressure_idx
                  = finite_element.component_to_system_index(introspection.component_indices.pressure,
                                                             /*dof index within component=*/ j);

                // skip entries that are not locally owned:
                if (!dof_handler.locally_owned_dofs().is_element(local_dof_indices[pressure_idx]))
                  continue;

                const double phi = std::max(0.0, porosity_values[j]);

                double p = p_f_values[j];
                if (phi < 1.0-parameters.melt_transport_threshold)
                  p = (p_c_values[j] - (phi-1.0) * p_f_values[j]) / (1.0-phi);

                distributed_vector(local_dof_indices[pressure_idx]) = p;
              }
          }
      distributed_vector.block(block_p).compress(VectorOperation::insert);
      solution.block(block_p) = distributed_vector.block(block_p);
    }
  }

  template <int dim>
  void Simulator<dim>::convert_pressure_blocks(const LinearAlgebra::BlockVector &input_solution,
                                               const bool                       solid_to_fluid_pressure,
                                               LinearAlgebra::BlockVector       &output_solution)
  {
    if (!parameters.include_melt_transport)
      return;

    // for the direct solver we have to copy the whole block,
    // because the velocity is included as well.
    const unsigned int block_p = introspection.block_indices.pressure;
    output_solution.block(block_p) = input_solution.block(block_p);

    // Think what we need to do if the pressure is not an FE_Q...
    Assert(parameters.use_locally_conservative_discretization == false, ExcNotImplemented());

    const unsigned int por_idx = introspection.compositional_index_for_name("porosity");
    const Quadrature<dim> quadrature(finite_element.base_element(introspection.base_elements.pressure).get_unit_support_points());
    std::vector<double> porosity_values(quadrature.size());
    FEValues<dim> fe_values (mapping,
                             finite_element,
                             quadrature,
                             update_quadrature_points | update_values);

    std::vector<types::global_dof_index> local_dof_indices (finite_element.dofs_per_cell);
    typename DoFHandler<dim>::active_cell_iterator
    cell = dof_handler.begin_active(),
    endc = dof_handler.end();
    for (; cell != endc; ++cell)
      if (cell->is_locally_owned())
        {
          fe_values.reinit(cell);
          cell->get_dof_indices (local_dof_indices);
          fe_values[introspection.extractors.compositional_fields[por_idx]].get_function_values (
            current_linearization_point, porosity_values);

          for (unsigned int j=0; j<finite_element.base_element(introspection.base_elements.pressure).dofs_per_cell; ++j)
            {
              const unsigned int pressure_idx
                = finite_element.component_to_system_index(introspection.component_indices.pressure,
                                                           /*dof index within component=*/ j);

              // skip entries that are not locally owned:
              if (!dof_handler.locally_owned_dofs().is_element(local_dof_indices[pressure_idx]))
                continue;

              const unsigned int p_c_idx
                = finite_element.component_to_system_index(
                    introspection.variable("compaction pressure").first_component_index,
                    /*dof index within component=*/ j);

              double phi = porosity_values[j];

              if (solid_to_fluid_pressure)
                // (p_s, p_f) -> (p_f, p_c)
                {
                  double p_s = input_solution(local_dof_indices[pressure_idx]);
                  double p_f = input_solution(local_dof_indices[p_c_idx]);
                  double p_c = (1-phi)*(p_s-p_f);

                  output_solution(local_dof_indices[pressure_idx]) = p_f;
                  output_solution(local_dof_indices[p_c_idx]) = p_c;
                }
              else
                // (p_f, p_c) -> (p_s, p_f)
                {
                  double p_f = input_solution(local_dof_indices[pressure_idx]);
                  double p_s;
                  if (phi >(1.0-parameters.melt_transport_threshold) || (phi <= 0.0))
                    p_s = p_f;
                  else
                    {
                      double p_c = input_solution(local_dof_indices[p_c_idx]);
                      p_s = (p_c - (phi-1.0) * p_f) / (1.0-phi);
                    }

                  output_solution(local_dof_indices[pressure_idx]) = p_s;
                  output_solution(local_dof_indices[p_c_idx]) = p_f;
                }
            }
        }
    output_solution.block(block_p).compress(VectorOperation::insert);
  }


  template <int dim>
  double
  Simulator<dim>::compute_initial_stokes_residual()
  {
    LinearAlgebra::BlockVector remap (introspection.index_sets.stokes_partitioning, mpi_communicator);
    LinearAlgebra::BlockVector residual (introspection.index_sets.stokes_partitioning, mpi_communicator);
    const unsigned int block_p =
      parameters.include_melt_transport ?
      introspection.variable("fluid pressure").block_index
      :
      introspection.block_indices.pressure;

    // if velocity and pressure are in the same block, we have to copy the
    // pressure to the solution and RHS vector with a zero velocity
    if (block_p == introspection.block_indices.velocities)
      {
        const IndexSet &idxset = (parameters.include_melt_transport) ?
                                 introspection.index_sets.locally_owned_fluid_pressure_dofs
                                 :
                                 introspection.index_sets.locally_owned_pressure_dofs;

        for (unsigned int i=0; i < idxset.n_elements(); ++i)
          {
            types::global_dof_index idx = idxset.nth_index_in_set(i);
            remap(idx)        = current_linearization_point(idx);
          }
        remap.block(block_p).compress(VectorOperation::insert);
      }
    else
      remap.block (block_p) = current_linearization_point.block (block_p);

    // TODO: we don't have .stokes_relevant_partitioning so I am creating a much
    // bigger vector here, oh well.
    LinearAlgebra::BlockVector ghosted (introspection.index_sets.system_partitioning,
                                        introspection.index_sets.system_relevant_partitioning,
                                        mpi_communicator);
    // TODO for Timo: can we create the ghost vector inside of denormalize_pressure
    // (only in cases where we need it)
    ghosted.block(block_p) = remap.block(block_p);
    denormalize_pressure (remap, ghosted);
    current_constraints.set_zero (remap);

    remap.block (block_p) /= pressure_scaling;

    // we calculate the velocity residual with a zero velocity,
    // computing only the part of the RHS not balanced by the static pressure
    if (block_p == introspection.block_indices.velocities)
      {
        // we can use the whole block here because we set the velocity to zero above
        return system_matrix.block(0,0).residual (residual.block(0),
                                                  remap.block(0),
                                                  system_rhs.block(0));
      }
    else
      {
        double residual_u = system_matrix.block(0,1).residual (residual.block(0),
                                                               remap.block(1),
                                                               system_rhs.block(0));
        double residual_p = system_rhs.block(block_p).l2_norm();
        return sqrt(residual_u*residual_u+residual_p*residual_p);
      }
  }

  template <int dim>
  bool
  Simulator<dim>::stokes_matrix_depends_on_solution() const
  {
    // currently, the only coefficient that really appears on the
    // left hand side of the Stokes equation is the viscosity. note
    // that our implementation of compressible materials makes sure
    // that the density does not appear on the lhs.
    // if melt transport is included in the simulation, we have an
    // additional equation with more coefficients on the left hand
    // side.

    return (material_model->get_model_dependence().viscosity != MaterialModel::NonlinearDependence::none)
           || parameters.include_melt_transport;
  }
}

// explicit instantiation of the functions we implement in this file
namespace aspect
{
#define INSTANTIATE(dim) \
  template struct Simulator<dim>::AdvectionField; \
  template void Simulator<dim>::normalize_pressure(LinearAlgebra::BlockVector &vector); \
  template void Simulator<dim>::denormalize_pressure(LinearAlgebra::BlockVector &vector, const LinearAlgebra::BlockVector &relevant_vector); \
  template double Simulator<dim>::get_maximal_velocity (const LinearAlgebra::BlockVector &solution) const; \
  template std::pair<double,double> Simulator<dim>::get_extrapolated_advection_field_range (const AdvectionField &advection_field) const; \
  template std::pair<double,bool> Simulator<dim>::compute_time_step () const; \
  template void Simulator<dim>::make_pressure_rhs_compatible(LinearAlgebra::BlockVector &vector); \
  template void Simulator<dim>::output_program_stats(); \
  template void Simulator<dim>::output_statistics(); \
  template void Simulator<dim>::convert_pressure_blocks(const LinearAlgebra::BlockVector &input_solution, const bool solid_to_fluid_pressure, LinearAlgebra::BlockVector &output_solution); \
  template void Simulator<dim>::compute_melt_variables(LinearAlgebra::BlockVector &solution); \
  template double Simulator<dim>::compute_initial_stokes_residual(); \
  template bool Simulator<dim>::stokes_matrix_depends_on_solution() const; \
  template void Simulator<dim>::interpolate_onto_velocity_system(const TensorFunction<1,dim> &func, LinearAlgebra::Vector &vec);

  ASPECT_INSTANTIATE(INSTANTIATE)
}
