/*
  Copyright (C) 2013 by the authors of the ASPECT code.

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


#ifndef __aspect__fluid_pressure_boundary_conditions_interface_h
#define __aspect__fluid_pressure_boundary_conditions_interface_h

#include <aspect/plugins.h>
#include <aspect/material_model/melt_interface.h>

namespace aspect
{
  /**
   * A namespace for the definition of things that have to do with describing
   * the boundary values for fluid pressure for computations with melt
   * transport.
   *
   * @ingroup FluidPressureBoundaryConditions
   */
  namespace FluidPressureBoundaryConditions
  {
    using namespace dealii;

    /**
     * Base class
     *
     * @ingroup FluidPressureBoundaryConditions
     */
    template <int dim>
    class Interface
    {
      public:
        /**
         * Destructor. Made virtual to enforce that derived classes also have
         * virtual destructors.
         */
        virtual ~Interface();

        /**
         * Initialization function. This function is called once at the
         * beginning of the program after parse_parameters is run and after the
         * SimulatorAccess (if applicable) is initialized.
         */
        virtual void initialize ();


        /**
         * Compute the gradient of the fluid pressure in the direction of gravity
         * for each quadrature point.
         *
         * The return value can typically contain @p material_model_outputs.fluid_densities[q]
         * or @p material_model_outputs.densities[q].
         *
         * @param material_model_inputs The material property inputs.
         * @param material_model_inputs The material property outputs.
         * @param output Result to be filled.
         */
        virtual
        void fluid_pressure (
            const typename MaterialModel::MeltInterface<dim>::MaterialModelInputs &material_model_inputs,
            const typename MaterialModel::MeltInterface<dim>::MaterialModelOutputs &material_model_outputs,
            std::vector<double> & output
            ) const = 0;

        /**
         * Declare the parameters this class takes through input files. The
         * default implementation of this function does not describe any
         * parameters. Consequently, derived classes do not have to overload
         * this function if they do not take any runtime parameters.
         */
        static
        void
        declare_parameters (ParameterHandler &prm);

        /**
         * Read the parameters this class declares from the parameter file.
         * The default implementation of this function does not read any
         * parameters. Consequently, derived classes do not have to overload
         * this function if they do not take any runtime parameters.
         */
        virtual
        void
        parse_parameters (ParameterHandler &prm);
    };


    /**
     * Register a boundary composition model so that it can be selected from
     * the parameter file.
     *
     * @param name A string that identifies the boundary composition model
     * @param description A text description of what this model does and that
     * will be listed in the documentation of the parameter file.
     * @param declare_parameters_function A pointer to a function that can be
     * used to declare the parameters that this geometry model wants to read
     * from input files.
     * @param factory_function A pointer to a function that can create an
     * object of this boundary composition model.
     *
     * @ingroup FluidPressureBoundaryConditions
     */
    template <int dim>
    void
    register_fluid_pressure_boundary (const std::string &name,
                                   const std::string &description,
                                   void (*declare_parameters_function) (ParameterHandler &),
                                   Interface<dim> *(*factory_function) ());

    /**
     * A function that given the name of a model returns a pointer to an
     * object that describes it. Ownership of the pointer is transferred to
     * the caller.
     *
     * The model object returned is not yet initialized and has not
     * read its runtime parameters yet.
     *
     * @ingroup FluidPressureBoundaryConditions
     */
    template <int dim>
    Interface<dim> *
    create_fluid_pressure_boundary (ParameterHandler &prm);


    /**
     * Declare the runtime parameters of the registered boundary composition
     * models.
     *
     * @ingroup FluidPressureBoundaryConditions
     */
    template <int dim>
    void
    declare_parameters (ParameterHandler &prm);


    /**
     * Given a class name, a name, and a description for the parameter file
     * for a boundary composition model, register it with the functions that
     * can declare their parameters and create these objects.
     *
     * @ingroup FluidPressureBoundaryConditions
     */
#define ASPECT_REGISTER_FLUID_PRESSURE_BOUNDARY_CONDITIONS(classname, name, description) \
  template class classname<2>; \
  template class classname<3>; \
  namespace ASPECT_REGISTER_FLUID_PRESSURE_BOUNDARY_CONDITIONS_ ## classname \
  { \
    aspect::internal::Plugins::RegisterHelper<aspect::FluidPressureBoundaryConditions::Interface<2>,classname<2> > \
    dummy_ ## classname ## _2d (&aspect::FluidPressureBoundaryConditions::register_fluid_pressure_boundary<2>, \
                                name, description); \
    aspect::internal::Plugins::RegisterHelper<aspect::FluidPressureBoundaryConditions::Interface<3>,classname<3> > \
    dummy_ ## classname ## _3d (&aspect::FluidPressureBoundaryConditions::register_fluid_pressure_boundary<3>, \
                                name, description); \
  }
  }
}


#endif
