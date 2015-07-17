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


#include <aspect/mesh_refinement/composition_threshold.h>

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/fe/fe_values.h>

namespace aspect
{
  namespace MeshRefinement
  {
    template <int dim>
    void
    CompositionThreshold<dim>::execute(Vector<float> &indicators) const
    {
      AssertThrow (this->n_compositional_fields() >= 1,
                   ExcMessage ("This refinement criterion can not be used when no "
                               "compositional fields are active!"));

      indicators = 0;

      const Quadrature<dim> quadrature(this->get_fe().base_element(this->introspection().base_elements.compositional_fields).get_unit_support_points());
      FEValues<dim> fe_values (this->get_mapping(),
                               this->get_fe(),
                               quadrature,
                               update_quadrature_points | update_values);

      // the values of the compositional fields are stored as blockvectors for each field
      // we have to extract them in this structure
      std::vector<double> composition_values (quadrature.size());

      for (unsigned int c=0; c<this->n_compositional_fields(); ++c)
        {
          typename DoFHandler<dim>::active_cell_iterator
          cell = this->get_dof_handler().begin_active(),
          endc = this->get_dof_handler().end();
          unsigned int i=0;
          for (; cell!=endc; ++cell, ++i)
            if (cell->is_locally_owned())
              {
                fe_values.reinit(cell);
                fe_values[this->introspection().extractors.compositional_fields[c]].get_function_values (this->get_solution(),
                                                                                                         composition_values);

                // if the composition exceeds the threshold, cell is marked for refinement
                for (unsigned int j=0; j<this->get_fe().base_element(this->introspection().base_elements.compositional_fields).dofs_per_cell; ++j)
                  if (composition_values[j] > composition_thresholds[c])
                    indicators[i] = 1.0;
              }
        }
    }

    template <int dim>
    void
    CompositionThreshold<dim>::
    declare_parameters (ParameterHandler &prm)
    {
      prm.enter_subsection("Mesh refinement");
      {
        prm.enter_subsection("Composition threshold");
        {
          prm.declare_entry("Compositional field thresholds",
                            "",
                            Patterns::List (Patterns::Double(0)),
                            "A list of thresholds that every individual compositional "
                            "field will be evaluated against. "
                            "\n\n"
                            "If the list of scaling factors given in this parameter is empty, then this "
                            "indicates that they should all be chosen equal to one. If the list "
                            "is not empty then it needs to have as many entries as there are "
                            "compositional fields.");
        }
        prm.leave_subsection();
      }
      prm.leave_subsection();
    }

    template <int dim>
    void
    CompositionThreshold<dim>::parse_parameters (ParameterHandler &prm)
    {
      prm.enter_subsection("Mesh refinement");
      {
        prm.enter_subsection("Composition threshold");
        {
          composition_thresholds
            = Utilities::string_to_double(
                Utilities::split_string_list(prm.get("Compositional field thresholds")));

          AssertThrow (composition_thresholds.size() == this->n_compositional_fields()
                       ||
                       composition_thresholds.size() == 0,
                       ExcMessage ("The number of thresholds given here must either be "
                                   "zero or equal to the number of chosen refinement criteria."));

          if (composition_thresholds.size() == 0)
            composition_thresholds.resize (this->n_compositional_fields(), 1.0);
        }
        prm.leave_subsection();
      }
      prm.leave_subsection();
    }
  }
}

// explicit instantiations
namespace aspect
{
  namespace MeshRefinement
  {
    ASPECT_REGISTER_MESH_REFINEMENT_CRITERION(CompositionThreshold,
                                              "composition threshold",
                                              "A mesh refinement criterion that computes refinement "
                                              "indicators from the compositional fields. If it exceeds "
                                              "a threshold given in the input file, the cell is marked "
                                              "for refinement. "
                                              "If there is more than one compositional field, then "
                                              "all the fields are evaluated with their individual thresholds.")
  }
}
