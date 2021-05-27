//==============================================================================
// TwoMomentRad - a radiation transport library for patch-based AMR codes
// Copyright 2020 Benjamin Wibking.
// Released under the MIT license. See LICENSE file included in the GitHub repo.
//==============================================================================
/// \file test_radiation_marshak.cpp
/// \brief Defines a test problem for radiation in the diffusion regime.
///

#include "test_radiation_marshak_cgs.hpp"
#include "AMReX_BC_TYPES.H"
#include "AMReX_Config.H"
#include <mpi.h>

auto main(int argc, char **argv) -> int
{
	// Initialization (copied from ExaWind)

	amrex::Initialize(argc, argv, true, MPI_COMM_WORLD, []() {
		amrex::ParmParse pp("amrex");
		// Set the defaults so that we throw an exception instead of attempting
		// to generate backtrace files. However, if the user has explicitly set
		// these options in their input files respect those settings.
		if (!pp.contains("throw_exception")) {
			pp.add("throw_exception", 1);
		}
		if (!pp.contains("signal_handling")) {
			pp.add("signal_handling", 0);
		}
	});

	int result = 0;

	{ // objects must be destroyed before amrex::finalize, so enter new
	  // scope here to do that automatically

		result = testproblem_radiation_marshak_cgs();

	} // destructors must be called before amrex::Finalize()
	amrex::Finalize();

	return result;
}

struct SuOlsonProblemCgs {
}; // dummy type to allow compile-type polymorphism via template specialization

// Su & Olson (1997) parameters
constexpr double eps_SuOlson = 1.0;	  // dimensionless
constexpr double kappa = 577.0;		  // g cm^-2 (opacity)
constexpr double rho = 10.0;		  // g cm^-3 (matter density)
constexpr double T_hohlraum = 3.481334e6; // K
constexpr double a_rad = 7.5646e-15;	  // erg cm^-3 K^-4
constexpr double c = 2.99792458e10;	  // cm s^-1
constexpr double alpha_SuOlson = 4.0 * a_rad / eps_SuOlson;

template <> struct RadSystem_Traits<SuOlsonProblemCgs> {
	static constexpr double c_light = c_light_cgs_;
	static constexpr double c_hat = c_light_cgs_;
	static constexpr double radiation_constant = radiation_constant_cgs_;
	static constexpr double mean_molecular_mass = hydrogen_mass_cgs_;
	static constexpr double boltzmann_constant = boltzmann_constant_cgs_;
	static constexpr double gamma = 5. / 3.;
	static constexpr double Erad_floor = 0.;
	static constexpr bool do_marshak_left_boundary = true;
	static constexpr double T_marshak_left = T_hohlraum;
};

template <>
auto RadSystem<SuOlsonProblemCgs>::ComputeOpacity(const double rho, const double Tgas) -> double
{
	return kappa;
}

template <>
auto RadSystem<SuOlsonProblemCgs>::ComputeTgasFromEgas(const double rho, const double Egas)
    -> double
{
	return std::pow(4.0 * Egas / alpha_SuOlson, 1. / 4.);
}

template <>
auto RadSystem<SuOlsonProblemCgs>::ComputeEgasFromTgas(const double rho, const double Tgas)
    -> double
{
	return (alpha_SuOlson / 4.0) * std::pow(Tgas, 4);
}

template <>
auto RadSystem<SuOlsonProblemCgs>::ComputeEgasTempDerivative(const double rho, const double Tgas)
    -> double
{
	// This is also known as the heat capacity, i.e.
	// 		\del E_g / \del T = \rho c_v,
	// for normal materials.

	// However, for this problem, this must be of the form \alpha T^3
	// in order to obtain an exact solution to the problem.
	// The input parameters are the density and *temperature*, not Egas
	// itself.

	return alpha_SuOlson * std::pow(Tgas, 3);
}

template <>
AMREX_GPU_DEVICE AMREX_FORCE_INLINE void
RadiationSimulation<SuOlsonProblemCgs>::setCustomBoundaryConditions(
    const amrex::IntVect &iv, amrex::Array4<Real> const &consVar, int dcomp, int numcomp,
    amrex::GeometryData const &geom, const Real time, const amrex::BCRec *bcr, int bcomp,
    int orig_comp)
{
	// This should only be called when amrex::BCType::ext_dir is set for a given boundary!
	// if (bcr->lo(0) != amrex::BCType::ext_dir) {
	//	return;
	//}

	// set boundary condition for cell 'iv'
#if (AMREX_SPACEDIM == 2)
	auto [i, j] = iv.toArray();
	int k = 0;
#endif
#if (AMREX_SPACEDIM == 3)
	auto [i, j, k] = iv.toArray();
#endif

	//amrex::Print() << "(" << i << ", " << j << ", " << k << ")\n";

	// Marshak boundary condition
	const double T_H = T_hohlraum;
	const double E_inc = radiation_constant_cgs_ * std::pow(T_H, 4);
	const double c = c_light_cgs_;
	// const double F_inc = c * E_inc / 4.0;

	const double E_0 = consVar(0, j, k, RadSystem<SuOlsonProblemCgs>::radEnergy_index);
	const double F_0 = consVar(0, j, k, RadSystem<SuOlsonProblemCgs>::x1RadFlux_index);

	// const double E_1 = consVar(1, j, k, RadSystem<SuOlsonProblemCgs>::radEnergy_index);
	// const double F_1 = consVar(1, j, k, RadSystem<SuOlsonProblemCgs>::x1RadFlux_index);

	// use PPM stencil at interface to solve for F_rad in the ghost zones
	// const double F_bdry = 0.5 * c * E_inc - (7. / 12.) * (c * E_0 + 2.0 * F_0) +
	//		      (1. / 12.) * (c * E_1 + 2.0 * F_1);

	// use value at interface to solve for F_rad in the ghost zones
	const double F_bdry = 0.5 * c * E_inc - 0.5 * (c * E_0 + 2.0 * F_0);

	AMREX_ASSERT(std::abs(F_bdry / (c * E_inc)) < 1.0);

	if (i < 0) {
		// x1 left side boundary (Marshak)
		consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::radEnergy_index) = E_inc;
		consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::x1RadFlux_index) = F_bdry;
		consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::x2RadFlux_index) = 0.;
		consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::x3RadFlux_index) = 0.;
	} else {
		const double T_initial = 1.0e4; // K
		const double Egas =
		    RadSystem<SuOlsonProblemCgs>::ComputeEgasFromTgas(rho, T_initial);
		const double Erad = a_rad * std::pow(T_initial, 4);
		consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::radEnergy_index) = Erad;
		consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::x1RadFlux_index) = 0;
		consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::x2RadFlux_index) = 0;
		consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::x3RadFlux_index) = 0;
	}
	const double T_initial = 1.0e4; // K
	const double Egas = RadSystem<SuOlsonProblemCgs>::ComputeEgasFromTgas(rho, T_initial);
	consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::gasEnergy_index) = Egas;
	consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::gasDensity_index) = rho;
	consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::x1GasMomentum_index) = 0.;
	consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::x2GasMomentum_index) = 0.;
	consVar(i, j, k, RadSystem<SuOlsonProblemCgs>::x3GasMomentum_index) = 0.;
}

template <> void RadiationSimulation<SuOlsonProblemCgs>::setInitialConditions()
{
	amrex::GpuArray<Real, AMREX_SPACEDIM> dx = simGeometry_.CellSizeArray();
	amrex::GpuArray<Real, AMREX_SPACEDIM> prob_lo = simGeometry_.ProbLoArray();

	for (amrex::MFIter iter(state_old_); iter.isValid(); ++iter) {
		const amrex::Box &indexRange = iter.validbox(); // excludes ghost zones
		auto const &state = state_new_.array(iter);

		amrex::ParallelFor(indexRange, [=] AMREX_GPU_DEVICE(int i, int j, int k) {
			const double T_initial = 1.0e4; // K
			const double Egas =
			    RadSystem<SuOlsonProblemCgs>::ComputeEgasFromTgas(rho, T_initial);
			const double Erad = a_rad * std::pow(T_initial, 4);

			state(i, j, k, RadSystem<SuOlsonProblemCgs>::radEnergy_index) = Erad;
			state(i, j, k, RadSystem<SuOlsonProblemCgs>::x1RadFlux_index) = 0;
			state(i, j, k, RadSystem<SuOlsonProblemCgs>::x2RadFlux_index) = 0;
			state(i, j, k, RadSystem<SuOlsonProblemCgs>::x3RadFlux_index) = 0;

			state(i, j, k, RadSystem<SuOlsonProblemCgs>::gasEnergy_index) = Egas;
			state(i, j, k, RadSystem<SuOlsonProblemCgs>::gasDensity_index) = rho;
			state(i, j, k, RadSystem<SuOlsonProblemCgs>::x1GasMomentum_index) = 0.;
			state(i, j, k, RadSystem<SuOlsonProblemCgs>::x2GasMomentum_index) = 0.;
			state(i, j, k, RadSystem<SuOlsonProblemCgs>::x3GasMomentum_index) = 0.;
		});
	}

	// set flag
	areInitialConditionsDefined_ = true;
}

auto testproblem_radiation_marshak_cgs() -> int
{
	// For this problem, you must do reconstruction in the reduced
	// flux, *not* the flux. Otherwise, F exceeds cE at sharp temperature
	// gradients.

	// Problem parameters

	const int max_timesteps = 10000;
	const double CFL_number = 0.4;
	const int nx = 400;
	// const double initial_dtau = 1e-9; // dimensionless time
	// const double max_dtau = 1e-3;	  // dimensionless time
	const double max_tau = 10.0; // dimensionless time
	const double Lz = 20.0;	     // dimensionless length

	// Su & Olson (1997) parameters
	const double chi = rho * kappa;				   // cm^-1 (total matter opacity)
	const double Lx = Lz / chi;				   // cm
	const double max_time = max_tau / (eps_SuOlson * c * chi); // s
	// const double max_dt = max_dtau / (eps_SuOlson * c * chi);  // s
	// const double initial_dt = initial_dtau / (eps_SuOlson * c * chi); // s

	amrex::IntVect gridDims{AMREX_D_DECL(nx, 4, 4)};
	amrex::RealBox boxSize{
	    {AMREX_D_DECL(amrex::Real(0.0), amrex::Real(0.0), amrex::Real(0.0))},
	    {AMREX_D_DECL(amrex::Real(Lx), amrex::Real(Lx*0.01), amrex::Real(1.0))}};

	constexpr int nvars = 9;
	amrex::Vector<amrex::BCRec> boundaryConditions(nvars);
	for (int n = 0; n < nvars; ++n) {
		for (int i = 1; i < AMREX_SPACEDIM; ++i) {
			boundaryConditions[n].setLo(i, amrex::BCType::int_dir); // periodic
			boundaryConditions[n].setHi(i, amrex::BCType::int_dir);
		}
		boundaryConditions[n].setLo(0, amrex::BCType::ext_dir);	 // custom (Marshak) x1
		boundaryConditions[n].setHi(0, amrex::BCType::ext_dir); // extrapolate x1
	}

	// Problem initialization
	RadiationSimulation<SuOlsonProblemCgs> sim(gridDims, boxSize, boundaryConditions, nvars);
	sim.stopTime_ = max_time;
	sim.cflNumber_ = CFL_number;
	sim.maxTimesteps_ = max_timesteps;
	sim.outputAtInterval_ = true;
	sim.plotfileInterval_ = 100; // for debugging
	sim.componentNames_ = {"radEnergy",	"x-RadFlux",	 "y-RadFlux",
			       "z-RadFlux",	"gasEnergy",	 "gasDensity",
			       "x-GasMomentum", "y-GasMomentum", "z-GasMomentum"};

	// initialize
	sim.setInitialConditions();

	// evolve
	sim.evolve();

	// save solution to vector
	int status = 0;

#if 0
	std::vector<double> xs(nx);
	std::vector<double> Trad(nx);
	std::vector<double> Tgas(nx);
	std::vector<double> Erad(nx);
	std::vector<double> Egas(nx);
	std::vector<double> x1GasMomentum(nx);
	std::vector<double> x1RadFlux(nx);

	for (int i = 0; i < nx; ++i) {
		const double x = Lx * ((i + 0.5) / static_cast<double>(nx));
		xs.at(i) = std::sqrt(3.0) * x;

		const double Erad_t = rad_system.radEnergy(i);
		Erad.at(i) = Erad_t;
		Trad.at(i) = std::pow(Erad_t / a_rad, 1. / 4.);

		const double Etot_t = rad_system.gasEnergy(i);
		const double rho = rad_system.staticGasDensity(i);
		const double x1GasMom = rad_system.x1GasMomentum(i);
		const double Ekin = (x1GasMom * x1GasMom) / (2.0 * rho);

		const double Egas_t = (Etot_t - Ekin);
		Egas.at(i) = Egas_t;
		Tgas.at(i) = rad_system.ComputeTgasFromEgas(rho, Egas_t);

		x1GasMomentum.at(i) = rad_system.x1GasMomentum(i);
		x1RadFlux.at(i) = rad_system.x1RadFlux(i);
	}

	// read in exact solution

	std::vector<double> xs_exact;
	std::vector<double> Trad_exact;
	std::vector<double> Tmat_exact;

	std::string filename = "../extern/SuOlson/100pt_tau10p0.dat";
	std::ifstream fstream(filename, std::ios::in);
	assert(fstream.is_open());

	std::string header;
	std::getline(fstream, header);

	for (std::string line; std::getline(fstream, line);) {
		std::istringstream iss(line);
		std::vector<double> values;

		for (double value; iss >> value;) {
			values.push_back(value);
		}
		auto x_val = std::sqrt(3.0) * values.at(1) / chi;
		auto Trad_val = T_hohlraum * values.at(4);
		auto Tmat_val = T_hohlraum * values.at(5);

		xs_exact.push_back(x_val);
		Trad_exact.push_back(Trad_val);
		Tmat_exact.push_back(Tmat_val);
	}

	// compute error norm

	std::vector<double> Trad_interp(xs_exact.size());
	interpolate_arrays(xs_exact.data(), Trad_interp.data(), xs_exact.size(), xs.data(),
			   Trad.data(), xs.size());

	double err_norm = 0.;
	double sol_norm = 0.;
	const double t = rad_system.time();
	const double xmax = c * t;
	amrex::Print() << "diffusion length = " << xmax << std::endl;
	for (int i = 0; i < xs_exact.size(); ++i) {
		if (xs_exact[i] < xmax) {
			err_norm += std::abs(Trad_interp[i] - Trad_exact[i]);
			sol_norm += std::abs(Trad_exact[i]);
		}
	}

	const double error_tol = 0.015; // 1.5 per cent
	const double rel_error = err_norm / sol_norm;
	amrex::Print() << "Relative L1 error norm = " << rel_error << std::endl;
	if ((rel_error > error_tol) || std::isnan(rel_error)) {
		status = 1;
	}

	// plot results

	// radiation temperature
	std::map<std::string, std::string> Trad_args;
	Trad_args["label"] = "radiation temperature";
	matplotlibcpp::plot(xs, Trad, Trad_args);

	std::map<std::string, std::string> Trad_exact_args;
	Trad_exact_args["label"] = "radiation temperature (exact)";
	matplotlibcpp::plot(xs_exact, Trad_exact, Trad_exact_args);

	matplotlibcpp::xlabel("length x (cm)");
	matplotlibcpp::ylabel("temperature (Kelvins)");
	matplotlibcpp::xlim(0.4 / chi, 100. / chi); // cm
	matplotlibcpp::ylim(T_initial, T_hohlraum); // K
	matplotlibcpp::xscale("log");
	// matplotlibcpp::yscale("log");
	matplotlibcpp::legend();
	matplotlibcpp::title(fmt::format("time t = {:.4g}", rad_system.time()));
	matplotlibcpp::save("./marshak_wave_cgs_temperature.pdf");

	// material temperature
	matplotlibcpp::clf();

	std::map<std::string, std::string> Tgas_args;
	Tgas_args["label"] = "gas temperature";
	matplotlibcpp::plot(xs, Tgas, Tgas_args);

	std::map<std::string, std::string> Tgas_exact_args;
	Tgas_exact_args["label"] = "gas temperature (exact)";
	matplotlibcpp::plot(xs_exact, Tmat_exact, Tgas_exact_args);

	matplotlibcpp::xlabel("length x (cm)");
	matplotlibcpp::ylabel("temperature (Kelvins)");
	matplotlibcpp::xlim(0.4 / chi, 100. / chi); // cm
	matplotlibcpp::ylim(T_initial, T_hohlraum); // K
	matplotlibcpp::xscale("log");
	// matplotlibcpp::yscale("log");
	matplotlibcpp::legend();
	matplotlibcpp::title(fmt::format("time t = {:.4g}", rad_system.time()));
	matplotlibcpp::save("./marshak_wave_cgs_gastemperature.pdf");
#endif

	// Cleanup and exit
	amrex::Print() << "Finished." << std::endl;
	return status;
}
