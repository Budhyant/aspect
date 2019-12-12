/*
  Copyright (C) 2011 - 2017 by the authors of the ASPECT code.

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
  along with ASPECT; see the file LICENSE.  If not see
  <http://www.gnu.org/licenses/>.
*/


#ifndef _aspect_postprocess_visualization_averaged_strain_rate_h
#define _aspect_postprocess_visualization_averaged_strain_rate_h

#include <aspect/postprocess/visualization.h>
#include <aspect/simulator_access.h>

#include <deal.II/numerics/data_postprocessor.h>


namespace aspect
{
  namespace Postprocess
  {
    namespace VisualizationPostprocessors
    {
      /**
       * Compute an averaged strain rate per cell.
       */
      template <int dim>
      class AveragedStrainRate
        : public CellDataVectorCreator<dim>,
          public SimulatorAccess<dim>
      {
        public:
          AveragedStrainRate ();

          /**
           * @copydoc CellDataVectorCreator<dim>::execute()
           */
          virtual
          std::pair<std::string, Vector<float> *>
          execute () const;

          virtual
          std::list<std::pair<std::string, Vector<float> *>>
                                                          execute2 () const;

        private:
          std::pair<std::string, Vector<float> *>
          compute (const unsigned int quadrature_degree, const bool compressible, const std::string &name) const;
      };
    }
  }
}

#endif
