/*
 * <Simple MD Module for Cucuma. >
 * Copyright (C) 2020 - 2024 Conrad Hübler <Conrad.Huebler@gmx.net>
 *               2024 Gerd Gehrisch
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "src/capabilities/curcumaopt.h"
#include "src/capabilities/rmsd.h"
#include "src/capabilities/rmsdtraj.h"

#include "src/core/elements.h"
#include "src/core/energycalculator.h"
#include "src/core/fileiterator.h"
#include "src/core/global.h"
#include "src/core/molecule.h"

#include "src/tools/geometry.h"

#include "external/CxxThreadPool/include/CxxThreadPool.hpp"

#ifdef USE_Plumed
#include "plumed2/src/wrapper/Plumed.h"
#endif
#include "simplemd.h"

// Claude Generated: Unit conversion constants for wall statistics
const double au2eV = 1.0 / eV2Eh; // Convert Hartree to eV
const double au2N = 8.2387225e-8; // Convert atomic force units (Eh/bohr) to Newton

BiasThread::BiasThread(const Molecule& reference, const json& rmsdconfig, bool nocolvarfile, bool nohillsfile)
    : m_reference(reference)
    , m_target(reference)
    , m_nocolvarfile(nocolvarfile)
    , m_nohillsfile(nohillsfile)
{
    m_driver = RMSDDriver(rmsdconfig, true);
    m_config = rmsdconfig;
    setAutoDelete(true);
    m_current_bias = 0;
    m_counter = 0;
    m_atoms = m_reference.AtomCount();
    m_gradient = Eigen::MatrixXd::Zero(m_reference.AtomCount(), 3);
}

BiasThread::~BiasThread()
{
}

int BiasThread::execute()
{
    if (m_biased_structures.empty())
        return 0;
    m_current_bias = 0;
    m_counter = 0;
    m_driver.setReference(m_reference);
    m_gradient = Eigen::MatrixXd::Zero(m_reference.AtomCount(), 3);

    for (int i = 0; i < m_biased_structures.size(); ++i) {
        double factor = 1;
        m_target.setGeometry(m_biased_structures[i].geometry);
        m_driver.setTarget(m_target);
        double rmsd = m_driver.BestFitRMSD();
        double expr = exp(-rmsd * rmsd * m_alpha);
        double bias_energy = expr * m_dT;
        factor = m_biased_structures[i].factor;

        if (!m_wtmtd)
            factor = m_biased_structures[i].counter;
        else
            factor += (exp(-(m_biased_structures[i].energy) / kb_Eh / m_DT));
        m_biased_structures[i].factor = factor;
        if (i == 0) {
            m_rmsd_reference = rmsd;
        }
        if (expr * m_rmsd_econv > 1 * m_biased_structures.size()) {
            m_biased_structures[i].counter++;
            m_biased_structures[i].energy += bias_energy;
        }
        bias_energy *= factor * m_k;

        m_current_bias += bias_energy;
        if (m_nocolvarfile == false) {
            std::ofstream colvarfile;
            colvarfile.open("COLVAR_" + std::to_string(m_biased_structures[i].index), std::iostream::app);
            colvarfile << m_currentStep << " " << rmsd << " " << bias_energy << " " << m_biased_structures[i].counter << " " << factor << std::endl;
            colvarfile.close();
        }
        /*
        if(nohillsfile == false)
        {
            std::ofstream hillsfile;
            if (i == 0) {
                hillsfile.open("HILLS", std::iostream::app);
            } else {
                hillsfile.open("HILLS_" + std::to_string(m_biased_structures[i].index), std::iostream::app);
            }
            hillsfile << m_currentStep << " " << rmsd << " " << m_alpha_rmsd << " " << m_k_rmsd << " " << "-1" << std::endl;
            hillsfile.close();
        }
        */

        double dEdR = -2 * m_alpha * m_k / m_atoms * exp(-rmsd * rmsd * m_alpha) * factor * m_dT;

        m_gradient += m_driver.Gradient() * dEdR;
        m_counter += m_biased_structures[i].counter;
    }
    return 1;
}

std::vector<json> BiasThread::getBias() const
{
    std::vector<json> bias(m_biased_structures.size());
    for (int i = 0; i < m_biased_structures.size(); ++i) {
        json current;
        // current["geometry"] = Tools::Matrix2String(m_biased_structures[i].geometry);
        current["time"] = m_biased_structures[i].time;
        current["rmsd_reference"] = m_biased_structures[i].rmsd_reference;
        current["energy"] = m_biased_structures[i].energy;
        current["factor"] = m_biased_structures[i].factor;
        current["index"] = m_biased_structures[i].index;
        current["counter"] = m_biased_structures[i].counter;
        bias[i] = current;
    }
    return bias;
}

SimpleMD::SimpleMD(const json& controller, const bool silent)
    : CurcumaMethod(CurcumaMDJson, controller, silent)
{
    UpdateController(controller);
}

SimpleMD::~SimpleMD()
{
    for (const auto & m_unique_structure : m_unique_structures)
        delete m_unique_structure;
    // delete m_bias_pool;
}

void SimpleMD::LoadControlJson()
{
    m_method = Json2KeyWord<std::string>(m_defaults, "method");
    m_thermostat = Json2KeyWord<std::string>(m_defaults, "thermostat");
    m_plumed = Json2KeyWord<std::string>(m_defaults, "plumed");

    m_spin = Json2KeyWord<int>(m_defaults, "spin");
    m_charge = Json2KeyWord<int>(m_defaults, "charge");
    m_dT = Json2KeyWord<double>(m_defaults, "dT");
    m_maxtime = Json2KeyWord<double>(m_defaults, "MaxTime");
    m_T0 = Json2KeyWord<double>(m_defaults, "T");
    m_rmrottrans = Json2KeyWord<int>(m_defaults, "rmrottrans");
    m_nocenter = Json2KeyWord<bool>(m_defaults, "nocenter");
    m_COM = Json2KeyWord<bool>(m_defaults, "COM");
    m_dump = Json2KeyWord<int>(m_defaults, "dump");
    m_print = Json2KeyWord<int>(m_defaults, "print");
    m_max_top_diff = Json2KeyWord<int>(m_defaults, "MaxTopoDiff");
    m_seed = Json2KeyWord<int>(m_defaults, "seed");
    m_threads = Json2KeyWord<int>(m_defaults, "threads");

    m_rmsd = Json2KeyWord<double>(m_defaults, "rmsd");
    m_hmass = Json2KeyWord<int>(m_defaults, "hmass");

    m_impuls = Json2KeyWord<double>(m_defaults, "impuls");
    m_impuls_scaling = Json2KeyWord<double>(m_defaults, "impuls_scaling");
    m_writeUnique = Json2KeyWord<bool>(m_defaults, "unique");
    m_opt = Json2KeyWord<bool>(m_defaults, "opt");
    m_scale_velo = Json2KeyWord<double>(m_defaults, "velo");
    m_rescue = Json2KeyWord<bool>(m_defaults, "rescue");
    m_wall_render = Json2KeyWord<bool>(m_defaults, "wall_render");
    m_coupling = Json2KeyWord<double>(m_defaults, "coupling");
    m_anderson = Json2KeyWord<double>(m_defaults, "anderson");

    if (m_coupling < m_dT)
        m_coupling = m_dT;

    /* RMSD Metadynamik block */
    /* this one is used to recover https://doi.org/10.1021/acs.jctc.9b00143 */
    m_rmsd_mtd = Json2KeyWord<bool>(m_defaults, "rmsd_mtd");
    m_k_rmsd = Json2KeyWord<double>(m_defaults, "k_rmsd");
    m_alpha_rmsd = Json2KeyWord<double>(m_defaults, "alpha_rmsd");
    m_mtd_steps = Json2KeyWord<int>(m_defaults, "mtd_steps");
    m_chain_length = Json2KeyWord<int>(m_defaults, "chainlength");
    m_rmsd_rmsd = Json2KeyWord<double>(m_defaults, "rmsd_rmsd");
    m_max_rmsd_N = Json2KeyWord<int>(m_defaults, "max_rmsd_N");
    m_rmsd_econv = Json2KeyWord<double>(m_defaults, "rmsd_econv");
    m_rmsd_DT = Json2KeyWord<double>(m_defaults, "rmsd_DT");
    m_wtmtd = Json2KeyWord<bool>(m_defaults, "wtmtd");
    m_rmsd_ref_file = Json2KeyWord<std::string>(m_defaults, "rmsd_ref_file");
    m_rmsd_fix_structure = Json2KeyWord<bool>(m_defaults, "rmsd_fix_structure");
    m_nocolvarfile = Json2KeyWord<bool>(m_defaults, "noCOLVARfile");
    m_nohillsfile = Json2KeyWord<bool>(m_defaults, "noHILSfile");

    m_rmsd_atoms = Json2KeyWord<std::string>(m_defaults, "rmsd_atoms");

    m_writerestart = Json2KeyWord<int>(m_defaults, "writerestart");
    m_respa = Json2KeyWord<int>(m_defaults, "respa");
    m_dipole = Json2KeyWord<bool>(m_defaults, "dipole");
    m_scaling_json = Json2KeyWord<std::string>(m_defaults, "scaling_json");

    m_writeXYZ = Json2KeyWord<bool>(m_defaults, "writeXYZ");
    m_writeinit = Json2KeyWord<bool>(m_defaults, "writeinit");
    m_mtd = Json2KeyWord<bool>(m_defaults, "mtd");
    m_mtd_dT = Json2KeyWord<int>(m_defaults, "mtd_dT");
    if (m_mtd_dT < 0) {
        m_eval_mtd = true;
    } else {
        m_eval_mtd = false;
    }
    m_initfile = Json2KeyWord<std::string>(m_defaults, "initfile");
    m_norestart = Json2KeyWord<bool>(m_defaults, "norestart");
    m_dt2 = m_dT * m_dT;
    m_rm_COM = Json2KeyWord<double>(m_defaults, "rm_COM");
    int rattle = Json2KeyWord<int>(m_defaults, "rattle");

    m_rattle_maxiter = Json2KeyWord<int>(m_defaults, "rattle_maxiter");
    m_rattle_dynamic_tol_iter = Json2KeyWord<int>(m_defaults, "rattle_dynamic_tol_iter");
    m_rattle_max = Json2KeyWord<double>(m_defaults, "rattle_max");
    m_rattle_min = Json2KeyWord<double>(m_defaults, "rattle_min");

    m_rattle_dynamic_tol = Json2KeyWord<bool>(m_defaults, "rattle_dynamic_tol");

    if (rattle == 1) {
        Integrator = [=]() {
            this->Rattle();
        };
        m_rattle_tol_12 = Json2KeyWord<double>(m_defaults, "rattle_tol_12");
        m_rattle_tol_13 = Json2KeyWord<double>(m_defaults, "rattle_tol_13");

        m_rattle_12 = Json2KeyWord<bool>(m_defaults, "rattle_12");
        m_rattle_13 = Json2KeyWord<bool>(m_defaults, "rattle_13");

        // m_coupling = m_dT;
        m_rattle = Json2KeyWord<int>(m_defaults, "rattle");
        std::cout << "Using rattle to constrain bonds!" << std::endl;
        if (m_rattle_12)
            std::cout << "Using rattle to constrain 1,2 distances!" << std::endl;
        if (m_rattle_13)
            std::cout << "Using rattle to constrain 1,3 distances between two bonds!" << std::endl;

    } else {
        Integrator = [=]() {
            this->Verlet();
        };
    }

    if (Json2KeyWord<bool>(m_defaults, "cleanenergy")) {
        Energy = [=]() -> double {
            return this->CleanEnergy();
        };
        std::cout << "Energy Calculator will be set up for each step! Single steps are slower, but more reliable. Recommended for the combination of GFN2 and solvation." << std::endl;
    } else {
        Energy = [=]() -> double {
            return this->FastEnergy();
        };
        std::cout << "Energy Calculator will NOT be set up for each step! Fast energy calculation! This is the default way and should not be changed unless the energy and gradient calculation are unstable (happens with GFN2 and solvation)." << std::endl;
    }

    if (Json2KeyWord<std::string>(m_defaults, "wall") == "spheric") {
        if (Json2KeyWord<std::string>(m_defaults, "wall_type") == "logfermi") {
            m_wall_type = 1;
            WallPotential = [=]() -> double {
                this->m_wall_potential = this->ApplySphericLogFermiWalls();
                return m_wall_potential;
            };
        } else if (Json2KeyWord<std::string>(m_defaults, "wall_type") == "harmonic") {
            m_wall_type = 1;
            WallPotential = [=]() -> double {
                this->m_wall_potential = this->ApplySphericHarmonicWalls();
                return m_wall_potential;
            };
        } else {
            std::cout << "Did not understand wall potential input. Exit now!" << std::endl;
            exit(1);
        }
        std::cout << "Setting up spherical potential" << std::endl;

    } else if (Json2KeyWord<std::string>(m_defaults, "wall") == "rect") {
        if (Json2KeyWord<std::string>(m_defaults, "wall_type") == "logfermi") {
            m_wall_type = 2;
            WallPotential = [=]() -> double {
                this->m_wall_potential = this->ApplyRectLogFermiWalls();
                return m_wall_potential;
            };
        } else if (Json2KeyWord<std::string>(m_defaults, "wall_type") == "harmonic") {
            m_wall_type = 2;
            WallPotential = [=]() -> double {
                this->m_wall_potential = this->ApplyRectHarmonicWalls();
                return m_wall_potential;
            };

        } else {
            std::cout << "Did not understand wall potential input. Exit now!" << std::endl;
            exit(1);
        }
        std::cout << "Setting up rectangular potential" << std::endl;
    } else
        WallPotential = [=]() -> double {
            return 0;
        };
    m_rm_COM_step = static_cast<int>(m_rm_COM / m_dT);
}

// Nach der Klassen-Deklaration unter public: hinzufügen

void SimpleMD::printHelp() const
{
    std::cout << "\n=== SimpleMD: Molecular Dynamics Configuration Parameters ===\n\n"
              << "Parameter           | Default     | Description\n"
              << "-------------------|-------------|----------------------------------------------------\n"
              << "dt                 | " << std::setw(11) << m_defaults.at("dt") << " | Integration time step in femtoseconds (fs)\n"
              << "MaxTime            | " << std::setw(11) << m_defaults.at("MaxTime") << " | Maximum simulation time in fs\n"
              << "T                  | " << std::setw(11) << m_defaults.at("T") << " | Target temperature in Kelvin (K)\n"
              << "thermostat         | " << "  berendson" << " | Thermostat type: berendson, anderson, nosehover, csvr, none\n"
              << "coupling           | " << std::setw(11) << m_defaults.at("coupling") << " | Thermostat coupling time in fs\n"
              << "anderson           | " << std::setw(11) << m_defaults.at("anderson") << " | Anderson thermostat collision probability\n"
              << "chainlength        | " << std::setw(11) << m_defaults.at("chainlength") << " | Chain length for Nosé-Hoover thermostat\n"
              << "charge             | " << std::setw(11) << m_defaults.at("charge") << " | Total charge of the system\n"
              << "Spin               | " << std::setw(11) << m_defaults.at("Spin") << " | Total spin multiplicity\n"
              << "seed               | " << std::setw(11) << m_defaults.at("seed") << " | Random seed (-1: use time, 0: use system size)\n"
              << "threads            | " << std::setw(11) << m_defaults.at("threads") << " | Number of computing threads\n"
              << "\n=== Output Options ===\n\n"
              << "dump               | " << std::setw(11) << m_defaults.at("dump") << " | Save coordinates every N steps\n"
              << "print              | " << std::setw(11) << m_defaults.at("print") << " | Print status every N fs\n"
              << "writeXYZ           | " << std::setw(11) << (m_defaults.at("writeXYZ") ? "true" : "false") << " | Write trajectory to XYZ file\n"
              << "writeinit          | " << std::setw(11) << (m_defaults.at("writeinit") ? "true" : "false") << " | Write initial conditions to file\n"
              << "unique             | " << std::setw(11) << (m_defaults.at("unique") ? "true" : "false") << " | Store only unique conformers\n"
              << "rmsd               | " << std::setw(11) << m_defaults.at("rmsd") << " | RMSD threshold for unique conformers (Å)\n"
              << "writerestart       | " << std::setw(11) << m_defaults.at("writerestart") << " | Write restart file every N steps\n"
              << "norestart          | " << std::setw(11) << (m_defaults.at("norestart") ? "true" : "false") << " | Disable automatic restart\n"
              << "initfile           | " << std::setw(11) << m_defaults.at("initfile") << " | Initial conditions file\n"
              << "dipole             | " << std::setw(11) << (m_defaults.at("dipole") ? "true" : "false") << " | Calculate dipole moments\n"
              << "\n=== System Control ===\n\n"
              << "rm_COM             | " << std::setw(11) << m_defaults.at("rm_COM") << " | Remove translation/rotation every N fs\n"
              << "rmrottrans         | " << std::setw(11) << m_defaults.at("rmrottrans") << " | Remove (0:none, 1:rot, 2:rots, 3:both)\n"
              << "nocenter           | " << std::setw(11) << (m_defaults.at("nocenter") ? "true" : "false") << " | Don't center molecule at origin\n"
              << "COM                | " << std::setw(11) << (m_defaults.at("COM") ? "true" : "false") << " | Use center of mass\n"
              << "opt                | " << std::setw(11) << (m_defaults.at("opt") ? "true" : "false") << " | Optimize structure before MD\n"
              << "hmass              | " << std::setw(11) << m_defaults.at("hmass") << " | Hydrogen mass scaling factor\n"
              << "velo               | " << std::setw(11) << m_defaults.at("velo") << " | Initial velocity scaling factor\n"
              << "rescue             | " << std::setw(11) << (m_defaults.at("rescue") ? "true" : "false") << " | Try to recover from unstable simulations\n"
              << "\n=== Method Options ===\n\n"
              << "method             | " << std::setw(11) << m_defaults.at("method") << " | Energy calculation method (uff, gfn2, etc.)\n"
              << "cleanenergy        | " << "      false" << " | Recreate energy calculator for each step\n"
              << "impuls             | " << std::setw(11) << m_defaults.at("impuls") << " | Temperature threshold for impulse cooling\n"
              << "impuls_scaling     | " << std::setw(11) << m_defaults.at("impuls_scaling") << " | Scaling factor for impulse cooling\n"
              << "\n=== RATTLE Constraints ===\n\n"
              << "rattle             | " << std::setw(11) << (m_defaults.at("rattle") ? "true" : "false") << " | Use RATTLE constraint algorithm\n"
              << "rattle_12          | " << std::setw(11) << (m_defaults.at("rattle_12") ? "true" : "false") << " | Constrain 1-2 bond distances\n"
              << "rattle_13          | " << std::setw(11) << (m_defaults.at("rattle_13") ? "true" : "false") << " | Constrain 1-3 bond angles\n"
              << "rattle_tol_12      | " << std::setw(11) << m_defaults.at("rattle_tol_12") << " | Tolerance for 1-2 constraints\n"
              << "rattle_tol_13      | " << std::setw(11) << m_defaults.at("rattle_tol_13") << " | Tolerance for 1-3 constraints\n"
              << "rattle_maxiter     | " << std::setw(11) << m_defaults.at("rattle_maxiter") << " | Maximum RATTLE iterations\n"
              << "rattle_dynamic_tol | " << "      false" << " | Adjust RATTLE tolerance dynamically\n"
              << "rattle_dynamic_tol_iter | " << std::setw(11) << m_defaults.at("rattle_dynamic_tol_iter") << " | Iterations before tolerance adjustment\n"
              << "rattle_max         | " << std::setw(11) << m_defaults.at("rattle_max") << " | Maximum correction in RATTLE\n"
              << "rattle_min         | " << std::setw(11) << m_defaults.at("rattle_min") << " | Minimum scalar product in RATTLE\n"
              << "\n=== Wall Potentials ===\n\n"
              << "wall               | " << "        none" << " | Wall type (none, spheric, rect)\n"
              << "wall_type          | " << "   logfermi" << " | Wall potential (logfermi, harmonic)\n"
              << "wall_spheric_radius| " << std::setw(11) << m_defaults.at("wall_spheric_radius") << " | Radius for spherical wall (Å)\n"
              << "wall_x/y/z_min/max | " << std::setw(11) << m_defaults.at("wall_x_min") << " | Box boundaries for rectangular wall (Å)\n"
              << "wall_temp          | " << std::setw(11) << m_defaults.at("wall_temp") << " | Wall temperature/strength in K\n"
              << "wall_beta          | " << std::setw(11) << m_defaults.at("wall_beta") << " | Steepness parameter for wall potential\n"
              << "wall_render        | " << std::setw(11) << (m_defaults.at("wall_render") ? "true" : "false") << " | Visualize wall in output\n"
              << "\n=== Metadynamics ===\n\n"
              << "mtd                | " << std::setw(11) << (m_defaults.at("mtd") ? "true" : "false") << " | Enable PLUMED metadynamics\n"
              << "plumed             | " << std::setw(11) << m_defaults.at("plumed") << " | PLUMED input file\n"
              << "mtd_dT             | " << std::setw(11) << m_defaults.at("mtd_dT") << " | Temperature threshold to start MTD\n"
              << "\n=== RMSD-based Metadynamics ===\n\n"
              << "rmsd_mtd           | " << std::setw(11) << (m_defaults.at("rmsd_mtd") ? "true" : "false") << " | Enable RMSD-based metadynamics\n"
              << "k_rmsd             | " << std::setw(11) << m_defaults.at("k_rmsd") << " | Force constant for RMSD bias\n"
              << "alpha_rmsd         | " << std::setw(11) << m_defaults.at("alpha_rmsd") << " | Width parameter for RMSD Gaussians\n"
              << "rmsd_rmsd          | " << std::setw(11) << m_defaults.at("rmsd_rmsd") << " | Height of RMSD Gaussians\n"
              << "mtd_steps          | " << std::setw(11) << m_defaults.at("mtd_steps") << " | Add bias every N steps\n"
              << "max_rmsd_N         | " << std::setw(11) << m_defaults.at("max_rmsd_N") << " | Maximum number of bias structures\n"
              << "rmsd_econv         | " << std::setw(11) << m_defaults.at("rmsd_econv") << " | Energy convergence for bias addition\n"
              << "rmsd_DT            | " << std::setw(11) << m_defaults.at("rmsd_DT") << " | Temperature factor for WT-MTD\n"
              << "wtmtd              | " << std::setw(11) << (m_defaults.at("wtmtd") ? "true" : "false") << " | Use well-tempered metadynamics\n"
              << "rmsd_ref_file      | " << std::setw(11) << m_defaults.at("rmsd_ref_file") << " | File with reference structures\n"
              << "rmsd_fix_structure | " << std::setw(11) << (m_defaults.at("rmsd_fix_structure") ? "true" : "false") << " | Fix reference structures\n"
              << "rmsd_atoms         | " << std::setw(11) << m_defaults.at("rmsd_atoms") << " | Atoms to include in RMSD (-1: all)\n"
              << "noCOLVARfile       | " << std::setw(11) << (m_defaults.at("noCOLVARfile") ? "true" : "false") << " | Disable COLVAR output file\n"
              << "noHILSfile         | " << std::setw(11) << (m_defaults.at("noHILSfile") ? "true" : "false") << " | Disable HILLS output file\n"
              << "\n=== Advanced Options ===\n\n"
              << "MaxTopoDiff        | " << std::setw(11) << m_defaults.at("MaxTopoDiff") << " | Maximum topology difference allowed\n"
              << "respa              | " << std::setw(11) << m_defaults.at("respa") << " | RESPA multiple time-stepping\n"
              << "scaling_json       | " << std::setw(11) << m_defaults.at("scaling_json") << " | JSON file with scaling factors\n"
              << "\n=== Unused Parameters ===\n"
              << "- wall_xl, wall_yl, wall_zl: Only box boundaries are used instead\n"
              << "- printOutput, unique: Duplicated functionality\n"
              << "- dT: Directly uses dt instead\n"
              << "\nExample configuration in JSON:\n"
              << "{\n"
              << "  \"dt\": 1.0,\n"
              << "  \"MaxTime\": 10000,\n"
              << "  \"T\": 300,\n"
              << "  \"thermostat\": \"berendson\",\n"
              << "  \"coupling\": 100,\n"
              << "  \"method\": \"gfn2\",\n"
              << "  \"dump\": 100,\n"
              << "  \"wall\": \"spheric\",\n"
              << "  \"wall_spheric_radius\": 10.0\n"
              << "}\n\n"
              << "Usage Tips:\n"
              << "- For stable dynamics, use timestep ≤ 1.0 fs\n"
              << "- The Berendsen thermostat is efficient but doesn't sample canonical ensemble\n"
              << "- For proper NVT sampling, use CSVR or Nosé-Hoover thermostats\n"
              << "- RATTLE constraints allow larger timesteps for bonds involving H atoms\n"
              << "- Wall potentials prevent molecules from drifting too far\n"
              << "- Metadynamics helps explore conformational space efficiently\n"
              << std::endl;
}

bool SimpleMD::Initialise()
{
    checkHelp();
    m_natoms = m_molecule.AtomCount();
    if(m_natoms == 0)
        return false;

    m_atomtype = std::vector<int>(m_natoms, 0);

    m_eigen_geometry = Eigen::MatrixXd::Zero(m_natoms, 3);
    m_eigen_geometry_old = Eigen::MatrixXd::Zero(m_natoms, 3);
    m_eigen_gradient = Eigen::MatrixXd::Zero(m_natoms, 3);
    m_eigen_gradient_old = Eigen::MatrixXd::Zero(m_natoms, 3);
    m_eigen_velocities = Eigen::MatrixXd::Zero(m_natoms, 3);

    m_eigen_masses = Eigen::VectorXd::Zero(3*m_natoms);
    m_eigen_inv_masses = Eigen::VectorXd::Zero(3*m_natoms);

    static std::random_device rd{};
    static std::mt19937 gen{ rd() };
    if (m_seed == -1) {
        const auto start = std::chrono::high_resolution_clock::now();
        m_seed = std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count();
    } else if (m_seed == 0)
        m_seed = m_natoms * m_T0;
    std::cout << "Random seed is " << m_seed << std::endl;
    gen.seed(m_seed);

    if (m_initfile != "none") {
        json md;
        std::ifstream restart_file(m_initfile);
        try {
            restart_file >> md;
        } catch ([[maybe_unused]] nlohmann::json::type_error& e) {
            throw 404;
        } catch ([[maybe_unused]] nlohmann::json::parse_error& e) {
            throw 404;
        }
        LoadRestartInformation(md);

    } else if (!m_restart)
        LoadRestartInformation();

    if (m_molecule.AtomCount() == 0)
        return false;

    if (!m_restart) {
        std::ofstream result_file;
        result_file.open(Basename() + ".trj.xyz");
        result_file.close();
    }

    if (m_seed == -1) {
        const auto start = std::chrono::high_resolution_clock::now();
        m_seed = std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count();
    } else if (m_seed == 0)
        m_seed = m_T0 * m_natoms;
    std::cout << "Random seed is " << m_seed << std::endl;
    gen.seed(m_seed);


    m_start_fragments = m_molecule.GetFragments();
    m_scaling_vector_linear = std::vector<double>(m_natoms, 1);
    m_scaling_vector_nonlinear = std::vector<double>(m_natoms, 1);
    if (m_scaling_json != "none") {
        json scaling;
        std::ifstream file(m_scaling_json);
        try {
            file >> scaling;
        } catch ([[maybe_unused]] nlohmann::json::type_error& e) {
            throw 404;
        } catch ([[maybe_unused]] nlohmann::json::parse_error& e) {
            throw 404;
        }
        std::string scaling_vector_linear, scaling_vector_nonlinear;
        try {
            scaling_vector_linear = scaling["scaling_vector_linear"];
            scaling_vector_nonlinear = scaling["scaling_vector_nonlinear"];
        } catch ([[maybe_unused]] json::type_error& e) {
        }
        if (!scaling_vector_linear.empty()) {
            m_scaling_vector_linear = Tools::String2DoubleVec(scaling_vector_linear, "|");
        }
        if (!scaling_vector_nonlinear.empty()) {
            m_scaling_vector_nonlinear = Tools::String2DoubleVec(scaling_vector_nonlinear, "|");
        }
    }

    m_molecule.setCharge(0);
    if (!m_nocenter) {
        std::cout << "Move stucture to the origin ... " << std::endl;
        m_molecule.Center(m_COM);
    } else
        std::cout << "Move stucture NOT to the origin ... " << std::endl;



    if (!m_restart) {
        m_eigen_geometry = Eigen::MatrixXd::Zero(m_natoms, 3);
        m_eigen_velocities = Eigen::MatrixXd::Zero(m_natoms, 3);
        m_currentStep = 0;
    }
    /* */
    m_rt_geom_1 = std::vector<double>(3 * m_natoms, 0);
    m_rt_geom_2 = std::vector<double>(3 * m_natoms, 0);
    m_rt_velo = std::vector<double>(3 * m_natoms, 0);
    /* */

    //m_gradient = std::vector<double>(3 * m_natoms, 0);
    m_virial = std::vector<double>(3 * m_natoms, 0);
    m_atom_temp = std::vector<std::vector<double>>(m_natoms);
    if(m_opt)
    {
        json js = CurcumaOptJson;
        js = MergeJson(js, m_defaults);
        js["writeXYZ"] = false;
        js["method"] = m_method;
        /*
        try {
            js["threads"] = m_defaults["threads"].get<int>();
        }
        catch (const nlohmann::detail::type_error& error) {

           }*/
        CurcumaOpt optimise(js, true);
        optimise.addMolecule(&m_molecule);
        optimise.start();
        auto mol = optimise.Molecules();

        auto molecule = ((*mol)[0]);
        m_molecule.setGeometry(molecule.getGeometry());
        m_molecule.appendXYZFile(Basename() + ".opt.xyz");
    }
    double mass = 0;
    for (int i = 0; i < m_natoms; ++i) {
        m_atomtype[i] = m_molecule.Atom(i).first;
        if (!m_restart) {
            Position pos = m_molecule.Atom(i).second;
            // std::cout << pos << std::endl;
            m_eigen_geometry.data()[3 * i + 0] = pos(0);
            m_eigen_geometry.data()[3 * i + 1] = pos(1);
            m_eigen_geometry.data()[3 * i + 2] = pos(2);
            /*
            m_eigen_geometry(i, 0) = pos(0) / 1;
            m_eigen_geometry(i, 1) = pos(1) / 1;
            m_eigen_geometry(i, 2) = pos(2) / 1;
            */
        }
        if (m_atomtype[i] == 1) {
            m_eigen_masses.data()[3 * i + 0] = Elements::AtomicMass[m_atomtype[i]] * m_hmass;
            m_eigen_masses.data()[3 * i + 1] = Elements::AtomicMass[m_atomtype[i]] * m_hmass;
            m_eigen_masses.data()[3 * i + 2] = Elements::AtomicMass[m_atomtype[i]] * m_hmass;

            m_eigen_masses(3*i) = Elements::AtomicMass[m_atomtype[i]] * m_hmass;
            m_eigen_masses(3*i+1) = Elements::AtomicMass[m_atomtype[i]] * m_hmass;
            m_eigen_masses(3*i+2) = Elements::AtomicMass[m_atomtype[i]] * m_hmass;

            m_eigen_inv_masses(3*i) = 1 / (Elements::AtomicMass[m_atomtype[i]] * m_hmass);
            m_eigen_inv_masses(3*i + 1) = 1 / (Elements::AtomicMass[m_atomtype[i]] * m_hmass);
            m_eigen_inv_masses(3*i + 2) = 1 / (Elements::AtomicMass[m_atomtype[i]] * m_hmass);  

            mass += Elements::AtomicMass[m_atomtype[i]] * m_hmass;

            m_eigen_inv_masses.data()[3 * i + 0] = 1 / m_eigen_masses.data()[3 * i + 0];
            m_eigen_inv_masses.data()[3 * i + 1] = 1 / m_eigen_masses.data()[3 * i + 1];
            m_eigen_inv_masses.data()[3 * i + 2] = 1 / m_eigen_masses.data()[3 * i + 2];
        } else {
            m_eigen_masses.data()[3 * i + 0] = Elements::AtomicMass[m_atomtype[i]];
            m_eigen_masses.data()[3 * i + 1] = Elements::AtomicMass[m_atomtype[i]];
            m_eigen_masses.data()[3 * i + 2] = Elements::AtomicMass[m_atomtype[i]];
            mass += Elements::AtomicMass[m_atomtype[i]];

            m_eigen_inv_masses.data()[3 * i + 0] = 1 / m_eigen_masses.data()[3 * i + 0];
            m_eigen_inv_masses.data()[3 * i + 1] = 1 / m_eigen_masses.data()[3 * i + 1];
            m_eigen_inv_masses.data()[3 * i + 2] = 1 / m_eigen_masses.data()[3 * i + 2];

            m_eigen_masses(3*i) = Elements::AtomicMass[m_atomtype[i]];
            m_eigen_masses(3*i+1) = Elements::AtomicMass[m_atomtype[i]];
            m_eigen_masses(3*i+2) = Elements::AtomicMass[m_atomtype[i]];

            m_eigen_inv_masses(3*i) = 1 / (Elements::AtomicMass[m_atomtype[i]]);
            m_eigen_inv_masses(3*i + 1) = 1 / (Elements::AtomicMass[m_atomtype[i]]);
            m_eigen_inv_masses(3*i + 2) = 1 / (Elements::AtomicMass[m_atomtype[i]]);    
        }
    }
    // std::cout << m_eigen_geometry << std::endl;
    // std::cout << m_eigen_masses << std::endl;
    m_molecule.setCharge(m_charge);
    m_molecule.setSpin(m_spin);
    m_interface = new EnergyCalculator(m_method, m_controller["md"], Basename());

    m_interface->setMolecule(m_molecule.getMolInfo());
    // m_interface->setGeometryFile(Basename() + ".xyz"); TODO this does not really work
    // m_interface->setBasename(Basename()); TODO this does not really work
    if (m_writeUnique) {
        json rmsdtraj = RMSDTrajJson;
        rmsdtraj["writeUnique"] = true;
        rmsdtraj["rmsd"] = m_rmsd;
        rmsdtraj["writeRMSD"] = false;
        m_unqiue = new RMSDTraj(rmsdtraj, true);
        m_unqiue->setBaseName(Basename() + ".xyz");
        m_unqiue->Initialise();
    }
    m_dof = 3 * m_natoms;
    InitialiseWalls();
    if (!m_restart) {

        InitConstrainedBonds();
        InitVelocities(m_scale_velo);
        m_xi.resize(m_chain_length, 0.0);
        m_Q.resize(m_chain_length, 100); // Setze eine geeignete Masse für jede Kette
        for (int i = 0; i < m_chain_length; ++i) {
            m_xi[i] = pow(10.0, static_cast<double>(i)) - 1;
            m_Q[i] = pow(10, i) * kb_Eh * m_T0 * m_dof * 100;
            // std::cout << m_xi[i] << "  " << m_Q[i] << std::endl;
        }
        m_eta = 0.0;
    }
    if (m_writeinit) {
        json init = WriteRestartInformation();
        std::ofstream result_file;
        result_file.open(Basename() + ".init.json");
        result_file << init;
        result_file.close();
    }
    /* Initialising MTD RMSD Threads */
    if (m_rmsd_mtd) {
        m_bias_pool = new CxxThreadPool;
        m_bias_pool->setProgressBar(CxxThreadPool::ProgressBarType::None);
        m_bias_pool->setActiveThreadCount(m_threads);
        m_molecule.GetFragments();
        m_rmsd_indicies = m_molecule.FragString2Indicies(m_rmsd_atoms);

        for(auto i : m_rmsd_indicies)
        {
            // std::cout << i << " ";
            m_rmsd_mtd_molecule.addPair(m_molecule.Atom(i));
        }
        m_rmsd_fragment_count = m_rmsd_mtd_molecule.GetFragments().size();

        json config = RMSDJson;
        config["silent"] = true;
        config["reorder"] = false;
        for (int i = 0; i < m_threads; ++i) {
            auto* thread = new BiasThread(m_rmsd_mtd_molecule, config, m_nocolvarfile, m_nohillsfile);
            thread->setDT(m_rmsd_DT);
            thread->setk(m_k_rmsd);
            thread->setalpha(m_alpha_rmsd);
            thread->setEnergyConv(m_rmsd_econv);
            thread->setWTMTD(m_wtmtd);
            m_bias_threads.push_back(thread);
            m_bias_pool->addThread(thread);
        }
        if (m_restart) {
            std::cout << "Reading structure files from " << m_rmsd_ref_file << std::endl;
            for (const auto& i : m_bias_json)
                std::cout << i << std::endl;
            FileIterator file(m_rmsd_ref_file);
            int index = 0;
            while (!file.AtEnd()) {
                Molecule mol = file.Next();
                std::cout << m_bias_json[index] << std::endl;
                int thread_index = index % m_bias_threads.size();
                m_bias_threads[thread_index]->addGeometry(mol.getGeometry(), m_bias_json[index]);
                ++index;
            }
            m_bias_structure_count = index;
        } else {
            if (m_rmsd_ref_file != "none") {
                std::cout << "Reading structure files from " << m_rmsd_ref_file << std::endl;
                int index = 0;

                FileIterator file(m_rmsd_ref_file);
                while (!file.AtEnd()) {
                    Molecule mol = file.Next();
                    int thread_index = index % m_bias_threads.size();
                    m_bias_threads[thread_index]->addGeometry(mol.getGeometry(), 0, 0, index);
                    ++index;
                }
                m_bias_structure_count = index;
            }
        }
    }

    m_initialised = true;
    return true;
}

void SimpleMD::InitConstrainedBonds()
{

    if (m_rattle) {
        auto m = m_molecule.DistanceMatrix();
        m_topo_initial = m.second;
        for (int i = 0; i < m_molecule.AtomCount(); ++i) {
            for (int j = 0; j < i; ++j) {
                if (m.second(i, j)) {
                    if (m_rattle == 2) {
                        if (m_molecule.Atom(i).first != 1 && m_molecule.Atom(j).first != 1)
                            continue;
                    }
                    std::pair<int, int> indicies(i, j);
                    std::pair<double, double> minmax(m_molecule.CalculateDistance(i, j) * m_molecule.CalculateDistance(i, j), m_molecule.CalculateDistance(i, j) * m_molecule.CalculateDistance(i, j));
                    std::pair<std::pair<int, int>, double> bond(indicies, m_molecule.CalculateDistance(i, j) * m_molecule.CalculateDistance(i, j));
                    if (m_rattle_12) {
                        m_bond_constrained.emplace_back(bond);
                        std::cout << "1,2: " << i << " " << j << " " << bond.second << " ";
                    }

                    for (int k = 0; k < j; ++k) {
                        if (m.second(k, j)) {
                            std::pair<int, int> indicies(i, k);
                            std::pair<double, double> minmax(m_molecule.CalculateDistance(i, k) * m_molecule.CalculateDistance(i, k), m_molecule.CalculateDistance(i, k) * m_molecule.CalculateDistance(i, k));
                            std::pair<std::pair<int, int>, double> bond(indicies, m_molecule.CalculateDistance(i, k) * m_molecule.CalculateDistance(i, k));
                            if (m_rattle_13) {
                                m_bond_13_constrained.push_back(std::pair<std::pair<int, int>, double>(bond));
                                std::cout << "1,3: " << i << " " << k << " " << bond.second << " ";
                            }
                        }
                    }
                }
                }
            }
    }

    std::cout << std::endl
              << m_dof << " initial degrees of freedom " << std::endl;
    std::cout << m_bond_constrained.size() << " constrains active" << std::endl;
    // m_dof -= (m_bond_constrained.size() + m_bond_13_constrained.size());

    std::cout << m_dof << " degrees of freedom remaining ..." << std::endl;
}

void SimpleMD::InitVelocities(double scaling)
{
    static std::default_random_engine generator;
    for (size_t i = 0; i < m_natoms; ++i) {
        std::normal_distribution<double> distribution(0.0, std::sqrt(kb_Eh * m_T0 * m_eigen_inv_masses.data()[i]));
        m_eigen_velocities.data()[3 * i + 0] = distribution(generator);
        m_eigen_velocities.data()[3 * i + 1] = distribution(generator);
        m_eigen_velocities.data()[3 * i + 2] = distribution(generator);
    }

    RemoveRotation();
    EKin();
    double coupling = m_coupling;
    m_coupling = m_dT;
    Berendson();
    Berendson();
    EKin();
    m_coupling = coupling;
}

void SimpleMD::InitialiseWalls()
{
    /*
    { "wall_xl", 0},
    { "wall_yl", 0},
    { "wall_zl", 0},
    { "wall_x_min", 0},
    { "wall_x_max", 0},
    { "wall_y_min", 0},
    { "wall_y_max", 0},
    { "wall_z_min", 0},
    { "wall_z_max", 0},*/
    m_wall_spheric_radius = Json2KeyWord<double>(m_defaults, "wall_spheric_radius");
    m_wall_temp = Json2KeyWord<double>(m_defaults, "wall_temp");
    m_wall_beta = Json2KeyWord<double>(m_defaults, "wall_beta");

    m_wall_x_min = Json2KeyWord<double>(m_defaults, "wall_x_min");
    m_wall_x_max = Json2KeyWord<double>(m_defaults, "wall_x_max");
    m_wall_y_min = Json2KeyWord<double>(m_defaults, "wall_y_min");
    m_wall_y_max = Json2KeyWord<double>(m_defaults, "wall_y_max");
    m_wall_z_min = Json2KeyWord<double>(m_defaults, "wall_z_min");
    m_wall_z_max = Json2KeyWord<double>(m_defaults, "wall_z_max");
    // Claude Generated: Intelligent auto-sizing - only when boundaries are undefined or invalid
    double radius = 0;
    bool auto_configured = false;

    // Only auto-configure if bounds are completely undefined (0,0) or clearly invalid (max <= min)
    bool x_needs_config = (m_wall_x_min == 0 && m_wall_x_max == 0) || (m_wall_x_max <= m_wall_x_min);
    bool y_needs_config = (m_wall_y_min == 0 && m_wall_y_max == 0) || (m_wall_y_max <= m_wall_y_min);
    bool z_needs_config = (m_wall_z_min == 0 && m_wall_z_max == 0) || (m_wall_z_max <= m_wall_z_min);
    bool sphere_needs_config = (m_wall_spheric_radius == 0);

    if (x_needs_config || y_needs_config || z_needs_config || sphere_needs_config) {
        auto_configured = true;

        // Find actual molecular extent by analyzing all atom positions
        double min_x = 1e10, max_x = -1e10;
        double min_y = 1e10, max_y = -1e10;
        double min_z = 1e10, max_z = -1e10;
        double max_distance_from_origin = 0;

        for (int i = 0; i < m_natoms; ++i) {
            double x = m_eigen_geometry.data()[3 * i + 0];
            double y = m_eigen_geometry.data()[3 * i + 1];
            double z = m_eigen_geometry.data()[3 * i + 2];

            // Track coordinate ranges
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            min_z = std::min(min_z, z);
            max_z = std::max(max_z, z);

            // Track maximum distance from origin for spherical walls
            double distance = std::sqrt(x * x + y * y + z * z);
            max_distance_from_origin = std::max(max_distance_from_origin, distance);
        }

        // Add safety margins (20% + 5 Å minimum)
        double margin_x = std::max(0.2 * (max_x - min_x), 5.0);
        double margin_y = std::max(0.2 * (max_y - min_y), 5.0);
        double margin_z = std::max(0.2 * (max_z - min_z), 5.0);
        double margin_sphere = std::max(0.2 * max_distance_from_origin, 5.0);

        // Only set boundaries that actually need configuration
        if (x_needs_config) {
            m_wall_x_min = min_x - margin_x;
            m_wall_x_max = max_x + margin_x;
        }
        if (y_needs_config) {
            m_wall_y_min = min_y - margin_y;
            m_wall_y_max = max_y + margin_y;
        }
        if (z_needs_config) {
            m_wall_z_min = min_z - margin_z;
            m_wall_z_max = max_z + margin_z;
        }

        // Set spherical radius with margin
        if (sphere_needs_config) {
            radius = max_distance_from_origin + margin_sphere;
        }
    }

    // Fallback to old box-based method if molecule geometry isn't available
    if (m_natoms == 0 && auto_configured) {
        std::vector<double> box = m_molecule.GetBox();
        if (x_needs_config) {
            m_wall_x_min = -box[0] * 0.75;
            m_wall_x_max = -1 * m_wall_x_min;
            radius = std::max(radius, box[0]);
        }
        if (y_needs_config) {
            m_wall_y_min = -box[1] * 0.75;
            m_wall_y_max = -1 * m_wall_y_min;
            radius = std::max(radius, box[1]);
        }
        if (z_needs_config) {
            m_wall_z_min = -box[2] * 0.75;
            m_wall_z_max = -1 * m_wall_z_min;
            radius = std::max(radius, box[2]);
        }
        radius += 5;
    }

    if (sphere_needs_config && m_wall_spheric_radius < radius) {
        m_wall_spheric_radius = radius;
    }
    if (m_wall_render) {
        std::cout << "render walls" << std::endl;
        if (m_wall_type == 1) {
            Position x0 = Position{ m_wall_spheric_radius, 0, 0 };
            Position x1 = Position{ -m_wall_spheric_radius, 0, 0 };
            Position y0 = Position{ 0, m_wall_spheric_radius, 0 };
            Position y1 = Position{ 0, -m_wall_spheric_radius, 0 };
            Position z0 = Position{ 0, 0, m_wall_spheric_radius };
            Position z1 = Position{ 0, 0, -m_wall_spheric_radius };
            m_molecule.addBorderPoint(x0);
            m_molecule.addBorderPoint(x1);
            m_molecule.addBorderPoint(y0);
            m_molecule.addBorderPoint(y1);
            m_molecule.addBorderPoint(z0);
            m_molecule.addBorderPoint(z1);

            double intermedia = 1 / sqrt(2.0) * m_wall_spheric_radius;
            x0 = Position{ intermedia, intermedia, 0 };
            y0 = Position{ 0, intermedia, intermedia };
            z0 = Position{ intermedia, 0, intermedia };

            m_molecule.addBorderPoint(x0);
            m_molecule.addBorderPoint(y0);
            m_molecule.addBorderPoint(z0);
            x0 = Position{ -intermedia, -intermedia, 0 };
            y0 = Position{ 0, -intermedia, -intermedia };
            z0 = Position{ -intermedia, 0, -intermedia };
            m_molecule.addBorderPoint(x0);
            m_molecule.addBorderPoint(y0);
            m_molecule.addBorderPoint(z0);
            x0 = Position{ -intermedia, intermedia, 0 };
            y0 = Position{ 0, -intermedia, intermedia };
            z0 = Position{ -intermedia, 0, intermedia };
            m_molecule.addBorderPoint(x0);
            m_molecule.addBorderPoint(y0);
            m_molecule.addBorderPoint(z0);
            x0 = Position{ intermedia, -intermedia, 0 };
            y0 = Position{ 0, intermedia, -intermedia };
            z0 = Position{ intermedia, 0, -intermedia };
            m_molecule.addBorderPoint(x0);
            m_molecule.addBorderPoint(y0);
            m_molecule.addBorderPoint(z0);
            intermedia = 1 / sqrt(3.0) * m_wall_spheric_radius;

            x0 = Position{ intermedia, intermedia, intermedia };
            m_molecule.addBorderPoint(x0);
            x0 = Position{ -intermedia, intermedia, intermedia };
            m_molecule.addBorderPoint(x0);
            x0 = Position{ intermedia, -intermedia, intermedia };
            m_molecule.addBorderPoint(x0);
            x0 = Position{ intermedia, intermedia, -intermedia };
            m_molecule.addBorderPoint(x0);
            x0 = Position{ -intermedia, intermedia, -intermedia };
            m_molecule.addBorderPoint(x0);
            x0 = Position{ intermedia, -intermedia, -intermedia };
            m_molecule.addBorderPoint(x0);
            x0 = Position{ -intermedia, -intermedia, intermedia };
            m_molecule.addBorderPoint(x0);
            x0 = Position{ -intermedia, -intermedia, -intermedia };
            m_molecule.addBorderPoint(x0);
        } else if (m_wall_type == 2) {
            Position x0 = Position{ m_wall_x_min, 0, 0 };
            Position x1 = Position{ m_wall_x_max, 0, 0 };
            Position y0 = Position{ 0, m_wall_y_min, 0 };
            Position y1 = Position{ 0, m_wall_y_max, 0 };
            Position z0 = Position{ 0, 0, m_wall_z_min };
            Position z1 = Position{ 0, 0, m_wall_z_max };

            m_molecule.addBorderPoint(x0);
            m_molecule.addBorderPoint(x1);
            m_molecule.addBorderPoint(y0);
            m_molecule.addBorderPoint(y1);
            m_molecule.addBorderPoint(z0);
            m_molecule.addBorderPoint(z1);

            x0 = Position{ m_wall_x_min, m_wall_y_min, 0 };
            x1 = Position{ m_wall_x_max, m_wall_y_max, 0 };
            y0 = Position{ m_wall_x_min, 0, m_wall_z_min };
            y1 = Position{ m_wall_x_max, 0, m_wall_z_min };
            z0 = Position{ 0, m_wall_y_min, m_wall_z_min };
            z1 = Position{ 0, m_wall_y_max, m_wall_z_max };

            m_molecule.addBorderPoint(x0);
            m_molecule.addBorderPoint(x1);
            m_molecule.addBorderPoint(y0);
            m_molecule.addBorderPoint(y1);
            m_molecule.addBorderPoint(z0);
            m_molecule.addBorderPoint(z1);

            x0 = Position{ m_wall_x_min, m_wall_y_min, m_wall_z_min };
            m_molecule.addBorderPoint(x0);
        }
    }
    // Claude Generated: Store wall configuration info for PrintStatus() display
    m_wall_geometry = Json2KeyWord<std::string>(m_defaults, "wall");
    m_wall_potential_type = Json2KeyWord<std::string>(m_defaults, "wall_type");
    m_wall_auto_configured = auto_configured;

    // Calculate molecular density within wall boundaries
    if (m_wall_geometry == "rect" && (m_wall_x_max > m_wall_x_min) && (m_wall_y_max > m_wall_y_min) && (m_wall_z_max > m_wall_z_min)) {
        double volume = (m_wall_x_max - m_wall_x_min) * (m_wall_y_max - m_wall_y_min) * (m_wall_z_max - m_wall_z_min);
        m_molecular_density = 1.0 / volume; // molecules per Å³
    } else if (m_wall_geometry == "spheric" && m_wall_spheric_radius > 0) {
        double volume = (4.0 / 3.0) * pi * m_wall_spheric_radius * m_wall_spheric_radius * m_wall_spheric_radius;
        m_molecular_density = 1.0 / volume; // molecules per Å³
    }
    // Claude Generated: Wall configuration summary in PrintStatus() - show once every 10000 steps
    if (m_wall_geometry != "none" && m_wall_geometry != "") {
        std::cout << "\n--- Wall Setup ---\n";
        std::cout << "Geometry: " << m_wall_geometry << " | Potential: " << m_wall_potential_type;
        if (m_wall_auto_configured)
            std::cout << " (auto-sized)";
        std::cout << "\n";

        if (m_wall_geometry == "spheric") {
            std::cout << "Radius: " << m_wall_spheric_radius << " Å";
            if (m_molecular_density > 0) {
                std::cout << " | Density: " << m_molecular_density * 1e3 << " molecules/nm³";
            }
        } else if (m_wall_geometry == "rect") {
            double volume = (m_wall_x_max - m_wall_x_min) * (m_wall_y_max - m_wall_y_min) * (m_wall_z_max - m_wall_z_min);
            std::cout << "Bounds: [" << m_wall_x_min << "," << m_wall_x_max << "] x ["
                      << m_wall_y_min << "," << m_wall_y_max << "] x ["
                      << m_wall_z_min << "," << m_wall_z_max << "] Å";
            std::cout << " | Vol: " << volume << " Å³";
            if (m_molecular_density > 0) {
                std::cout << " | Density: " << m_molecular_density * 1e3 << " molecules/nm³";
            }
        }

        if (m_wall_violation_count > 0) {
            std::cout << " | Violations: " << m_wall_violation_count << "/" << m_natoms << " atoms";
        }
        std::cout << "\n---------------------------------\n";
    }
}

nlohmann::json SimpleMD::WriteRestartInformation()
{
    nlohmann::json restart;
    restart["method"] = m_method;
    restart["thermostat"] = m_thermostat;
    restart["dT"] = m_dT;
    restart["MaxTime"] = m_maxtime;
    restart["T"] = m_T0;
    restart["currentStep"] = m_currentStep;
    restart["seed"] = m_seed;
    restart["velocities"] = Tools::Geometry2String(m_eigen_velocities);
    restart["geometry"] = Tools::Geometry2String(m_eigen_geometry);
    restart["gradient"] = Tools::Geometry2String(m_eigen_gradient);
    restart["rmrottrans"] = m_rmrottrans;
    restart["nocenter"] = m_nocenter;
    restart["COM"] = m_COM;
    restart["average_T"] = m_aver_Temp;
    restart["average_Epot"] = m_aver_Epot;
    restart["average_Ekin"] = m_aver_Ekin;
    restart["average_Etot"] = m_aver_Etot;
    restart["average_Virial"] = m_average_virial_correction;
    restart["average_Wall"] = m_average_wall_potential;

    restart["rattle"] = m_rattle;
    restart["rattle_maxiter"] = m_rattle_maxiter;
    // restart["rattle_dynamic_tol"] = m_rattle_toler;
    restart["rattle_dynamic_tol_iter"] = m_rattle_dynamic_tol_iter;

    restart["coupling"] = m_coupling;
    restart["MaxTopoDiff"] = m_max_top_diff;
    restart["impuls"] = m_impuls;
    restart["impuls_scaling"] = m_impuls_scaling;
    restart["respa"] = m_respa;
    restart["rm_COM"] = m_rm_COM;
    restart["mtd"] = m_mtd;
    restart["rmsd_mtd"] = m_rmsd_mtd;
    restart["chainlength"] = m_chain_length;
    restart["eta"] = m_eta;
    restart["xi"] = Tools::DoubleVector2String(m_xi);
    restart["Q"] = Tools::DoubleVector2String(m_Q);

    if (m_rmsd_mtd) {
        restart["k_rmsd"] = m_k_rmsd;
        restart["alpha_rmsd"] = m_alpha_rmsd;
        restart["mtd_steps"] = m_mtd_steps;
        restart["rmsd_econv"] = m_rmsd_econv;
        restart["wtmtd"] = m_wtmtd;
        restart["rmsd_DT"] = m_rmsd_DT;
        restart["rmsd_ref_file"] = Basename() + ".mtd.xyz";
        restart["counter"] = m_bias_structure_count;
        restart["rmsd_atoms"] = m_rmsd_atoms;
        std::vector<json> bias(m_bias_structure_count);
        for (const auto & m_bias_thread : m_bias_threads) {
            for (const auto& stored_bias : m_bias_thread->getBias()) {
                bias[stored_bias["index"]] = stored_bias;
            }
        }
        json bias_restart;
        for (int i = 0; i < bias.size(); ++i) {
            bias_restart[i] = bias[i];
        }
        restart["bias"] = bias_restart;
    }
    if (m_rattle) {
        json constrains;
        if (m_rattle_12) {
            json constrains_12;
            for (int i = 0; i < m_bond_constrained.size(); ++i) {
                json element;
                element["i"] = m_bond_constrained[i].first.first;
                element["j"] = m_bond_constrained[i].first.second;
                element["d"] = m_bond_constrained[i].second;

                constrains_12[i] = element;
            }
            constrains["constrain_12"] = true;
            constrains["num_constrain_12"] = m_bond_constrained.size();
            constrains["constrains_12"] = constrains_12;
        }

        if (m_rattle_13) {
            json constrains_13;
            for (int i = 0; i < m_bond_13_constrained.size(); ++i) {
                json element;
                element["i"] = m_bond_13_constrained[i].first.first;
                element["j"] = m_bond_13_constrained[i].first.second;
                element["d"] = m_bond_13_constrained[i].second;

                constrains_13[i] = element;
            }
            constrains["constrain_13"] = true;
            constrains["num_constrain_13"] = m_bond_13_constrained.size();
            constrains["constrains_13"] = constrains_13;
        }
        restart["constrains"] = constrains;
    }
    return restart;
};

bool SimpleMD::LoadRestartInformation()
{
    if (!Restart())
        return false;
    StringList files = RestartFiles();
    int error = 0;
    for (const auto& f : files) {
        std::ifstream file(f);
        json restart;
        try {
            file >> restart;
        } catch ([[maybe_unused]] json::type_error& e) {
            error++;
            continue;
        } catch ([[maybe_unused]] json::parse_error& e) {
            error++;
            continue;
        }

        json md;
        try {
            md = restart[MethodName()[0]];
        } catch ([[maybe_unused]] json::type_error& e) {
            error++;
            continue;
        }
        return LoadRestartInformation(md);
    }
    return true;
};

bool SimpleMD::LoadRestartInformation(const json& state)
{
    std::string geometry, velocities, constrains, xi, Q;

    try {
        m_method = state["method"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_dT = state["dT"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }
    try {
        m_maxtime = state["MaxTime"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }
    try {
        m_rmrottrans = state["rmrottrans"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }
    try {
        m_nocenter = state["nocenter"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }
    try {
        m_COM = state["COM"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }
    try {
        m_T0 = state["T"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }
    try {
        m_currentStep = state["currentStep"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_aver_Epot = state["average_Epot"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_aver_Ekin = state["average_Ekin"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_aver_Etot = state["average_Etot"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }
    try {
        m_aver_Temp = state["average_T"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_average_virial_correction = state["average_Virial"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_average_wall_potential = state["average_Wall"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_coupling = state["coupling"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_respa = state["respa"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_eta = state["eta"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_thermostat = state["thermostat"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        geometry = state["geometry"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        velocities = state["velocities"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        xi = state["xi"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }
    try {
        Q = state["Q"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_mtd = state["mtd"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    try {
        m_rattle = state["rattle"];
    } catch ([[maybe_unused]] json::type_error& e) {
    }
    if(m_rattle)
    {
        try {
            m_rattle_tol_12 = state["rattle_tol_12"];
        } catch ([[maybe_unused]] json::type_error& e) {
        }

        try {
            m_rattle_tol_13 = state["rattle_tol_13"];
        } catch (json::type_error& e) {
        }
        try {
            m_rattle_maxiter = state["rattle_maxiter"];
        } catch ([[maybe_unused]] json::type_error& e) {
        }

        try {
            m_rattle_dynamic_tol = state["rattle_dynamic_tol"];
        } catch ([[maybe_unused]] json::type_error& e) {
        }

        try {
            m_rattle_dynamic_tol_iter = state["rattle_dynamic_tol_iter"];
        } catch ([[maybe_unused]] json::type_error& e) {
        }
    }
    try {
        m_seed = state["seed"];
    } catch (json::type_error& e) {
    }
    try {
        m_rmsd_mtd = state["rmsd_mtd"];
        if (m_rmsd_mtd) {
            m_k_rmsd = state["k_rmsd"];
            m_alpha_rmsd = state["alpha_rmsd"];
            m_mtd_steps = state["mtd_steps"];
            m_rmsd_econv = state["rmsd_econv"];
            m_wtmtd = state["wtmtd"];
            m_rmsd_DT = state["rmsd_DT"];
            m_rmsd_ref_file = state["rmsd_ref_file"];
            m_bias_json = state["bias"];
        }
    } catch ([[maybe_unused]] json::type_error& e) {
    }

    if (!geometry.empty()) {
        Tools::String2Geometry(m_eigen_geometry, geometry);

    }
    if (!velocities.empty()) {
        Tools::String2Geometry(m_eigen_velocities, velocities); 
    }

    if (!xi.empty()) {
        m_xi = Tools::String2DoubleVec(xi, "|");
    }

    if (!Q.empty()) {
        m_Q = Tools::String2DoubleVec(Q, "|");
    }

    try {
        if (state.contains("constrains")) {
            const json& constrains = state["constrains"];

            if (constrains.contains("constrain_12") && constrains["constrain_12"].get<bool>()) {
                m_bond_constrained.clear();
                int num_constrain_12 = constrains["num_constrain_12"];
                const json& constrains_12 = constrains["constrains_12"];

                for (int i = 0; i < num_constrain_12; ++i) {
                    int i_index = constrains_12[i]["i"].get<int>();
                    int j_index = constrains_12[i]["j"].get<int>();
                    double distance = constrains_12[i]["d"].get<double>();
                    m_bond_constrained.emplace_back(std::make_pair(i_index, j_index), distance);
                    std::cout << "1,2: " << i_index << " " << j_index << " " << distance << " ";
                }
            }

            if (constrains.contains("constrain_13") && constrains["constrain_13"].get<bool>()) {
                m_bond_13_constrained.clear();
                int num_constrain_13 = constrains["num_constrain_13"];
                const json& constrains_13 = constrains["constrains_13"];

                for (int i = 0; i < num_constrain_13; ++i) {
                    int i_index = constrains_13[i]["i"].get<int>();
                    int j_index = constrains_13[i]["j"].get<int>();
                    double distance = constrains_13[i]["d"].get<double>();
                    m_bond_13_constrained.emplace_back(std::make_pair(i_index, j_index), distance);
                    std::cout << "1,3: " << i_index << " " << j_index << " " << distance << " ";
                }
            }
        }
    } catch ([[maybe_unused]] json::type_error& e) {
        std::cerr << e.what() << '\n';
    }

    m_restart = !geometry.empty() && !velocities.empty();

    return true;
}

void SimpleMD::start()
{
    if (m_initialised == false)
        return;
    bool aborted = false;
    auto unix_timestamp = std::chrono::seconds(std::time(nullptr));
    m_unix_started = std::chrono::milliseconds(unix_timestamp).count();

    std::vector<json> states;

    if (m_thermostat == "csvr") {
        fmt::print(fg(fmt::color::green) | fmt::emphasis::bold, "\nUsing Canonical sampling through velocity rescaling (CSVR) Thermostat\nJ. Chem. Phys. 126, 014101 (2007) - DOI: 10.1063/1.2408420\n\n");
        ThermostatFunction = [this] { CSVR(); };
    } else if (m_thermostat == "berendson") {
        fmt::print(fg(fmt::color::green) | fmt::emphasis::bold, "\nUsing Berendson Thermostat\nJ. Chem. Phys. 81, 3684 (1984) - DOI: 10.1063/1.448118\n\n");
        ThermostatFunction = [this] { Berendson(); };
    } else if (m_thermostat == "anderson") {
        fmt::print(fg(fmt::color::green) | fmt::emphasis::bold, "\nUsing Anderson Thermostat\n ... \n\n");
        ThermostatFunction = [this] { Anderson(); };
    } else if (m_thermostat == "nosehover") {
        fmt::print(fg(fmt::color::green) | fmt::emphasis::bold, "\nUsing Nosé-Hoover-Chain Thermostat\n ... \n\n");
        ThermostatFunction = [this] { NoseHover(); };
    } else {
        ThermostatFunction = [this] { None(); };
        std::cout << "No Thermostat applied\n"
                  << std::endl;
    }

    m_Epot = Energy();
    EKin();
    m_Etot = m_Epot + m_Ekin;
    AverageQuantities();
    int m_step = 0;
    WriteGeometry();
#ifdef USE_Plumed
    if (m_mtd) {
        m_plumedmain = plumed_create();
        int real_precision = 8;
        double energyUnits = 2625.5;
        double lengthUnits = 10;
        double timeUnits = 1e-3;
        double massUnits = 1;
        double chargeUnit = 1;
        int restart = m_restart;
        plumed_cmd(m_plumedmain, "setRealPrecision", &real_precision); // Pass a pointer to an integer containing the size of a real number (4 or 8)
        plumed_cmd(m_plumedmain, "setMDEnergyUnits", &energyUnits); // Pass a pointer to the conversion factor between the energy unit used in your code and kJ mol-1
        plumed_cmd(m_plumedmain, "setMDLengthUnits", &lengthUnits); // Pass a pointer to the conversion factor between the length unit used in your code and nm
        plumed_cmd(m_plumedmain, "setMDTimeUnits", &timeUnits); // Pass a pointer to the conversion factor between the time unit used in your code and ps
        plumed_cmd(m_plumedmain, "setNatoms", &m_natoms); // Pass a pointer to the number of atoms in the system to plumed
        plumed_cmd(m_plumedmain, "setMDEngine", "curcuma");
        plumed_cmd(m_plumedmain, "setMDMassUnits", &massUnits); // Pass a pointer to the conversion factor between the mass unit used in your code and amu
        plumed_cmd(m_plumedmain, "setMDChargeUnits", &chargeUnit);
        plumed_cmd(m_plumedmain, "setTimestep", &m_dT); // Pass a pointer to the molecular dynamics timestep to plumed                       // Pass the name of your md engine to plumed (now it is just a label)
        plumed_cmd(m_plumedmain, "setKbT", &kb_Eh);
        plumed_cmd(m_plumedmain, "setLogFile", "plumed_log.out"); // Pass the file  on which to write out the plumed log (to be created)
        plumed_cmd(m_plumedmain, "setRestart", &restart); // Pointer to an integer saying if we are restarting (zero means no, one means yes)
        plumed_cmd(m_plumedmain, "init", NULL);
        plumed_cmd(m_plumedmain, "read", m_plumed.c_str());
        plumed_cmd(m_plumedmain, "setStep", &m_step);
        plumed_cmd(m_plumedmain, "setPositions", &m_eigen_geometry.data()[0]);
        plumed_cmd(m_plumedmain, "setEnergy", &m_Epot);
        plumed_cmd(m_plumedmain, "setForces", &m_eigen_gradient.data()[0]);
        plumed_cmd(m_plumedmain, "setVirial", &m_virial[0]);
        plumed_cmd(m_plumedmain, "setMasses", &m_eigen_masses.data()[0]);
        plumed_cmd(m_plumedmain, "prepareCalc", NULL);
        plumed_cmd(m_plumedmain, "performCalc", NULL);
    }
#endif
    std::vector<double> charge(0, m_natoms);

#ifdef GCC
    //         std::cout << fmt::format("{0: ^{0}} {1: ^{1}} {2: ^{2}} {3: ^{3}} {4: ^{4}}\n", "Step", "Epot", "Ekin", "Etot", "T");
    // std::cout << fmt::format("{1: ^{0}} {1: ^{1}} {1: ^{2}} {1: ^{3}} {1: ^{4}}\n", "", "Eh", "Eh", "Eh", "K");
#else
    std::cout << "Step"
              << "\t"
              << "Epot"
              << "\t"
              << "Ekin"
              << "\t"
              << "Etot"
              << "\t"
              << "T" << std::endl;
    std::cout << "  "
              << "\t"
              << "Eh"
              << "\t"
              << "Eh"
              << "\t"
              << "Eh"
              << "\t"
              << "T" << std::endl;
#endif
    if (m_rmsd_mtd) {
        std::cout << "k\t" << m_k_rmsd << std::endl;
        std::cout << "alpha\t" << m_alpha_rmsd << std::endl;
        std::cout << "steps\t" << m_mtd_steps << std::endl;
        std::cout << "Ethresh\t" << m_rmsd_econv << std::endl;
        if (m_wtmtd)
            std::cout << "Well Tempered\tOn (" << m_rmsd_DT << ")" << std::endl;
        else
            std::cout << "Well Tempered\tOff" << std::endl;
    }
    PrintStatus();

    /* Start MD Lopp here */
    while (m_currentStep < m_maxtime) {
        auto step0 = std::chrono::system_clock::now();

        if (CheckStop() == true) {
            TriggerWriteRestart();
            aborted = true;
#ifdef USE_Plumed
            if (m_mtd) {
                plumed_finalize(m_plumedmain); // Call the plumed destructor
            }
#endif

            break;
        }

        if (m_rm_COM_step > 0 && m_step % m_rm_COM_step == 0) {
            // std::cout << "Removing COM motion." << std::endl;
            if (m_rmrottrans == 1)
                RemoveRotation();
            else if (m_rmrottrans == 2)
                RemoveRotations();
            else if (m_rmrottrans == 3) {
                RemoveRotations();
                RemoveRotation();
            }
        }

        Integrator();
        AverageQuantities();

        if (m_mtd) {
            if (!m_eval_mtd) {
                if (std::abs(m_T0 - m_aver_Temp) < m_mtd_dT && m_step > 10) {
                    m_eval_mtd = true;
                    std::cout << "Starting with MetaDynamics ..." << std::endl;
                }
            }
        }

        /////////// Dipole
        if (m_dipole && m_method == "gfn2") {
            //linear Dipoles
            auto curr_dipoles_lin = m_molecule.CalculateDipoleMoments(m_scaling_vector_linear, m_start_fragments);
            std::ofstream file;
            file.open(Basename() + "_dipole_linear.out", std::ios_base::app);
            Position d = {0,0,0};
            for (const auto& dipole_lin : curr_dipoles_lin) {
                d += dipole_lin;
                file << dipole_lin[0] << " " << dipole_lin[1] << " " << dipole_lin[2] << " " << dipole_lin.norm() << ", ";
            }
            file << d[0] << " " << d[1] << " " << d[2] << ", " << m_molecule.getDipole()[0] << " " << m_molecule.getDipole()[1] << " " << m_molecule.getDipole()[2] << std::endl;
            file.close();
            //nonlinear Dipoles
            auto curr_dipoles_nlin = m_molecule.CalculateDipoleMoments(m_scaling_vector_nonlinear, m_start_fragments);
            std::ofstream file2;
            file2.open(Basename() + "_dipole_nonlinear.out", std::ios_base::app);
            Position sum = {0,0,0};
            for (const auto& dipole_nlin : curr_dipoles_nlin) {
                sum += dipole_nlin;
                file2 << dipole_nlin[0] << " " << dipole_nlin[1] << " " << dipole_nlin[2] << " " << dipole_nlin.norm() <<", ";
            }
            file2 << sum[0] << " " << sum[1] << " " << sum[2] << ", " << m_molecule.getDipole()[0] << " " << m_molecule.getDipole()[1] << " " << m_molecule.getDipole()[2] << std::endl;
            file2.close();
        }
        //////////// Dipole


        if (m_step % m_dump == 0) {
            if (bool write = WriteGeometry()) {
                states.push_back(WriteRestartInformation());
                m_current_rescue = 0;
            } else if (!write && m_rescue && states.size() > (1 - m_current_rescue)) {
                std::cout << "Molecule exploded, resetting to previous state ..." << std::endl;
                LoadRestartInformation(states[states.size() - 1 - m_current_rescue]);
                Geometry geometry = m_molecule.getGeometry();
                for (int i = 0; i < m_natoms; ++i) {
                    geometry(i, 0) = m_eigen_geometry.data()[3 * i + 0];
                    geometry(i, 1) = m_eigen_geometry.data()[3 * i + 1];
                    geometry(i, 2) = m_eigen_geometry.data()[3 * i + 2];
                }
                m_molecule.setGeometry(geometry);
                m_molecule.GetFragments();
                InitVelocities(-1);
                Energy();
                EKin();
                m_Etot = m_Epot + m_Ekin;
                m_current_rescue++;
                PrintStatus();
                m_time_step = 0;
            }
        }

        if (m_unstable || m_interface->Error() || m_interface->HasNan()) {
            PrintStatus();
            fmt::print(fg(fmt::color::salmon) | fmt::emphasis::bold, "Simulation got unstable, exiting!\n");

            std::ofstream restart_file("unstable_curcuma.json");
            nlohmann::json restart;
            restart[MethodName()[0]] = WriteRestartInformation();
            restart_file << restart << std::endl;

            m_time_step = 0;
            aborted = true;

#ifdef USE_Plumed
            if (m_mtd) {
                plumed_finalize(m_plumedmain); // Call the plumed destructor
            }
#endif
            return;
        }

        if (m_writerestart > -1 && m_step % m_writerestart == 0) {
            std::ofstream restart_file("curcuma_step_" + std::to_string(static_cast<int>(m_step * m_dT)) + ".json");
            json restart;
            restart[MethodName()[0]] = WriteRestartInformation();
            restart_file << restart << std::endl;
        }
        if ((m_step && static_cast<int>(m_step * m_dT) % m_print == 0)) {
            m_Etot = m_Epot + m_Ekin;
            PrintStatus();
            m_time_step = 0;
        }
        if (m_rattle && m_rattle_dynamic_tol) {
            m_aver_rattle_Temp += m_T;
            m_rattle_counter++;
            if (m_rattle_counter == m_rattle_dynamic_tol_iter)
                AdjustRattleTolerance();
        }
        if (m_impuls > m_T) {
            InitVelocities(m_scale_velo * m_impuls_scaling);
            EKin();
            // PrintStatus();
            m_time_step = 0;
        }

        if (m_current_rescue >= m_max_rescue) {
            fmt::print(fg(fmt::color::salmon) | fmt::emphasis::bold, "Nothing really helps");
            break;
        }
        m_step++;
        m_currentStep += m_dT;
        m_time_step += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - step0).count();
    } //MD Loop end here
    PrintStatus();
    if (m_thermostat == "csvr")
        std::cout << "Exchange with heat bath " << m_Ekin_exchange << "Eh" << std::endl;
    if (m_dipole) {

        double dipole = 0.0;
        //std::cout << dipole*2.5418 << " average dipole in Debye and " << dipole*2.5418*3.3356e-30 << " Cm" << std::endl;

        std::cout << "Calculated averaged dipole moment " << m_aver_dipol_linear * 2.5418 << " Debye and " << m_aver_dipol_linear * 2.5418 * 3.3356 << " Cm [e-30]" << std::endl;
    }

#ifdef USE_Plumed
    if (m_mtd) {
        plumed_finalize(m_plumedmain); // Call the plumed destructor
    }
#endif
    if (m_rmsd_mtd) {
        std::cout << "Sum of Energy of COLVARs:" << std::endl;
        // std::vector<BiasStructure> biased_structures;

        for (int i = 0; i < m_bias_threads.size(); ++i) {
            auto structures = m_bias_threads[i]->getBiasStructure();
            // biased_structures.push_back(structures);
            for (int j = 0; j < structures.size(); ++j) {
                std::cout << structures[j].rmsd_reference << "\t" << structures[j].energy << "\t" << structures[j].counter / static_cast<double>(m_colvar_incr) * 100 << std::endl;

                m_rmsd_mtd_molecule.setGeometry(structures[j].geometry);
                m_rmsd_mtd_molecule.setEnergy(structures[j].energy);
                m_rmsd_mtd_molecule.setName(std::to_string(structures[j].index) + " " + std::to_string(structures[j].rmsd_reference));
                if (i == j && i == 0)
                    m_rmsd_mtd_molecule.writeXYZFile(Basename() + ".mtd.xyz");
                else
                    m_rmsd_mtd_molecule.appendXYZFile(Basename() + ".mtd.xyz");
            }
        }
    }
    std::ofstream restart_file("curcuma_final.json");
    nlohmann::json restart;
    restart[MethodName()[0]] = WriteRestartInformation();
    restart_file << restart << std::endl;
    if (aborted == false)
        std::remove("curcuma_restart.json");
}

void SimpleMD::AdjustRattleTolerance()
{
    m_aver_rattle_Temp /= static_cast<double>(m_rattle_counter);

    // std::pair<double, double> pair(m_rattle_tolerance, m_aver_Temp);

    if (m_aver_rattle_Temp > m_T0)
        m_rattle_tol_12 -= 0.01;
    else if (m_aver_rattle_Temp < m_T0)
        m_rattle_tol_12 += 0.01;
    std::cout << m_rattle_counter << " " << m_aver_rattle_Temp << " " << m_rattle_tol_12 << std::endl;
    m_rattle_tol_12 = std::abs(m_rattle_tol_12);
    m_rattle_counter = 0;
    m_aver_rattle_Temp = 0;
}

void SimpleMD::Verlet()
{
    double ekin = 0;
    //std::cout << m_eigen_inv_masses << std::endl;
    for (int i = 0; i < m_natoms; ++i) {
        m_eigen_geometry.data()[3 * i + 0] = m_eigen_geometry.data()[3 * i + 0] + m_dT * m_eigen_velocities.data()[3 * i + 0] - 0.5 * m_eigen_gradient.data()[3 * i + 0] * m_eigen_inv_masses.data()[3 * i + 0] * m_dt2;
        m_eigen_geometry.data()[3 * i + 1] = m_eigen_geometry.data()[3 * i + 1] + m_dT * m_eigen_velocities.data()[3 * i + 1] - 0.5 * m_eigen_gradient.data()[3 * i + 1] * m_eigen_inv_masses.data()[3 * i + 1] * m_dt2;
        m_eigen_geometry.data()[3 * i + 2] = m_eigen_geometry.data()[3 * i + 2] + m_dT * m_eigen_velocities.data()[3 * i + 2] - 0.5 * m_eigen_gradient.data()[3 * i + 2] * m_eigen_inv_masses.data()[3 * i + 2] * m_dt2;

        m_eigen_velocities.data()[3 * i + 0] = m_eigen_velocities.data()[3 * i + 0] - 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 0] * m_eigen_inv_masses.data()[3 * i + 0];
        m_eigen_velocities.data()[3 * i + 1] = m_eigen_velocities.data()[3 * i + 1] - 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 1] * m_eigen_inv_masses.data()[3 * i + 1];
        m_eigen_velocities.data()[3 * i + 2] = m_eigen_velocities.data()[3 * i + 2] - 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 2] * m_eigen_inv_masses.data()[3 * i + 2];
        ekin += m_eigen_masses.data()[i] * (m_eigen_velocities.data()[3 * i] * m_eigen_velocities.data()[3 * i] + m_eigen_velocities.data()[3 * i + 1] * m_eigen_velocities.data()[3 * i + 1] + m_eigen_velocities.data()[3 * i + 2] * m_eigen_velocities.data()[3 * i + 2]);
    }
    ekin *= 0.5;
    m_T = 2.0 * ekin / (kb_Eh * m_dof);
    m_Ekin = ekin;
    ThermostatFunction();
    m_Epot = Energy();
    if (m_rmsd_mtd) {
        if (m_step % m_mtd_steps == 0) {
            ApplyRMSDMTD();
        }
    }
#ifdef USE_Plumed
    if (m_mtd) {
        plumed_cmd(m_plumedmain, "setStep", &m_step);

        plumed_cmd(m_plumedmain, "setPositions", &m_eigen_geometry.data()[0]);

        plumed_cmd(m_plumedmain, "setEnergy", &m_Epot);
        plumed_cmd(m_plumedmain, "setForces", &m_eigen_gradient.data()[0]);
        plumed_cmd(m_plumedmain, "setVirial", &m_virial[0]);

        plumed_cmd(m_plumedmain, "setMasses", &m_eigen_masses.data()[0]);
        if (m_eval_mtd) {
            plumed_cmd(m_plumedmain, "prepareCalc", NULL);
            plumed_cmd(m_plumedmain, "performCalc", NULL);
        } else {
            if (std::abs(m_T0 - m_aver_Temp) < m_mtd_dT && m_step > 10) {
                m_eval_mtd = true;
                std::cout << "Starting with MetaDynamics ..." << std::endl;
            }
        }
    }
#endif
    WallPotential();
    ekin = 0.0;

    for (int i = 0; i < m_natoms; ++i) {
        m_eigen_velocities.data()[3 * i + 0] -= 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 0] * m_eigen_inv_masses.data()[3 * i + 0];
        m_eigen_velocities.data()[3 * i + 1] -= 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 1] * m_eigen_inv_masses.data()[3 * i + 1];
        m_eigen_velocities.data()[3 * i + 2] -= 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 2] * m_eigen_inv_masses.data()[3 * i + 2];

        ekin += m_eigen_masses.data()[i] * (m_eigen_velocities.data()[3 * i] * m_eigen_velocities.data()[3 * i] + m_eigen_velocities.data()[3 * i + 1] * m_eigen_velocities.data()[3 * i + 1] + m_eigen_velocities.data()[3 * i + 2] * m_eigen_velocities.data()[3 * i + 2]);
        //m_gradient[3 * i + 0] = m_eigen_gradient.data()[3 * i + 0];
        //m_gradient[3 * i + 1] = m_eigen_gradient.data()[3 * i + 1];
        //m_gradient[3 * i + 2] = m_eigen_gradient.data()[3 * i + 2];
    }
    ekin *= 0.5;
    double T = 2.0 * ekin / (kb_Eh * m_dof);
    m_unstable = T > 10000 * m_T || std::isnan(T);
    m_T = T;
    m_Ekin = ekin;
    ThermostatFunction();
    EKin();
}

void SimpleMD::Rattle()
{
    /* this part was adopted from
     * Numerische Simulation in der Moleküldynamik
     * by
     * Griebel, Knapek, Zumbusch, Caglar
     * 2003, Springer-Verlag
     * and from
     * Molecular Simulation of Fluids
     * by Richard J. Sadus
     * some suff was just ignored or corrected
     * like dT^3 -> dT^2 and
     * updated velocities of the second atom (minus instead of plus)
     * and adjusted to some needs
     */
    TriggerWriteRestart();

    auto* coord = new double[3 * m_natoms];
    double m_dT_inverse = 1 / m_dT;
    std::vector<int> moved_12(m_natoms, 0), moved_13(m_natoms, 0);
    bool move = false;
    int dof = m_dof;
    for (int i = 0; i < m_natoms; ++i) {
        coord[3 * i + 0] = m_eigen_geometry.data()[3 * i + 0] + m_dT * m_eigen_velocities.data()[3 * i + 0] - 0.5 * m_eigen_gradient.data()[3 * i + 0] * m_eigen_inv_masses.data()[3 * i + 0] * m_dt2;
        coord[3 * i + 1] = m_eigen_geometry.data()[3 * i + 1] + m_dT * m_eigen_velocities.data()[3 * i + 1] - 0.5 * m_eigen_gradient.data()[3 * i + 1] * m_eigen_inv_masses.data()[3 * i + 1] * m_dt2;
        coord[3 * i + 2] = m_eigen_geometry.data()[3 * i + 2] + m_dT * m_eigen_velocities.data()[3 * i + 2] - 0.5 * m_eigen_gradient.data()[3 * i + 2] * m_eigen_inv_masses.data()[3 * i + 2] * m_dt2;

        m_rt_geom_1[3 * i + 0] = coord[3 * i + 0];
        m_rt_geom_1[3 * i + 1] = coord[3 * i + 1];
        m_rt_geom_1[3 * i + 2] = coord[3 * i + 2];

        m_eigen_velocities.data()[3 * i + 0] -= 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 0] * m_eigen_inv_masses.data()[3 * i + 0];
        m_eigen_velocities.data()[3 * i + 1] -= 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 1] * m_eigen_inv_masses.data()[3 * i + 1];
        m_eigen_velocities.data()[3 * i + 2] -= 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 2] * m_eigen_inv_masses.data()[3 * i + 2];

        m_rt_velo[3 * i + 0] = m_eigen_velocities.data()[3 * i + 0];
        m_rt_velo[3 * i + 1] = m_eigen_velocities.data()[3 * i + 1];
        m_rt_velo[3 * i + 2] = m_eigen_velocities.data()[3 * i + 2];
    }

    double iter = 0;
    double difference = 0;
    double difference_prev = 1e22, difference_curr = 1e22;
    double max = m_rattle_max;
    double scale = 0.1;
    int local_dof = 0;
    while (iter < m_rattle_maxiter) {
        difference_prev = difference_curr;
        difference_curr = 0;
        difference = 0;
        iter++;
        int active = 0;

        for (auto bond : m_bond_constrained) {
            int i = bond.first.first, j = bond.first.second;
            double distance = bond.second;
            double distance_current = ((m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0]) * (m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0])
                + (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1]) * (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1])
                + (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]) * (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]));
            if (std::abs(distance - distance_current) > m_rattle_tol_12) {
                move = true;
                double r = distance - distance_current;
                double dx = m_eigen_geometry.data()[3 * i + 0] - m_eigen_geometry.data()[3 * j + 0];
                double dy = m_eigen_geometry.data()[3 * i + 1] - m_eigen_geometry.data()[3 * j + 1];
                double dz = m_eigen_geometry.data()[3 * i + 2] - m_eigen_geometry.data()[3 * j + 2];

                double scalarproduct = (dx) * (m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0])
                    + (dy) * (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1])
                    + (dz) * (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]);
                // if (scalarproduct >= m_rattle_tolerance * distance) {
                moved_12[i] += 1;
                moved_12[j] += 1;
                if (moved_12[i] == 1)
                    local_dof++;
                active++;
                // std::cout << i << " " << j << " " << distance_current << " " << scalarproduct <<std::endl;

                if (std::abs(scalarproduct) < m_rattle_min) {
                    std::cout << "small" << scalarproduct << " " << distance - distance_current << "" << std::endl;
                    if (scalarproduct < 0)
                        scalarproduct = -1 * m_rattle_min;
                    else
                        scalarproduct = m_rattle_min;

                    std::cout << scalarproduct << std::endl;
                }

                    double lambda = r / (1 * (m_eigen_inv_masses.data()[i] + m_eigen_inv_masses.data()[j]) * scalarproduct);
                    if (std::isinf(lambda)) {
                        std::cout << i << " " << j << std::endl;
                        std::cout << r << " " << scalarproduct << " " << distance_current;
                        std::cout << " " << (coord[3 * i + 0] - coord[3 * j + 0]) << " " << coord[3 * i + 1] - coord[3 * j + 1] << " " << (coord[3 * i + 2] - coord[3 * j + 2]);
                        std::cout << " " << (m_eigen_geometry.data()[3 * i + 0] - m_eigen_geometry.data()[3 * j + 0]) << " " << m_eigen_geometry.data()[3 * i + 1] - m_eigen_geometry.data()[3 * j + 1] << " " << (m_eigen_geometry.data()[3 * i + 2] - m_eigen_geometry.data()[3 * j + 2]);

                        std::cout << "inf" << std::endl;
                        exit(0);
                    }
                    if (std::isnan(lambda)) {
                        std::cout << "nan" << std::endl;
                        exit(0);
                    }

                    while (std::abs(lambda) > max) {
                        // std::cout << " " << lambda << " " << max_mu;
                        lambda *= scale;
                    }

                    m_rt_geom_1[3 * i + 0] = coord[3 * i + 0] + dx * lambda * 0.5 * m_eigen_inv_masses.data()[i];
                    m_rt_geom_1[3 * i + 1] = coord[3 * i + 1] + dy * lambda * 0.5 * m_eigen_inv_masses.data()[i];
                    m_rt_geom_1[3 * i + 2] = coord[3 * i + 2] + dz * lambda * 0.5 * m_eigen_inv_masses.data()[i];

                    m_rt_geom_1[3 * j + 0] = coord[3 * j + 0] - dx * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                    m_rt_geom_1[3 * j + 1] = coord[3 * j + 1] - dy * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                    m_rt_geom_1[3 * j + 2] = coord[3 * j + 2] - dz * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                    /*
                                        coord[3 * i + 0] += dx * lambda * 0.5 * m_eigen_inv_masses.data()[i];
                                        coord[3 * i + 1] += dy * lambda * 0.5 * m_eigen_inv_masses.data()[i];
                                        coord[3 * i + 2] += dz * lambda * 0.5 * m_eigen_inv_masses.data()[i];

                                        coord[3 * j + 0] -= dx * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                                        coord[3 * j + 1] -= dy * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                                        coord[3 * j + 2] -= dz * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                    */

                    /*
                    std::cout <<m_rt_geom_1[3 * i + 0] << " " << m_rt_geom_1[3 * i + 1] << " " << m_rt_geom_1[3 * i + 2] << std::endl;
                    std::cout <<coord[3 * i + 0] << " " << coord[3 * i + 1] << " " << coord[3 * i + 2] << std::endl;

                    std::cout <<m_rt_geom_1[3 * j + 0] << " " << m_rt_geom_1[3 * j + 1] << " " << m_rt_geom_1[3 * j + 2] << std::endl;
                    std::cout <<coord[3 * j + 0] << " " << coord[3 * j + 1] << " " << coord[3 * j + 2] << std::endl << std::endl;

*/
                    double distance_current_New = ((m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0]) * (m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0])
                        + (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1]) * (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1])
                        + (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]) * (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]));
                    /*
                                        double distance_current_New = ((coord[3 * i + 0] - coord[3 * j + 0]) * (coord[3 * i + 0] - coord[3 * j + 0])
                                            + (coord[3 * i + 1] - coord[3 * j + 1]) * (coord[3 * i + 1] - coord[3 * j + 1])
                                            + (coord[3 * i + 2] - coord[3 * j + 2]) * (coord[3 * i + 2] - coord[3 * j + 2]));
                    */
                    difference_curr += std::abs(distance_current_New - distance_current);
                    // std::cout << i<< " "<< j<< " " << distance << " "<< distance_current << " " << distance_current_New << " " << lambda << std::endl << std::endl;

                    /*
                                        m_eigen_velocities.data()[3 * i + 0] += dx * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                                        m_eigen_velocities.data()[3 * i + 1] += dy * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                                        m_eigen_velocities.data()[3 * i + 2] += dz * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;

                                        m_eigen_velocities.data()[3 * j + 0] -= dx * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                                        m_eigen_velocities.data()[3 * j + 1] -= dy * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                                        m_eigen_velocities.data()[3 * j + 2] -= dz * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                    */

                    m_rt_velo[3 * i + 0] = m_eigen_velocities.data()[3 * i + 0] + dx * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                    m_rt_velo[3 * i + 1] = m_eigen_velocities.data()[3 * i + 1] + dy * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                    m_rt_velo[3 * i + 2] = m_eigen_velocities.data()[3 * i + 2] + dz * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;

                    m_rt_velo[3 * j + 0] = m_eigen_velocities.data()[3 * j + 0] - dx * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                    m_rt_velo[3 * j + 1] = m_eigen_velocities.data()[3 * j + 1] - dy * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                    m_rt_velo[3 * j + 2] = m_eigen_velocities.data()[3 * j + 2] - dz * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;

                    //}
            }
        }

        for (auto bond : m_bond_13_constrained) {
            int i = bond.first.first, j = bond.first.second;
            double distance = bond.second;
            double distance_current = ((m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0]) * (m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0])
                + (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1]) * (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1])
                + (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]) * (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]));
            if (std::abs(distance - distance_current) > m_rattle_tol_13) {
                move = true;
                double r = distance - distance_current;
                double dx = m_eigen_geometry.data()[3 * i + 0] - m_eigen_geometry.data()[3 * j + 0];
                double dy = m_eigen_geometry.data()[3 * i + 1] - m_eigen_geometry.data()[3 * j + 1];
                double dz = m_eigen_geometry.data()[3 * i + 2] - m_eigen_geometry.data()[3 * j + 2];

                double scalarproduct = (dx) * (m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0])
                    + (dy) * (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1])
                    + (dz) * (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]);
                // if (scalarproduct >= m_rattle_tolerance * distance) {
                moved_13[i] += 1;
                moved_13[j] += 1;
                if (moved_13[i] == 1)
                    local_dof++;
                active++;
                // std::cout << i << " " << j << " " << distance_current << " " << scalarproduct <<std::endl;

                if (std::abs(scalarproduct) < m_rattle_min) {
                    std::cout << "small" << scalarproduct << " " << distance - distance_current << "" << std::endl;
                    if (scalarproduct < 0)
                        scalarproduct = -1 * m_rattle_min;
                    else
                        scalarproduct = m_rattle_min;

                    std::cout << scalarproduct << std::endl;
                }

                double lambda = r / (1 * (m_eigen_inv_masses.data()[i] + m_eigen_inv_masses.data()[j]) * scalarproduct);
                if (std::isinf(lambda)) {
                    std::cout << i << " " << j << std::endl;
                    std::cout << r << " " << scalarproduct << " " << distance_current;
                    std::cout << " " << (coord[3 * i + 0] - coord[3 * j + 0]) << " " << coord[3 * i + 1] - coord[3 * j + 1] << " " << (coord[3 * i + 2] - coord[3 * j + 2]);
                    std::cout << " " << (m_eigen_geometry.data()[3 * i + 0] - m_eigen_geometry.data()[3 * j + 0]) << " " << m_eigen_geometry.data()[3 * i + 1] - m_eigen_geometry.data()[3 * j + 1] << " " << (m_eigen_geometry.data()[3 * i + 2] - m_eigen_geometry.data()[3 * j + 2]);

                    std::cout << "inf" << std::endl;
                    std::cout << "nan" << std::endl;
                    exit(0);
                }

                while (std::abs(lambda) > max) {
                    // std::cout << " " << lambda << " " << max_mu;
                    lambda *= scale;
                }

                m_rt_geom_1[3 * i + 0] = coord[3 * i + 0] + dx * lambda * 0.5 * m_eigen_inv_masses.data()[i];
                m_rt_geom_1[3 * i + 1] = coord[3 * i + 1] + dy * lambda * 0.5 * m_eigen_inv_masses.data()[i];
                m_rt_geom_1[3 * i + 2] = coord[3 * i + 2] + dz * lambda * 0.5 * m_eigen_inv_masses.data()[i];

                m_rt_geom_1[3 * j + 0] = coord[3 * j + 0] - dx * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                m_rt_geom_1[3 * j + 1] = coord[3 * j + 1] - dy * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                m_rt_geom_1[3 * j + 2] = coord[3 * j + 2] - dz * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                /*
                                    coord[3 * i + 0] += dx * lambda * 0.5 * m_eigen_inv_masses.data()[i];
                                    coord[3 * i + 1] += dy * lambda * 0.5 * m_eigen_inv_masses.data()[i];
                                    coord[3 * i + 2] += dz * lambda * 0.5 * m_eigen_inv_masses.data()[i];

                                 coord[3 * j + 0] -= dx * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                                 coord[3 * j + 1] -= dy * lambda * 0.5 * m_eigen_inv_masses.data()[j];
                                 coord[3 * j + 2] -= dz * lambda * 0.5 * m_eigen_inv_masses.data()[j];
             */

                /*
                std::cout <<m_rt_geom_1[3 * i + 0] << " " << m_rt_geom_1[3 * i + 1] << " " << m_rt_geom_1[3 * i + 2] << std::endl;
                std::cout <<coord[3 * i + 0] << " " << coord[3 * i + 1] << " " << coord[3 * i + 2] << std::endl;

                     std::cout <<m_rt_geom_1[3 * j + 0] << " " << m_rt_geom_1[3 * j + 1] << " " << m_rt_geom_1[3 * j + 2] << std::endl;
                     std::cout <<coord[3 * j + 0] << " " << coord[3 * j + 1] << " " << coord[3 * j + 2] << std::endl << std::endl;

      */
                double distance_current_New = ((m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0]) * (m_rt_geom_1[3 * i + 0] - m_rt_geom_1[3 * j + 0])
                    + (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1]) * (m_rt_geom_1[3 * i + 1] - m_rt_geom_1[3 * j + 1])
                    + (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]) * (m_rt_geom_1[3 * i + 2] - m_rt_geom_1[3 * j + 2]));
                /*
                                    double distance_current_New = ((coord[3 * i + 0] - coord[3 * j + 0]) * (coord[3 * i + 0] - coord[3 * j + 0])
                                        + (coord[3 * i + 1] - coord[3 * j + 1]) * (coord[3 * i + 1] - coord[3 * j + 1])
                                        + (coord[3 * i + 2] - coord[3 * j + 2]) * (coord[3 * i + 2] - coord[3 * j + 2]));
                */
                difference_curr += std::abs(distance_current_New - distance_current);
                // std::cout << i<< " "<< j<< " " << distance << " "<< distance_current << " " << distance_current_New << " " << lambda << std::endl << std::endl;

                /*
                                    m_eigen_velocities.data()[3 * i + 0] += dx * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                                    m_eigen_velocities.data()[3 * i + 1] += dy * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                                    m_eigen_velocities.data()[3 * i + 2] += dz * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;

                                 m_eigen_velocities.data()[3 * j + 0] -= dx * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                                 m_eigen_velocities.data()[3 * j + 1] -= dy * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                                 m_eigen_velocities.data()[3 * j + 2] -= dz * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
             */

                m_rt_velo[3 * i + 0] = m_eigen_velocities.data()[3 * i + 0] + dx * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                m_rt_velo[3 * i + 1] = m_eigen_velocities.data()[3 * i + 1] + dy * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                m_rt_velo[3 * i + 2] = m_eigen_velocities.data()[3 * i + 2] + dz * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;

                m_rt_velo[3 * j + 0] = m_eigen_velocities.data()[3 * j + 0] - dx * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                m_rt_velo[3 * j + 1] = m_eigen_velocities.data()[3 * j + 1] - dy * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                m_rt_velo[3 * j + 2] = m_eigen_velocities.data()[3 * j + 2] - dz * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;

                //}
            }
        }
        /*
                Geometry geometry = m_molecule.getGeometry();
                for (int i = 0; i < m_natoms; ++i) {
                    geometry(i, 0) = m_rt_geom_1[3 * i + 0];
                    geometry(i, 1) = m_rt_geom_1[3 * i + 1];
                    geometry(i, 2) = m_rt_geom_1[3 * i + 2];
                }
                m_molecule.setGeometry(geometry);

                m_molecule.appendXYZFile(Basename() + ".rattle.trj.xyz");
        */
        //   std::cout << "Current Difference: " <<  difference_curr << " Old Difference: " << difference_prev << " increasing " << difference_curr-difference_prev << " " << iter <<  std::endl;
        difference /= double(active);
        if ((iter > 1 && difference_curr < difference_prev)) {
            //    std::cout << "rise " << difference_curr << " " << difference_prev << " " << iter << " ";
        }
        if (active == 0 || (iter > 1 && difference_curr > difference_prev)) {
            // std::cout <<difference_prev << " " << difference_curr << " " << iter;
            // if(iter > 40)
            //     break;
            // else{
            //     max /= 2;
            //     scale /= 2;
            // }
        }

        for (int i = 0; i < m_natoms; ++i) {
            m_eigen_velocities.data()[3 * i + 0] = m_rt_velo[3 * i + 0];
            m_eigen_velocities.data()[3 * i + 1] = m_rt_velo[3 * i + 1];
            m_eigen_velocities.data()[3 * i + 2] = m_rt_velo[3 * i + 2];

            coord[3 * i + 0] = m_rt_geom_1[3 * i + 0];
            coord[3 * i + 1] = m_rt_geom_1[3 * i + 1];
            coord[3 * i + 2] = m_rt_geom_1[3 * i + 2];
        }
    }
    m_dof -= local_dof;
    /*
        for (auto bond : m_bond_constrained) {
            int i = bond.first.first, j = bond.first.second;
            double distance = bond.second;
            double distance_current = ((coord[3 * i + 0] - coord[3 * j + 0]) * (coord[3 * i + 0] - coord[3 * j + 0])
                + (coord[3 * i + 1] - coord[3 * j + 1]) * (coord[3 * i + 1] - coord[3 * j + 1])
                + (coord[3 * i + 2] - coord[3 * j + 2]) * (coord[3 * i + 2] - coord[3 * j + 2]));

                double r = distance - distance_current;
                double dx = m_eigen_geometry.data()[3 * i + 0] - m_eigen_geometry.data()[3 * j + 0];
                double dy = m_eigen_geometry.data()[3 * i + 1] - m_eigen_geometry.data()[3 * j + 1];
                double dz = m_eigen_geometry.data()[3 * i + 2] - m_eigen_geometry.data()[3 * j + 2];

                double scalarproduct = (dx) * (coord[3 * i + 0] - coord[3 * j + 0])
                    + (dy) * (coord[3 * i + 1] - coord[3 * j + 1])
                    + (dz) * (coord[3 * i + 2] - coord[3 * j + 2]);

                double lambda = r / (1 * (m_eigen_inv_masses.data()[i] + m_eigen_inv_masses.data()[j]) * scalarproduct);

                m_eigen_velocities.data()[3 * i + 0] += dx * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                m_eigen_velocities.data()[3 * i + 1] += dy * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;
                m_eigen_velocities.data()[3 * i + 2] += dz * lambda * 0.5 * m_eigen_inv_masses.data()[i] * m_dT_inverse;

                m_eigen_velocities.data()[3 * j + 0] -= dx * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                m_eigen_velocities.data()[3 * j + 1] -= dy * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
                m_eigen_velocities.data()[3 * j + 2] -= dz * lambda * 0.5 * m_eigen_inv_masses.data()[j] * m_dT_inverse;
        }

    */

    if (iter >= m_rattle_maxiter) {
        //  std::cout << "numeric difficulties - 1st step in rattle velocity verlet " << difference << std::endl;
        // std::ofstream restart_file("unstable_curcuma_" + std::to_string(m_currentStep) + ".json");
        // nlohmann::json restart;
        // restart[MethodName()[0]] = WriteRestartInformation();
        // restart_file << restart << std::endl;
        //  PrintStatus();
    }
    double ekin = 0;

    for (int i = 0; i < m_natoms; ++i) {
        m_eigen_geometry.data()[3 * i + 0] = coord[3 * i + 0];
        m_eigen_geometry.data()[3 * i + 1] = coord[3 * i + 1];
        m_eigen_geometry.data()[3 * i + 2] = coord[3 * i + 2];
        ekin += m_eigen_masses.data()[i] * (m_eigen_velocities.data()[3 * i] * m_eigen_velocities.data()[3 * i] + m_eigen_velocities.data()[3 * i + 1] * m_eigen_velocities.data()[3 * i + 1] + m_eigen_velocities.data()[3 * i + 2] * m_eigen_velocities.data()[3 * i + 2]);
    }
    ekin *= 0.5;
    m_T = 2.0 * ekin / (kb_Eh * m_dof);
    m_Ekin = ekin;
    ThermostatFunction();
    m_Epot = Energy();

    if (m_rmsd_mtd) {
        if (m_step % m_mtd_steps == 0) {
            ApplyRMSDMTD();
        }
    }
#ifdef USE_Plumed
    if (m_mtd) {
        plumed_cmd(m_plumedmain, "setStep", &m_step);

        plumed_cmd(m_plumedmain, "setPositions", &m_eigen_geometry.data()[0]);

        plumed_cmd(m_plumedmain, "setEnergy", &m_Epot);
        plumed_cmd(m_plumedmain, "setForces", &m_eigen_gradient.data()[0]);
        plumed_cmd(m_plumedmain, "setVirial", &m_virial[0]);

        plumed_cmd(m_plumedmain, "setMasses", &m_eigen_masses.data()[0]);
        if (m_eval_mtd) {
            plumed_cmd(m_plumedmain, "prepareCalc", NULL);
            plumed_cmd(m_plumedmain, "performCalc", NULL);
        } else {
            if (std::abs(m_T0 - m_aver_Temp) < m_mtd_dT && m_step > 10) {
                m_eval_mtd = true;
                std::cout << "Starting with MetaDynamics ..." << std::endl;
            }
        }
    }
#endif
    WallPotential();

    for (int i = 0; i < m_natoms; ++i) {
        m_eigen_velocities.data()[3 * i + 0] -= 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 0] * m_eigen_inv_masses.data()[3 * i + 0];
        m_eigen_velocities.data()[3 * i + 1] -= 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 1] * m_eigen_inv_masses.data()[3 * i + 1];
        m_eigen_velocities.data()[3 * i + 2] -= 0.5 * m_dT * m_eigen_gradient.data()[3 * i + 2] * m_eigen_inv_masses.data()[3 * i + 2];

        //m_gradient[3 * i + 0] = m_eigen_gradient.data()[3 * i + 0];
        //m_gradient[3 * i + 1] = m_eigen_gradient.data()[3 * i + 1];
        //m_gradient[3 * i + 2] = m_eigen_gradient.data()[3 * i + 2];
    }
    m_virial_correction = 0;
    iter = 0;
    ekin = 0.0;
    double sum_active = 0;

    while (iter < m_rattle_maxiter) {
        iter++;
        int active = 0;
        double sum_mu = 0;
        for (auto bond : m_bond_constrained) {
            int i = bond.first.first, j = bond.first.second;
            if (moved_12[i] != 0 && moved_12[j] != 0) {
                moved_12[i] -= 1;
                moved_12[j] -= 1;
                double distance_current = ((coord[3 * i + 0] - coord[3 * j + 0]) * (coord[3 * i + 0] - coord[3 * j + 0])
                    + (coord[3 * i + 1] - coord[3 * j + 1]) * (coord[3 * i + 1] - coord[3 * j + 1])
                    + (coord[3 * i + 2] - coord[3 * j + 2]) * (coord[3 * i + 2] - coord[3 * j + 2]));

                double distance = bond.second;

                double dx = coord[3 * i + 0] - coord[3 * j + 0];
                double dy = coord[3 * i + 1] - coord[3 * j + 1];
                double dz = coord[3 * i + 2] - coord[3 * j + 2];
                double dvx = m_eigen_velocities.data()[3 * i + 0] - m_eigen_velocities.data()[3 * j + 0];
                double dvy = m_eigen_velocities.data()[3 * i + 1] - m_eigen_velocities.data()[3 * j + 1];
                double dvz = m_eigen_velocities.data()[3 * i + 2] - m_eigen_velocities.data()[3 * j + 2];

                double r = (dx) * (dvx) + (dy) * (dvy) + (dz) * (dvz);

                double mu = -1 * r / ((m_eigen_inv_masses.data()[i] + m_eigen_inv_masses.data()[j]) * distance_current);
                sum_mu += mu;
                if (iter == m_rattle_maxiter) {
                    //      std::cout << i << " " << j << " " << r << " " << (dx) * (dx) + (dy) * (dy) + (dz) * (dz) << " " << distance << " " << mu << " .. ";
                }
                // if (std::abs(mu) > m_rattle_tolerance) {
                while (std::abs(mu) > m_rattle_max)
                    mu *= 0.1;
                active = 1;
                sum_active++;
                m_virial_correction += mu * distance_current;
                m_eigen_velocities.data()[3 * i + 0] += dx * mu * m_eigen_inv_masses.data()[i];
                m_eigen_velocities.data()[3 * i + 1] += dy * mu * m_eigen_inv_masses.data()[i];
                m_eigen_velocities.data()[3 * i + 2] += dz * mu * m_eigen_inv_masses.data()[i];

                m_eigen_velocities.data()[3 * j + 0] -= dx * mu * m_eigen_inv_masses.data()[j];
                m_eigen_velocities.data()[3 * j + 1] -= dy * mu * m_eigen_inv_masses.data()[j];
                m_eigen_velocities.data()[3 * j + 2] -= dz * mu * m_eigen_inv_masses.data()[j];
                // if(iter == m_rattle_maxiter)
                //      std::cout << " " << mu << std::endl;
                //}//lse
                //{
                // std::cout << mu <<std::endl;
                //}
            }
            //   std::cout << sum_mu << " (" << iter << "/" << sum_active << ") ";
        }

        for (auto bond : m_bond_13_constrained) {
            int i = bond.first.first, j = bond.first.second;
            if (moved_13[i] != 0 && moved_13[j] != 0) {
                moved_13[i] -= 1;
                moved_13[j] -= 1;
                double distance_current = ((coord[3 * i + 0] - coord[3 * j + 0]) * (coord[3 * i + 0] - coord[3 * j + 0])
                    + (coord[3 * i + 1] - coord[3 * j + 1]) * (coord[3 * i + 1] - coord[3 * j + 1])
                    + (coord[3 * i + 2] - coord[3 * j + 2]) * (coord[3 * i + 2] - coord[3 * j + 2]));

                double distance = bond.second;

                double dx = coord[3 * i + 0] - coord[3 * j + 0];
                double dy = coord[3 * i + 1] - coord[3 * j + 1];
                double dz = coord[3 * i + 2] - coord[3 * j + 2];
                double dvx = m_eigen_velocities.data()[3 * i + 0] - m_eigen_velocities.data()[3 * j + 0];
                double dvy = m_eigen_velocities.data()[3 * i + 1] - m_eigen_velocities.data()[3 * j + 1];
                double dvz = m_eigen_velocities.data()[3 * i + 2] - m_eigen_velocities.data()[3 * j + 2];

                double r = (dx) * (dvx) + (dy) * (dvy) + (dz) * (dvz);

                double mu = -1 * r / ((m_eigen_inv_masses.data()[i] + m_eigen_inv_masses.data()[j]) * distance_current);
                sum_mu += mu;
                if (iter == m_rattle_maxiter) {
                    //      std::cout << i << " " << j << " " << r << " " << (dx) * (dx) + (dy) * (dy) + (dz) * (dz) << " " << distance << " " << mu << " .. ";
                }
                // if (std::abs(mu) > m_rattle_tolerance) {
                while (std::abs(mu) > m_rattle_max)
                    mu *= 0.1;
                active = 1;
                sum_active++;
                m_virial_correction += mu * distance_current;
                m_eigen_velocities.data()[3 * i + 0] += dx * mu * m_eigen_inv_masses.data()[i];
                m_eigen_velocities.data()[3 * i + 1] += dy * mu * m_eigen_inv_masses.data()[i];
                m_eigen_velocities.data()[3 * i + 2] += dz * mu * m_eigen_inv_masses.data()[i];

                m_eigen_velocities.data()[3 * j + 0] -= dx * mu * m_eigen_inv_masses.data()[j];
                m_eigen_velocities.data()[3 * j + 1] -= dy * mu * m_eigen_inv_masses.data()[j];
                m_eigen_velocities.data()[3 * j + 2] -= dz * mu * m_eigen_inv_masses.data()[j];
                // if(iter == m_rattle_maxiter)
                //      std::cout << " " << mu << std::endl;
                //}//lse
                //{
                // std::cout << mu <<std::endl;
                //}
            }
            //   std::cout << sum_mu << " (" << iter << "/" << sum_active << ") ";
        }
        //   std::cout << std::endl;
        if (active == 0)
            break;
    }

    if (iter >= m_rattle_maxiter) {
        std::cout << "numeric difficulties - 2nd in rattle velocity verlet " << iter << std::endl;
        /*    std::ofstream restart_file("unstable_curcuma_" + std::to_string(m_currentStep) + ".json");
            nlohmann::json restart;
            restart[MethodName()[0]] = WriteRestartInformation();
            restart_file << restart << std::endl; */
        PrintStatus();
    }

    if (move)
        RemoveRotations();

    delete[] coord;
    for (int i = 0; i < m_natoms; ++i) {
        ekin += m_eigen_masses.data()[i] * (m_eigen_velocities.data()[3 * i] * m_eigen_velocities.data()[3 * i] + m_eigen_velocities.data()[3 * i + 1] * m_eigen_velocities.data()[3 * i + 1] + m_eigen_velocities.data()[3 * i + 2] * m_eigen_velocities.data()[3 * i + 2]);
    }
    ekin *= 0.5;
    double T = 2.0 * ekin / (kb_Eh * m_dof);
    m_unstable = T > 10000 * m_T || std::isnan(T);
    m_T = T;
    ThermostatFunction();
    EKin();
    m_dof = dof;
}

void SimpleMD::ApplyRMSDMTD()
{
    std::chrono::time_point<std::chrono::system_clock> m_start, m_end;
    m_start = std::chrono::system_clock::now();
    m_colvar_incr = 0;

    Geometry current_geometry = m_rmsd_mtd_molecule.getGeometry();
    for (int i = 0; i < m_rmsd_indicies.size(); ++i) {
        current_geometry(i, 0) = m_eigen_geometry.data()[3 * m_rmsd_indicies[i] + 0];
        current_geometry(i, 1) = m_eigen_geometry.data()[3 * m_rmsd_indicies[i] + 1];
        current_geometry(i, 2) = m_eigen_geometry.data()[3 * m_rmsd_indicies[i] + 2];
    }

    double current_bias = 0;
    double rmsd_reference = 0;

    if (m_bias_structure_count == 0) {
        m_bias_threads[0]->addGeometry(current_geometry, 0, m_currentStep, 0);
        m_bias_structure_count++;
        m_rmsd_mtd_molecule.writeXYZFile(Basename() + ".mtd.xyz");
        if (m_nocolvarfile == false) {
            std::ofstream colvarfile;
            colvarfile.open("COLVAR");
            colvarfile.close();
        }
    }
    if (m_threads == 1 || m_bias_structure_count == 1) {
        for (auto & m_bias_thread : m_bias_threads) {
            m_bias_thread->setCurrentGeometry(current_geometry, m_currentStep);
            m_bias_thread->start();
            current_bias += m_bias_thread->BiasEnergy();
            for (int j = 0; j < m_rmsd_indicies.size(); ++j) {
                m_eigen_gradient.data()[3 * m_rmsd_indicies[j] + 0] += m_bias_thread->Gradient()(j, 0);
                m_eigen_gradient.data()[3 * m_rmsd_indicies[j] + 1] += m_bias_thread->Gradient()(j, 1);
                m_eigen_gradient.data()[3 * m_rmsd_indicies[j] + 2] += m_bias_thread->Gradient()(j, 2);
            }
            m_colvar_incr += m_bias_thread->Counter();
            m_loop_time += m_bias_thread->getExecutionTime();
        }
    } else {
        if (m_bias_structure_count < m_threads) {
            for (int i = 0; i < m_bias_structure_count; ++i) {
                m_bias_threads[i]->setCurrentGeometry(current_geometry, m_currentStep);
            }
        } else {
            for (auto & m_bias_thread : m_bias_threads) {
                m_bias_thread->setCurrentGeometry(current_geometry, m_currentStep);
            }
        }

        m_bias_pool->setActiveThreadCount(m_threads);
        m_bias_pool->StaticPool();
        m_bias_pool->StartAndWait();
        // m_bias_pool->setWakeUp(m_bias_pool->WakeUp() / 2);

        for (auto & m_bias_thread : m_bias_threads) {
            if (m_bias_thread->getReturnValue() == 1) {

                current_bias += m_bias_thread->BiasEnergy();
                for (int j = 0; j < m_rmsd_indicies.size(); ++j) {
                    m_eigen_gradient.data()[3 * m_rmsd_indicies[j] + 0] += m_bias_thread->Gradient()(j, 0);
                    m_eigen_gradient.data()[3 * m_rmsd_indicies[j] + 1] += m_bias_thread->Gradient()(j, 1);
                    m_eigen_gradient.data()[3 * m_rmsd_indicies[j] + 2] += m_bias_thread->Gradient()(j, 2);
                }
                m_colvar_incr += m_bias_thread->Counter();
            }
            m_loop_time += m_bias_thread->getExecutionTime();
        }
        m_bias_pool->Reset();
    }
    rmsd_reference = m_bias_threads[0]->RMSDReference();
    m_rmsd_mtd_molecule.setGeometry(current_geometry);

    if (m_nocolvarfile == false) {
        std::ofstream colvarfile;
        colvarfile.open("COLVAR", std::iostream::app);
        colvarfile << m_currentStep << " ";
        if (m_rmsd_fragment_count < 2)
            colvarfile << rmsd_reference << " ";

        for (int i = 0; i < m_rmsd_fragment_count; ++i)
            for (int j = 0; j < i; ++j) {
                colvarfile << (m_rmsd_mtd_molecule.Centroid(true, i) - m_rmsd_mtd_molecule.Centroid(true, j)).norm() << " ";
            }
        colvarfile << current_bias << " " << std::endl;
        colvarfile.close();
    }
    m_bias_energy += current_bias;

    if (current_bias * m_rmsd_econv < m_bias_structure_count && m_rmsd_fix_structure == false) {
        int thread_index = m_bias_structure_count % m_bias_threads.size();
        m_bias_threads[thread_index]->addGeometry(current_geometry, rmsd_reference, m_currentStep, m_bias_structure_count);
        m_bias_structure_count++;
        m_rmsd_mtd_molecule.appendXYZFile(Basename() + ".mtd.xyz");
        std::cout << m_bias_structure_count << " stored structures currently" << std::endl;
    }
    m_end = std::chrono::system_clock::now();
    int m_time = std::chrono::duration_cast<std::chrono::milliseconds>(m_end - m_start).count();
    m_mtd_time += m_time;
}

void SimpleMD::Rattle_Verlet_First(double* coord, double* grad)
{
}

void SimpleMD::Rattle_Constrain_First(double* coord, double* grad)
{
}

void SimpleMD::Rattle_Verlet_Second(double* coord, double* grad)
{
}

double SimpleMD::ApplySphericLogFermiWalls()
{
    double potential = 0;
    double kbT = m_wall_temp * kb_Eh;
    int counter = 0;
    double sum_grad = 0; // Claude Generated: Track total wall force
    for (int i = 0; i < m_natoms; ++i) {
        double distance = sqrt(m_eigen_geometry.data()[3 * i + 0] * m_eigen_geometry.data()[3 * i + 0] + m_eigen_geometry.data()[3 * i + 1] * m_eigen_geometry.data()[3 * i + 1] + m_eigen_geometry.data()[3 * i + 2] * m_eigen_geometry.data()[3 * i + 2]);

        // Claude Generated: Add numerical stability - prevent exponential overflow and division by zero
        double beta_arg = m_wall_beta * (distance - m_wall_spheric_radius);
        double exp_expr;
        if (beta_arg > 700.0) { // Prevent overflow
            exp_expr = std::numeric_limits<double>::max() / 2.0;
        } else if (beta_arg < -700.0) { // Prevent underflow
            exp_expr = 0.0;
        } else {
            exp_expr = exp(beta_arg);
        }
        double curr_pot = kbT * log(1 + exp_expr);
        // counter += distance > m_wall_radius;
        // std::cout << m_wall_beta*m_eigen_geometry.data()[3 * i + 0]*exp_expr/(distance*(1-exp_expr)) << " ";
        // Claude Generated: Fix log-Fermi forces - correct denominator (1 + exp) for derivative of log(1 + e^x)
        // Add numerical stability check for distance = 0
        if (distance > 1e-10) {
            double fx = kbT * m_wall_beta * m_eigen_geometry.data()[3 * i + 0] * exp_expr / (distance * (1 + exp_expr));
            double fy = kbT * m_wall_beta * m_eigen_geometry.data()[3 * i + 1] * exp_expr / (distance * (1 + exp_expr));
            double fz = kbT * m_wall_beta * m_eigen_geometry.data()[3 * i + 2] * exp_expr / (distance * (1 + exp_expr));

            m_eigen_gradient.data()[3 * i + 0] -= fx;
            m_eigen_gradient.data()[3 * i + 1] -= fy;
            m_eigen_gradient.data()[3 * i + 2] -= fz;

            // Track wall force magnitude
            sum_grad += std::sqrt(fx * fx + fy * fy + fz * fz);
        }

        // Count atoms outside sphere
        if (distance > m_wall_spheric_radius)
            counter++;

        // std::cout << distance << " ";
        potential += curr_pot;
    }

    // Claude Generated: Smart wall violation reporting - prevent console spam
    m_wall_violation_count = counter;

    // Only report if violations exceed 5% of atoms OR it's been 1000 steps since last report
    bool should_report = (counter > m_natoms * 0.05) || (counter > 0 && (m_currentStep - m_wall_violation_last_reported) > 1000) || (sum_grad > 0.01); // Or if wall forces are very high

    if (should_report) {
        std::cout << "Wall stats - Atoms outside sphere: " << counter << "/" << m_natoms
                  << ", Total wall force: " << sum_grad * au2N << " N"
                  << ", Wall potential: " << potential * au2eV << " eV" << std::endl;
        m_wall_violation_last_reported = m_currentStep;
    }

    return potential;
    // std::cout << potential*kbT << std::endl;
}

double SimpleMD::ApplyRectLogFermiWalls()
{
    double potential = 0;
    double kbT = m_wall_temp * kb_Eh;
    int counter = 0;
    double b = m_wall_beta;
    double sum_grad = 0;
    for (int i = 0; i < m_natoms; ++i) {
        double exp_expr_xl = exp(b * (m_wall_x_min - m_eigen_geometry.data()[3 * i + 0]));
        double exp_expr_xu = exp(b * (m_eigen_geometry.data()[3 * i + 0] - m_wall_x_max));

        double exp_expr_yl = exp(b * (m_wall_y_min - m_eigen_geometry.data()[3 * i + 1]));
        double exp_expr_yu = exp(b * (m_eigen_geometry.data()[3 * i + 1] - m_wall_y_max));

        double exp_expr_zl = exp(b * (m_wall_z_min - m_eigen_geometry.data()[3 * i + 2]));
        double exp_expr_zu = exp(b * (m_eigen_geometry.data()[3 * i + 2] - m_wall_z_max));

        double curr_pot = kbT * (log(1 + exp_expr_xl) + log(1 + exp_expr_xu) + log(1 + exp_expr_yl) + log(1 + exp_expr_yu) + log(1 + exp_expr_zl) + log(1 + exp_expr_zu));
        counter += (m_eigen_geometry.data()[3 * i + 0] - m_wall_x_min) < 0 || (m_wall_x_max - m_eigen_geometry.data()[3 * i + 0]) < 0 || (m_eigen_geometry.data()[3 * i + 1] - m_wall_y_min) < 0 || (m_wall_y_max - m_eigen_geometry.data()[3 * i + 1]) < 0 || (m_eigen_geometry.data()[3 * i + 2] - m_wall_z_min) < 0 || (m_wall_z_max - m_eigen_geometry.data()[3 * i + 2]) < 0;
        // std::cout << i << " " << counter << std::endl;

        // std::cout << m_wall_beta*m_eigen_geometry.data()[3 * i + 0]*exp_expr/(distance*(1-exp_expr)) << " ";
        if (i == 81) {
            //    std::cout << std::endl;
            //    std::cout << m_eigen_geometry.data()[3 * i + 0] << " " << m_eigen_geometry.data()[3 * i + 1] << " " << m_eigen_geometry.data()[3 * i + 2] << std::endl;
            //    std::cout << m_eigen_gradient.data()[3 * i + 0] << " " << m_eigen_gradient.data()[3 * i + 1] << " " <<m_eigen_gradient.data()[3 * i + 2] << std::endl;
        }
        // Claude Generated: Fix rectangular log-Fermi forces - correct denominator (1 + exp) for derivative
        double fx = kbT * b * (exp_expr_xu / (1 + exp_expr_xu) - exp_expr_xl / (1 + exp_expr_xl));
        double fy = kbT * b * (exp_expr_yu / (1 + exp_expr_yu) - exp_expr_yl / (1 + exp_expr_yl));
        double fz = kbT * b * (exp_expr_zu / (1 + exp_expr_zu) - exp_expr_zl / (1 + exp_expr_zl));

        m_eigen_gradient.data()[3 * i + 0] += fx;
        m_eigen_gradient.data()[3 * i + 1] += fy;
        m_eigen_gradient.data()[3 * i + 2] += fz;

        // Track wall force magnitude
        sum_grad += std::abs(fx) + std::abs(fy) + std::abs(fz);
        // if( i == 81)
        {
            // std::cout << i << " " <<m_eigen_gradient.data()[3 * i + 0] << " " << m_eigen_gradient.data()[3 * i + 1] << " " <<m_eigen_gradient.data()[3 * i + 2] << std::endl;
        }
        // std::cout << distance << " ";
        potential += curr_pot;
    }

    // Claude Generated: Smart wall violation reporting - prevent console spam
    m_wall_violation_count = counter;

    // Only report if violations exceed 5% of atoms OR it's been 1000 steps since last report
    bool should_report = (counter > m_natoms * 0.05) || (counter > 0 && (m_currentStep - m_wall_violation_last_reported) > 1000) || (sum_grad > 0.01); // Or if wall forces are very high

    if (should_report) {
        std::cout << "Wall stats - Atoms outside rectangular: " << counter << "/" << m_natoms
                  << ", Total wall force: " << sum_grad * au2N << " N"
                  << ", Wall potential: " << potential * au2eV << " eV" << std::endl;
        m_wall_violation_last_reported = m_currentStep;
    }

    return potential;
    // std::cout << potential*kbT << std::endl;
}

double SimpleMD::ApplySphericHarmonicWalls()
{
    double potential = 0;
    double k = m_wall_temp * kb_Eh;
    int counter = 0;
    double sum_grad = 0; // Claude Generated: Track total wall force
    for (int i = 0; i < m_natoms; ++i) {
        double distance = sqrt(m_eigen_geometry.data()[3 * i + 0] * m_eigen_geometry.data()[3 * i + 0] + m_eigen_geometry.data()[3 * i + 1] * m_eigen_geometry.data()[3 * i + 1] + m_eigen_geometry.data()[3 * i + 2] * m_eigen_geometry.data()[3 * i + 2]);
        double curr_pot = 0.5 * k * (m_wall_spheric_radius - distance) * (m_wall_spheric_radius - distance) * (distance > m_wall_spheric_radius);
        double out = distance > m_wall_spheric_radius;
        counter += out;

        double diff = k * (m_wall_spheric_radius - distance) * (distance > m_wall_spheric_radius);

        double dx = diff * m_eigen_geometry.data()[3 * i + 0] / distance;
        double dy = diff * m_eigen_geometry.data()[3 * i + 1] / distance;
        double dz = diff * m_eigen_geometry.data()[3 * i + 2] / distance;

        m_eigen_gradient.data()[3 * i + 0] -= dx;
        m_eigen_gradient.data()[3 * i + 1] -= dy;
        m_eigen_gradient.data()[3 * i + 2] -= dz;

        // Claude Generated: Track wall force magnitude
        sum_grad += std::sqrt(dx * dx + dy * dy + dz * dz);

        /*
        if(out)
        {
            std::cout << m_eigen_geometry.data()[3 * i + 0]  << " " << m_eigen_geometry.data()[3 * i + 1]  << " " << m_eigen_geometry.data()[3 * i + 2] << std::endl;
            std::cout << dx << " " << dy << " " << dz << std::endl;
        }*/
        // std::cout << distance << " ";
        potential += curr_pot;
    }

    // Claude Generated: Smart wall violation reporting - prevent console spam
    m_wall_violation_count = counter;

    // Only report if violations exceed 5% of atoms OR it's been 1000 steps since last report
    bool should_report = (counter > m_natoms * 0.05) || (counter > 0 && (m_currentStep - m_wall_violation_last_reported) > 1000) || (sum_grad > 0.01); // Or if wall forces are very high

    if (should_report) {
        std::cout << "Wall stats - Atoms outside sphere: " << counter << "/" << m_natoms
                  << ", Total wall force: " << sum_grad * au2N << " N"
                  << ", Wall potential: " << potential * au2eV << " eV" << std::endl;
        m_wall_violation_last_reported = m_currentStep;
    }

    return potential;
    // std::cout << potential*kbT << std::endl;
}

double SimpleMD::ApplyRectHarmonicWalls()
{
    double potential = 0;
    double k = m_wall_temp * kb_Eh;
    int counter = 0;
    double b = m_wall_beta;
    double sum_grad = 0;
    for (int i = 0; i < m_natoms; ++i) {
        double Vx = (m_eigen_geometry.data()[3 * i + 0] - m_wall_x_min) * (m_eigen_geometry.data()[3 * i + 0] - m_wall_x_min) * (m_eigen_geometry.data()[3 * i + 0] < m_wall_x_min)
            + (m_eigen_geometry.data()[3 * i + 0] - m_wall_x_max) * (m_eigen_geometry.data()[3 * i + 0] - m_wall_x_max) * (m_eigen_geometry.data()[3 * i + 0] > m_wall_x_max);

        double Vy = (m_eigen_geometry.data()[3 * i + 1] - m_wall_y_min) * (m_eigen_geometry.data()[3 * i + 1] - m_wall_y_min) * (m_eigen_geometry.data()[3 * i + 1] < m_wall_y_min)
            + (m_eigen_geometry.data()[3 * i + 1] - m_wall_y_max) * (m_eigen_geometry.data()[3 * i + 1] - m_wall_y_max) * (m_eigen_geometry.data()[3 * i + 1] > m_wall_y_max);

        double Vz = (m_eigen_geometry.data()[3 * i + 2] - m_wall_z_min) * (m_eigen_geometry.data()[3 * i + 2] - m_wall_z_min) * (m_eigen_geometry.data()[3 * i + 2] < m_wall_z_min)
            + (m_eigen_geometry.data()[3 * i + 2] - m_wall_z_max) * (m_eigen_geometry.data()[3 * i + 2] - m_wall_z_max) * (m_eigen_geometry.data()[3 * i + 2] > m_wall_z_max);

        double curr_pot = 0.5 * k * (Vx + Vy + Vz);
        int out = (m_eigen_geometry.data()[3 * i + 0] - m_wall_x_min) < 0 || (m_wall_x_max - m_eigen_geometry.data()[3 * i + 0]) < 0 || (m_eigen_geometry.data()[3 * i + 1] - m_wall_y_min) < 0 || (m_wall_y_max - m_eigen_geometry.data()[3 * i + 1]) < 0 || (m_eigen_geometry.data()[3 * i + 2] - m_wall_z_min) < 0 || (m_wall_z_max - m_eigen_geometry.data()[3 * i + 2]) < 0;
        counter += out;

        // std::cout << i << " " << counter << std::endl;

        // Claude Generated: Fix harmonic wall forces - remove std::abs() for correct force direction
        // Force = -k * displacement, where displacement is signed distance from boundary
        double dx = k * ((m_eigen_geometry.data()[3 * i + 0] - m_wall_x_min) * (m_eigen_geometry.data()[3 * i + 0] < m_wall_x_min) - (m_eigen_geometry.data()[3 * i + 0] - m_wall_x_max) * (m_eigen_geometry.data()[3 * i + 0] > m_wall_x_max));

        double dy = k * ((m_eigen_geometry.data()[3 * i + 1] - m_wall_y_min) * (m_eigen_geometry.data()[3 * i + 1] < m_wall_y_min) - (m_eigen_geometry.data()[3 * i + 1] - m_wall_y_max) * (m_eigen_geometry.data()[3 * i + 1] > m_wall_y_max));

        double dz = k * ((m_eigen_geometry.data()[3 * i + 2] - m_wall_z_min) * (m_eigen_geometry.data()[3 * i + 2] < m_wall_z_min) - (m_eigen_geometry.data()[3 * i + 2] - m_wall_z_max) * (m_eigen_geometry.data()[3 * i + 2] > m_wall_z_max));
        m_eigen_gradient.data()[3 * i + 0] -= dx;
        m_eigen_gradient.data()[3 * i + 1] -= dy;
        m_eigen_gradient.data()[3 * i + 2] -= dz;
        /* if(out)
         {
             std::cout << m_eigen_geometry.data()[3 * i + 0]  << " " << m_eigen_geometry.data()[3 * i + 1]  << " " << m_eigen_geometry.data()[3 * i + 2] << std::endl;
             std::cout << dx << " " << dy << " " << dz << std::endl;
         }*/
        sum_grad += std::abs(dx) + std::abs(dy) + std::abs(dz);

        potential += curr_pot;
    }

    // Claude Generated: Smart wall violation reporting - prevent console spam
    m_wall_violation_count = counter;

    // Only report if violations exceed 5% of atoms OR it's been 1000 steps since last report
    bool should_report = (counter > m_natoms * 0.05) || (counter > 0 && (m_currentStep - m_wall_violation_last_reported) > 1000) || (sum_grad > 0.01); // Or if wall forces are very high

    if (should_report) {
        std::cout << "Wall stats - Atoms outside rectangular: " << counter << "/" << m_natoms
                  << ", Total wall force: " << sum_grad * au2N << " N"
                  << ", Wall potential: " << potential * au2eV << " eV" << std::endl;
        m_wall_violation_last_reported = m_currentStep;
    }

    return potential;
    // std::cout << potential*kbT << std::endl;
}

void SimpleMD::RemoveRotations()
{
    /*
     * This code was taken and adopted from the xtb sources
     * https://github.com/grimme-lab/xtb/blob/main/src/rmrottr.f90
     * Special thanks to the developers
     */
    double mass = 0;
    Position pos = { 0, 0, 0 }, angom{ 0, 0, 0 };
    Geometry geom(m_natoms, 3);

    std::vector<std::vector<int>> fragments = m_molecule.GetFragments();
    // std::cout << fragments.size() << std::endl;
    for (auto & fragment : fragments) {
        for (const int i : fragment) {
            const double m = m_eigen_masses.data()[i];
            mass += m;
            pos(0) += m * m_eigen_geometry.data()[3 * i + 0];
            pos(1) += m * m_eigen_geometry.data()[3 * i + 1];
            pos(2) += m * m_eigen_geometry.data()[3 * i + 2];

            geom(i, 0) = m_eigen_geometry.data()[3 * i + 0];
            geom(i, 1) = m_eigen_geometry.data()[3 * i + 1];
            geom(i, 2) = m_eigen_geometry.data()[3 * i + 2];
        }
        pos(0) /= mass;
        pos(1) /= mass;
        pos(2) /= mass;

        Geometry matrix = Geometry::Zero(3, 3);
        for (const int i : fragment) {
            const double m = m_eigen_masses.data()[i];
            geom(i, 0) -= pos(0);
            geom(i, 1) -= pos(1);
            geom(i, 2) -= pos(2);

            const double x = geom(i, 0);
            const double y = geom(i, 1);
            const double z = geom(i, 2);
            angom(0) += m_eigen_masses.data()[i] * (geom(i, 1) *  m_eigen_velocities.data()[3 * i + 2] - geom(i, 2) *  m_eigen_velocities.data()[3 * i + 1]);
            angom(1) += m_eigen_masses.data()[i] * (geom(i, 2) *  m_eigen_velocities.data()[3 * i + 0] - geom(i, 0) *  m_eigen_velocities.data()[3 * i + 2]);
            angom(2) += m_eigen_masses.data()[i] * (geom(i, 0) *  m_eigen_velocities.data()[3 * i + 1] - geom(i, 1) *  m_eigen_velocities.data()[3 * i + 0]);
            const double x2 = x * x;
            const double y2 = y * y;
            const double z2 = z * z;
            matrix(0, 0) += m * (y2 + z2);
            matrix(1, 1) += m * (x2 + z2);
            matrix(2, 2) += m * (x2 + y2);
            matrix(0, 1) -= m * x * y;
            matrix(0, 2) -= m * x * z;
            matrix(1, 2) -= m * y * z;
        }
        matrix(1, 0) = matrix(0, 1);
        matrix(2, 0) = matrix(0, 2);
        matrix(2, 1) = matrix(1, 2);

        Position omega = matrix.inverse() * angom;

        Position rlm = { 0, 0, 0 }, ram = { 0, 0, 0 };
        for (const int i : fragment) {
            rlm(0) = rlm(0) + m_eigen_masses.data()[i] *  m_eigen_velocities.data()[3 * i + 0];
            rlm(1) = rlm(1) + m_eigen_masses.data()[i] *  m_eigen_velocities.data()[3 * i + 1];
            rlm(2) = rlm(2) + m_eigen_masses.data()[i] *  m_eigen_velocities.data()[3 * i + 2];
        }

        for (const int i : fragment) {
            ram(0) = (omega(1) * geom(i, 2) - omega(2) * geom(i, 1));
            ram(1) = (omega(2) * geom(i, 0) - omega(0) * geom(i, 2));
            ram(2) = (omega(0) * geom(i, 1) - omega(1) * geom(i, 0));

             m_eigen_velocities.data()[3 * i + 0] = m_eigen_velocities.data()[3 * i + 0] - rlm(0) / mass - ram(0);
             m_eigen_velocities.data()[3 * i + 1] = m_eigen_velocities.data()[3 * i + 1] - rlm(1) / mass - ram(1);
             m_eigen_velocities.data()[3 * i + 2] = m_eigen_velocities.data()[3 * i + 2] - rlm(2) / mass - ram(2);
        }
    }
}

void SimpleMD::RemoveRotation()
{
    /*
     * This code was taken and adopted from the xtb sources
     * https://github.com/grimme-lab/xtb/blob/main/src/rmrottr.f90
     * Special thanks to the developers
     */
    double mass = 0;
    Position pos = { 0, 0, 0 }, angom{ 0, 0, 0 };
    Geometry geom(m_natoms, 3);

    for (int i = 0; i < m_natoms; ++i) {
        double m = m_eigen_masses.data()[i];
        mass += m;
        pos(0) += m * m_eigen_geometry.data()[3 * i + 0];
        pos(1) += m * m_eigen_geometry.data()[3 * i + 1];
        pos(2) += m * m_eigen_geometry.data()[3 * i + 2];

        geom(i, 0) = m_eigen_geometry.data()[3 * i + 0];
        geom(i, 1) = m_eigen_geometry.data()[3 * i + 1];
        geom(i, 2) = m_eigen_geometry.data()[3 * i + 2];
    }
    pos(0) /= mass;
    pos(1) /= mass;
    pos(2) /= mass;

    Geometry matrix = Geometry::Zero(3, 3);
    for (int i = 0; i < m_natoms; ++i) {
        double m = m_eigen_masses.data()[i];
        geom(i, 0) -= pos(0);
        geom(i, 1) -= pos(1);
        geom(i, 2) -= pos(2);

        double x = geom(i, 0);
        double y = geom(i, 1);
        double z = geom(i, 2);
        angom(0) += m_eigen_masses.data()[i] * (geom(i, 1) * m_eigen_velocities.data()[3 * i + 2] - geom(i, 2) *  m_eigen_velocities.data()[3 * i + 1]);
        angom(1) += m_eigen_masses.data()[i] * (geom(i, 2) * m_eigen_velocities.data()[3 * i + 0] - geom(i, 0) *  m_eigen_velocities.data()[3 * i + 2]);
        angom(2) += m_eigen_masses.data()[i] * (geom(i, 0) * m_eigen_velocities.data()[3 * i + 1] - geom(i, 1) *  m_eigen_velocities.data()[3 * i + 0]);
        double x2 = x * x;
        double y2 = y * y;
        double z2 = z * z;
        matrix(0, 0) += m * (y2 + z2);
        matrix(1, 1) += m * (x2 + z2);
        matrix(2, 2) += m * (x2 + y2);
        matrix(0, 1) -= m * x * y;
        matrix(0, 2) -= m * x * z;
        matrix(1, 2) -= m * y * z;
    }
    matrix(1, 0) = matrix(0, 1);
    matrix(2, 0) = matrix(0, 2);
    matrix(2, 1) = matrix(1, 2);

    Position omega = matrix.inverse() * angom;

    Position rlm = { 0, 0, 0 }, ram = { 0, 0, 0 };
    for (int i = 0; i < m_natoms; ++i) {
        rlm(0) = rlm(0) + m_eigen_masses.data()[i] *  m_eigen_velocities.data()[3 * i + 0];
        rlm(1) = rlm(1) + m_eigen_masses.data()[i] *  m_eigen_velocities.data()[3 * i + 1];
        rlm(2) = rlm(2) + m_eigen_masses.data()[i] *  m_eigen_velocities.data()[3 * i + 2];
    }

    for (int i = 0; i < m_natoms; ++i) {
        ram(0) = (omega(1) * geom(i, 2) - omega(2) * geom(i, 1));
        ram(1) = (omega(2) * geom(i, 0) - omega(0) * geom(i, 2));
        ram(2) = (omega(0) * geom(i, 1) - omega(1) * geom(i, 0));

         m_eigen_velocities.data()[3 * i + 0] =  m_eigen_velocities.data()[3 * i + 0] - rlm(0) / mass - ram(0);
         m_eigen_velocities.data()[3 * i + 1] =  m_eigen_velocities.data()[3 * i + 1] - rlm(1) / mass - ram(1);
         m_eigen_velocities.data()[3 * i + 2] =  m_eigen_velocities.data()[3 * i + 2] - rlm(2) / mass - ram(2);
    }
}

void SimpleMD::PrintStatus() const
{
    const auto unix_timestamp = std::chrono::seconds(std::time(NULL));

    const int current = std::chrono::milliseconds(unix_timestamp).count();
    const double duration = (current - m_unix_started) / (1000.0 * static_cast<double>(m_currentStep));
    double remaining;
    if (const double tmp = (m_maxtime - m_currentStep) * duration / 60; tmp >= 1)
        remaining = tmp;
    else
        remaining = (m_maxtime - m_currentStep) * duration;
#pragma message("awfull, fix it ")
    if (m_writeUnique) {
#ifdef GCC
        std::cout << fmt::format("{1: ^{0}f} {2: ^{0}f} {3: ^{0}f} {4: ^{0}f} {5: ^{0}f} {6: ^{0}f} {7: ^{0}f} {8: ^{0}f} {9: ^{0}f} {10: ^{0}f} {11: ^{0}f} {12: ^{0}f} {13: ^{0}f} {14: ^{0}f} {15: ^{0}} {16: ^{0}}\n", 15,
            m_currentStep / 1000, m_Epot, m_aver_Epot, m_Ekin, m_aver_Ekin, m_Etot, m_aver_Etot, m_T, m_aver_Temp, m_wall_potential, m_average_wall_potential, m_virial_correction, m_average_virial_correction, remaining, m_time_step / 1000.0, m_unqiue->StoredStructures());
#else
        std::cout << m_currentStep * m_dT / fs2amu / 1000 << " " << m_Epot << " " << m_Ekin << " " << m_Epot + m_Ekin << m_T << std::endl;

#endif
    } else {
#ifdef GCC
        if (m_dipole)
            std::cout << fmt::format("{1: ^{0}f} {2: ^{0}f} {3: ^{0}f} {4: ^{0}f} {5: ^{0}f} {6: ^{0}f} {7: ^{0}f} {8: ^{0}f} {9: ^{0}f} {10: ^{0}f} {11: ^{0}f} {12: ^{0}f} {13: ^{0}f} {14: ^{0}f} {15: ^{0}f} {16: ^{0}f}\n", 15,
                m_currentStep / 1000, m_Epot, m_aver_Epot, m_Ekin, m_aver_Ekin, m_Etot, m_aver_Etot, m_T, m_aver_Temp, m_wall_potential, m_average_wall_potential, m_aver_dipol_linear * 2.5418 * 3.3356, m_virial_correction, m_average_virial_correction, remaining, m_time_step / 1000.0);
        else
            std::cout << fmt::format("{1: ^{0}f} {2: ^{0}f} {3: ^{0}f} {4: ^{0}f} {5: ^{0}f} {6: ^{0}f} {7: ^{0}f} {8: ^{0}f} {9: ^{0}f} {10: ^{0}f} {11: ^{0}f} {12: ^{0}f} {13: ^{0}f} {14: ^{0}f} {15: ^{0}f}\n", 15,
                m_currentStep / 1000, m_Epot, m_aver_Epot, m_Ekin, m_aver_Ekin, m_Etot, m_aver_Etot, m_T, m_aver_Temp, m_wall_potential, m_average_wall_potential, m_virial_correction, m_average_virial_correction, remaining, m_time_step / 1000.0);
#else
        std::cout << m_currentStep * m_dT / fs2amu / 1000 << " " << m_Epot << " " << m_Ekin << " " << m_Epot + m_Ekin << m_T << std::endl;

#endif
    }

    //std::cout << m_mtd_time << " " << m_loop_time << std::endl;
}

void SimpleMD::PrintMatrix(const double* matrix) const
{
    std::cout << "Print Matrix" << std::endl;
    for (int i = 0; i < m_natoms; ++i) {
        std::cout << matrix[3 * i] << " " << matrix[3 * i + 1] << " " << matrix[3 * i + 2] << std::endl;
    }
    std::cout << std::endl;
}

double SimpleMD::CleanEnergy()
{
    // Claude Generated: Use new constructor with basename for parameter caching
    EnergyCalculator interface(m_method, m_defaults, Basename());
    interface.setMolecule(m_molecule.getMolInfo());
    interface.updateGeometry(m_eigen_geometry);

    const double Energy = interface.CalculateEnergy(true);
    m_eigen_gradient = interface.Gradient();
    if (m_dipole && m_method == "gfn2") {
        m_molecule.setDipole(interface.Dipole()*au);//in eA
        m_molecule.setPartialCharges(interface.Charges());
    }
    return Energy;
}

double SimpleMD::FastEnergy()
{
    m_interface->updateGeometry(m_eigen_geometry);

    const double Energy = m_interface->CalculateEnergy(true);
    m_eigen_gradient = m_interface->Gradient();

    if (m_dipole && m_method == "gfn2") {
        m_molecule.setDipole(m_interface->Dipole()*au);// in eA
        m_molecule.setPartialCharges(m_interface->Charges());
    }
    return Energy;
}

void SimpleMD::EKin()
{
    double ekin = 0;
    for (int i = 0; i < m_natoms; ++i) {
        ekin += m_eigen_masses.data()[i] * (m_eigen_velocities.data()[3 * i] * m_eigen_velocities.data()[3 * i] + m_eigen_velocities.data()[3 * i + 1] * m_eigen_velocities.data()[3 * i + 1] + m_eigen_velocities.data()[3 * i + 2] * m_eigen_velocities.data()[3 * i + 2]);
    }
    ekin *= 0.5;
    m_Ekin = ekin;
    m_T = 2.0 * ekin / (kb_Eh * m_dof);
}

void SimpleMD::AverageQuantities()
{
    m_aver_Temp = (m_T + (m_currentStep)*m_aver_Temp) / (m_currentStep + 1);
    m_aver_Epot = (m_Epot + (m_currentStep)*m_aver_Epot) / (m_currentStep + 1);
    m_aver_Ekin = (m_Ekin + (m_currentStep)*m_aver_Ekin) / (m_currentStep + 1);
    m_aver_Etot = (m_Etot + (m_currentStep)*m_aver_Etot) / (m_currentStep + 1);
    if (m_dipole) {
        //m_aver_dipol = (m_curr_dipoles + (m_currentStep)*m_aver_dipol) / (m_currentStep + 1);
    }
    m_average_wall_potential = (m_wall_potential + (m_currentStep)*m_average_wall_potential) / (m_currentStep + 1);
    m_average_virial_correction = (m_virial_correction + (m_currentStep)*m_average_virial_correction) / (m_currentStep + 1);
}

bool SimpleMD::WriteGeometry()
{
    bool result = true;
    Geometry geometry = m_molecule.getGeometry();
    for (int i = 0; i < m_natoms; ++i) {
        geometry(i, 0) = m_eigen_geometry.data()[3 * i + 0];
        geometry(i, 1) = m_eigen_geometry.data()[3 * i + 1];
        geometry(i, 2) = m_eigen_geometry.data()[3 * i + 2];
    }
    TriggerWriteRestart();
    m_molecule.setGeometry(geometry);

    if (m_writeXYZ) {
        m_molecule.setEnergy(m_Epot);
        m_molecule.setName(std::to_string(m_currentStep));
        m_molecule.appendXYZFile(Basename() + ".trj.xyz");
    }
    if (m_writeUnique) {
        if (m_unqiue->CheckMolecule(new Molecule(m_molecule))) {
            std::cout << " ** new structure was added **" << std::endl;
            PrintStatus();
            m_time_step = 0;
            m_unique_structures.push_back(new Molecule(m_molecule));
        }
    }
    return result;
}

void SimpleMD::None()
{
}

void SimpleMD::Berendson()
{
    double lambda = sqrt(1 + (m_dT / 2.0 * (m_T0 - m_T)) / (m_T * m_coupling));
    for (int i = 0; i < m_natoms; ++i) {
        m_eigen_velocities.data()[3 * i + 0] *= lambda;
        m_eigen_velocities.data()[3 * i + 1] *= lambda;
        m_eigen_velocities.data()[3 * i + 2] *= lambda;
    }
}

void SimpleMD::CSVR()
{
    double Ekin_target = 0.5 * kb_Eh * (m_T0)*m_dof;
    double c = exp(-(m_dT / 2.0 * m_respa) / m_coupling);
    static std::default_random_engine rd{};
    static std::mt19937 gen{ rd() };
    static std::normal_distribution<> d{ 0, 1 };
    static std::chi_squared_distribution<float> dchi{ static_cast<float>(m_dof) };
    double R = d(gen);
    double SNf = dchi(gen);
    double alpha2 = c + (1 - c) * (SNf + R * R) * Ekin_target / (m_dof * m_Ekin) + 2 * R * sqrt(c * (1 - c) * Ekin_target / (m_dof * m_Ekin));
    m_Ekin_exchange += m_Ekin * (alpha2 - 1);
    double alpha = sqrt(alpha2);
    for (int i = 0; i < m_natoms; ++i) {
        m_eigen_velocities.data()[3 * i + 0] *= alpha;
        m_eigen_velocities.data()[3 * i + 1] *= alpha;
        m_eigen_velocities.data()[3 * i + 2] *= alpha;

        m_atom_temp[i].push_back(m_eigen_masses.data()[i] * (m_eigen_velocities.data()[3 * i + 0] * m_eigen_velocities.data()[3 * i + 0] + m_eigen_velocities.data()[3 * i + 1] * m_eigen_velocities.data()[3 * i + 1] + m_eigen_velocities.data()[3 * i + 2] * m_eigen_velocities.data()[3 * i + 2]) / (kb_Eh * m_dof));
    }
    m_seed++;
}

void SimpleMD::Anderson()
{
    static std::default_random_engine generator;
    double probability = m_anderson * m_dT;
    std::uniform_real_distribution<double> uniform_dist(0.0, 1.0);
    for (size_t i = 0; i < m_natoms; ++i) {
        if (uniform_dist(generator) < probability) {
            std::normal_distribution<double> distribution(0.0, std::sqrt(kb_Eh * m_T0 * m_eigen_inv_masses.data()[i]));
            m_eigen_velocities.data()[3 * i + 0] = (m_eigen_velocities.data()[3 * i + 0] + distribution(generator)) / 2.0;
            m_eigen_velocities.data()[3 * i + 1] = (m_eigen_velocities.data()[3 * i + 1] + distribution(generator)) / 2.0;
            m_eigen_velocities.data()[3 * i + 2] = (m_eigen_velocities.data()[3 * i + 2] + distribution(generator)) / 2.0;
            m_seed += 3;
        }
    }
}
void SimpleMD::NoseHover()
{
    // Berechnung der kinetischen Energie
    double kinetic_energy = 0.0;
    for (int i = 0; i < m_natoms; ++i) {
        kinetic_energy += 0.5 * m_eigen_masses.data()[i] * (m_eigen_velocities.data()[3 * i] * m_eigen_velocities.data()[3 * i] + m_eigen_velocities.data()[3 * i + 1] * m_eigen_velocities.data()[3 * i + 1] + m_eigen_velocities.data()[3 * i + 2] * m_eigen_velocities.data()[3 * i + 2]);
    }
    // Update der Thermostatkette
    m_xi[0] += 0.5 * m_dT * (2.0 * kinetic_energy - m_dof * m_T0 * kb_Eh) / m_Q[0];
    for (int j = 1; j < m_chain_length; ++j) {
        m_xi[j] += 0.5 * m_dT * (m_Q[j - 1] * m_xi[j - 1] * m_xi[j - 1] - m_T0 * kb_Eh) / m_Q[j];
    }

    // Update der Geschwindigkeiten
    double scale = exp(-m_xi[0] * m_dT);
    for (int i = 0; i < m_natoms; ++i) {
        m_eigen_velocities.data()[3 * i + 0] *= scale;
        m_eigen_velocities.data()[3 * i + 1] *= scale;
        m_eigen_velocities.data()[3 * i + 2] *= scale;
    }

    // Rückwärts-Update der Thermostatkette
    for (int j = m_chain_length - 1; j >= 1; --j) {
        m_xi[j] += 0.5 * m_dT * (m_Q[j - 1] * m_xi[j - 1] * m_xi[j - 1] - m_T0 * kb_Eh) / m_Q[j];
    }
    m_xi[0] += 0.5 * m_dT * (2.0 * kinetic_energy - m_dof * m_T0 * kb_Eh) / m_Q[0];
}
