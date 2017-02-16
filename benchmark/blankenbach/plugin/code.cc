#include <aspect/global.h>

#include <aspect/postprocess/interface.h>
#include <aspect/simulator_access.h>

#include <deal.II/base/data_out_base.h>

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/numerics/vector_tools.h>

#include <math.h>
#include <algorithm>


namespace aspect
{
  namespace Postprocess
  {

    /**
     * A postprocessor that generates ascii data output of the temperature
     * field to be used as initial condition.
     *
     * @ingroup Postprocessing
     */
    template <int dim>
    class TemperatureAsciiOut : public Interface<dim>, public ::aspect::SimulatorAccess<dim>
    {
    public:
      /**
       * Constructor.
       */
      TemperatureAsciiOut ();

      /**
       * Evaluate the solution and compute the requested depth averages.
       */
      virtual
      std::pair<std::string,std::string>
      execute (TableHandler &statistics);

      /**
       * Declare the parameters this class takes through input files.
       */
      static
      void
      declare_parameters (ParameterHandler &prm);

      /**
       * Read the parameters this class declares from the parameter file.
       */
      virtual
      void
      parse_parameters (ParameterHandler &prm);

    private:
      /**
       * Interval between the generation of output in seconds. This parameter is read
       * from the input file and consequently is not part of the state that
       * needs to be saved and restored.
       */
      double output_interval;

      /**
       * A time (in seconds) the last output has been produced.
       */
      double last_output_time;

      /**
       * Set the time output was supposed to be written. In the simplest
       * case, this is the previous last output time plus the interval, but
       * in general we'd like to ensure that it is the largest supposed
       * output time, which is smaller than the current time, to avoid
       * falling behind with last_output_time and having to catch up once
       * the time step becomes larger. This is done after every output.
       */
      void set_last_output_time (const double current_time);
    };

    template <int dim>
    TemperatureAsciiOut<dim>::TemperatureAsciiOut()
    {}

    template <int dim>
    std::pair<std::string,std::string>
    TemperatureAsciiOut<dim>::execute (TableHandler &)
    {
      std::string filename = (this->get_output_directory() + "temperature_ascii_data.txt");

      struct entry
      {
        Point<dim> p;
        double t;
      };
      std::vector<entry> entries;

      const QIterated<dim> quadrature_formula (QTrapez<1>(), 2);
      //      const QMidpoint<dim> quadrature_formula;

      const unsigned int n_q_points =  quadrature_formula.size();
      FEValues<dim> fe_values (this->get_mapping(), this->get_fe(),  quadrature_formula,
                               update_JxW_values | update_values | update_quadrature_points);

      std::vector<double> temperature_values(n_q_points);

      typename DoFHandler<dim>::active_cell_iterator
      cell = this->get_dof_handler().begin_active(),
      endc = this->get_dof_handler().end();
      for (; cell != endc; ++cell)
        {
          fe_values.reinit (cell);
          fe_values[this->introspection().extractors.temperature].get_function_values (this->get_solution(), temperature_values);

          for (unsigned int q=0; q<fe_values.n_quadrature_points; ++q)
            {
              entry e;
              e.p = fe_values.quadrature_point(q);
              e.t = temperature_values[q];
              entries.push_back(e);
            }
        }

      struct sorter
      {
        bool operator() (const entry &i, const entry &j)
        {
          if (std::abs(i.p[1]-j.p[1])<1e-6)
            return i.p[0] < j.p[0];
          else
            return i.p[1] < j.p[1];
        }
      } sorter_instance;
      struct comparer
     {
        bool operator() (const entry &i, const entry &j)
        {
          return (std::abs(i.p[0]-j.p[0])<1e-6)
      && (std::abs(i.p[1]-j.p[1])<1e-6);
        }
      } comparer_instance;
      std::sort(entries.begin(), entries.end(), sorter_instance);
      entries.erase( std::unique( entries.begin(), entries.end(), comparer_instance ), entries.end() );



      unsigned int n_x, n_y;
      {
        // find unique number of x and y values by stuffing them into a std::set
        struct sort_double
        {
          bool operator() (const double &i, const double &j)
          {
            return i<j;
          }
        } sort_double_instance;
        struct compare_double_eps
        {
          bool operator() (const double &i, const double &j)
          {
            return (std::abs(i-j)<1e-6);
          }
        } compare_double_instance;

        std::vector<double> x_values, y_values;
        for (unsigned int idx = 0; idx< entries.size(); ++idx)
          {
            x_values.push_back(entries[idx].p(0));
            y_values.push_back(entries[idx].p(1));
          }
        std::sort(x_values.begin(), x_values.end(), sort_double_instance);
        x_values.erase( std::unique( x_values.begin(), x_values.end(), compare_double_instance ), x_values.end() );
        std::sort(y_values.begin(), y_values.end(), sort_double_instance);
        y_values.erase( std::unique( y_values.begin(), y_values.end(), compare_double_instance ), y_values.end() );

        n_x = x_values.size();
        n_y = y_values.size();
      }


      std::ofstream f(filename.c_str());

      f << std::scientific << std::setprecision(15);

      // Note: POINTS is only useful if our mesh is a structured grid
      f << "# POINTS: " << n_x << " " << n_y << "\n";
      f << "# x y T\n";

      for (unsigned int idx = 0; idx< entries.size(); ++idx)
        f << entries[idx].p << ' ' << entries[idx].t << '\n';

      f.close();

      // return what should be printed to the screen. note that we had
      // just incremented the number, so use the previous value
      return std::make_pair (std::string ("Writing TemperatureAsciiOut:"),
                             filename);
    }


    template <int dim>
    void
    TemperatureAsciiOut<dim>::declare_parameters (ParameterHandler &/*prm*/)
    {
    }


    template <int dim>
    void
    TemperatureAsciiOut<dim>::parse_parameters (ParameterHandler &/*prm*/)
    {
    }
  }
}


// explicit instantiations
namespace aspect
{
  namespace Postprocess
  {
    ASPECT_REGISTER_POSTPROCESSOR(TemperatureAsciiOut,
                                  "temperature ascii out",
                                  "A postprocessor that generates ascii data "
                                  "output of the temperature field to be "
                                  "used as initial condition.")
  }
}
