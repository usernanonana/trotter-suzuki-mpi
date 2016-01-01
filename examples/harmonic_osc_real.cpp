/**
 * Massively Parallel Trotter-Suzuki Solver
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <fstream>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef HAVE_MPI
#include <mpi.h>
#endif
#include "trottersuzuki.h"

#define EDGE_LENGTH 14.14     //Physical length of the grid's edge
#define DIM 256         //Number of dots of the grid's edge
#define DELTA_T 1.e-4     //Time step evolution
#define ITERATIONS 1000     //Number of iterations before calculating expected values
#define KERNEL_TYPE "gpu"
#define SNAPSHOTS 20      //Number of times the expected values are calculated
#define SNAP_PER_STAMP 5    //The particles density and phase of the wave function are stamped every "SNAP_PER_STAMP" expected values calculations
#define COUPLING_CONST_2D 0   // 0 for linear Schrodinger equation
#define PARTICLES_NUM 1     //Particle numbers (nonlinear Schrodinger equation)

int rot_coord_x = 320, rot_coord_y = 320;
double omega = 0.;

std::complex<double> gauss_ini_state(int m, int n, Lattice *grid) {
    double x = (m - grid->global_dim_x / 2.) * grid->delta_x;
    double y = (n - grid->global_dim_y / 2.) * grid->delta_x;
    double w = 1.;
    return std::complex<double>(sqrt(0.5 * w / M_PI) * exp(-(x * x + y * y) * 0.5 * w) * (1. + sqrt(2. * w) * x), 0.);
}

double parabolic_potential(int m, int n, Lattice *grid) {
    double x = (m - grid->global_dim_x / 2.) * grid->delta_x, y = (n - grid->global_dim_x / 2.) * grid->delta_x;
    double w_x = 1., w_y = 1.; 
    return 0.5 * (w_x * w_x * x * x + w_y * w_y * y * y);
}


int main(int argc, char** argv) {
    int periods[2] = {0, 0};
    char file_name[] = "";
    char pot_name[1] = "";
    const double particle_mass = 1.;
    bool imag_time = false;
    double h_a = 0.;
    double h_b = 0.;
    int time, tot_time = 0;
    double delta_t = double(DELTA_T);
    double delta_x = double(EDGE_LENGTH)/double(DIM), delta_y = double(EDGE_LENGTH)/double(DIM);

#ifdef HAVE_MPI
    MPI_Init(&argc, &argv);
#endif
    Lattice *grid = new Lattice(DIM, delta_x, delta_y, periods, omega);

    //set and calculate evolution operator variables from hamiltonian
    double *external_pot_real = new double[grid->dim_x * grid->dim_y];
    double *external_pot_imag = new double[grid->dim_x * grid->dim_y];
    
    //set and calculate evolution operator variables from hamiltonian
    double time_single_it;
    double coupling_const = double(COUPLING_CONST_2D);

    if(imag_time) {
        time_single_it = delta_t / 2.;  //second approx trotter-suzuki: time/2
        if(h_a == 0. && h_b == 0.) {
            h_a = cosh(time_single_it / (2. * particle_mass * delta_x * delta_y));
            h_b = sinh(time_single_it / (2. * particle_mass * delta_x * delta_y));
        }
    }
    else {
        time_single_it = delta_t / 2.;  //second approx trotter-suzuki: time/2
        if(h_a == 0. && h_b == 0.) {
            h_a = cos(time_single_it / (2. * particle_mass * delta_x * delta_y));
            h_b = sin(time_single_it / (2. * particle_mass * delta_x * delta_y));
        }
    }
    initialize_exp_potential(grid, external_pot_real, external_pot_imag, 
                             parabolic_potential, time_single_it, 
                             particle_mass, imag_time);

    //set initial state
    State *state = new State(grid);
    state->init_state(gauss_ini_state);
    Hamiltonian *hamiltonian = new Hamiltonian(grid, particle_mass, coupling_const, 0, 0, rot_coord_x, rot_coord_y, omega);
    //set file output directory
    std::stringstream dirname, file_info;
    std::string dirnames, file_infos;
    if (SNAPSHOTS) {
        int status = 0;
        dirname.str("");
        dirname << "Harmonic_osc_RE";
        dirnames = dirname.str();
        status = mkdir(dirnames.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        if(status != 0 && status != -1)
            dirnames = ".";
    } else {
        dirnames = ".";
    }
    file_info.str("");
    file_info << dirnames << "/file_info.txt";
    file_infos = file_info.str();
    std::ofstream out(file_infos.c_str());
    
    double *_matrix = new double[grid->dim_x * grid->dim_y];
    double _mean_positions[4], _mean_momenta[4], _norm2, sum, results[4];
    
    //norm calculation
    double norm2 = state->calculate_squared_norm();
    double _tot_energy = calculate_total_energy(grid, state, hamiltonian, parabolic_potential, NULL, norm2);
    double _kin_energy = calculate_kinetic_energy(grid, state, hamiltonian, norm2);

    //Position expected values
    calculate_mean_position(grid, state, grid->dim_x / 2, grid->dim_y / 2, _mean_positions, norm2);

    //Momenta expected values
    calculate_mean_momentum(grid, state,_mean_momenta, norm2);
                   
    //get and stamp phase
    state->get_phase(_matrix);
    stamp_real(grid, _matrix, 0, dirnames.c_str(), "phase");

    //get and stamp particles density
    state->get_particle_density(_matrix);
    stamp_real(grid, _matrix, 0, dirnames.c_str(), "density");
    
    if (grid->mpi_rank == 0){
      out << "iterations\tsquared norm\ttotal_energy\tkinetic_energy\t<X>\t<(X-<X>)^2>\t<Y>\t<(Y-<Y>)^2>\t<Px>\t<(Px-<Px>)^2>\t<Py>\t<(Py-<Py>)^2>\n";
      out << "0\t\t" << norm2 << "\t\t"<< _tot_energy << "\t" << _kin_energy << "\t" << _mean_positions[0] << "\t" << _mean_positions[1] << "\t" << _mean_positions[2] << "\t" << _mean_positions[3] << "\t" << _mean_momenta[0] << "\t" << _mean_momenta[1] << "\t" << _mean_momenta[2] << "\t" << _mean_momenta[3] << std::endl;
    }
  
    struct timeval start, end;
    for (int count_snap = 0; count_snap < SNAPSHOTS; count_snap++) {
        
        gettimeofday(&start, NULL);
        trotter(grid, state, hamiltonian, h_a, h_b, external_pot_real, external_pot_imag, delta_t, ITERATIONS, KERNEL_TYPE, norm2, imag_time);
        gettimeofday(&end, NULL);
        time = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
        tot_time += time;
        
        //norm calculation
        _norm2 = state->calculate_squared_norm();
        _tot_energy = calculate_total_energy(grid, state, hamiltonian, parabolic_potential, NULL, _norm2);
        _kin_energy = calculate_kinetic_energy(grid, state, hamiltonian, norm2);
              
        //Position expected values
        calculate_mean_position(grid, state, grid->dim_x / 2, grid->dim_y / 2, _mean_positions, norm2);

        //Momenta expected values
        calculate_mean_momentum(grid, state,_mean_momenta, norm2);

        if(grid->mpi_rank == 0){
            out << (count_snap + 1) * ITERATIONS << "\t\t" << norm2 << "\t\t"<< _tot_energy << "\t" << _kin_energy << "\t" << _mean_positions[0] << "\t" << _mean_positions[1] << "\t" << _mean_positions[2] << "\t" << _mean_positions[3] << "\t" << _mean_momenta[0] << "\t" << _mean_momenta[1] << "\t" << _mean_momenta[2] << "\t" << _mean_momenta[3] << std::endl;
        }
    
        //stamp phase and particles density
        if((count_snap + 1) % SNAP_PER_STAMP == 0.) {
            //get and stamp phase
            state->get_phase(_matrix);
            stamp_real(grid, _matrix, ITERATIONS * (count_snap + 1), dirnames.c_str(), "phase");

            //get and stamp particles density
            state->get_particle_density(_matrix);
            stamp_real(grid, _matrix, ITERATIONS * (count_snap + 1), dirnames.c_str(), "density");
        }
    }
    out.close();
    
    if (grid->mpi_rank == 0) {
        std::cout << "TROTTER " << DIM << "x" << DIM << " kernel:" << KERNEL_TYPE << " np:" << grid->mpi_procs << " time:" << tot_time << " usec" << std::endl;
    }
    delete hamiltonian;
    delete state;
    delete grid;
    return 0;
}
