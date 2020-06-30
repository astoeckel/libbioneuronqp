/*
 *  libbioneuronqp -- Library solving for synaptic weights
 *  Copyright (C) 2020  Andreas Stöckel
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file bioneuron.c
 *
 * Actual implementation of libioneuron.
 *
 * @author Andreas Stöckel
 */

//#define BQP_DEBUG

#include "bioneuronqp.h"

#ifdef BQP_DEBUG
#include <iostream>
#endif

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include "osqp/osqp.h"
#include "threadpool.hpp"

using namespace Eigen;

/******************************************************************************
 * INTERNAL C++ CODE                                                          *
 ******************************************************************************/

using SpMatrixXd = SparseMatrix<double>;
using MatrixMap = Map<Matrix<double, Dynamic, Dynamic, Eigen::RowMajor>>;
using BoolMatrixMap =
    Map<Matrix<unsigned char, Dynamic, Dynamic, Eigen::RowMajor>>;
using BoolVector = Matrix<unsigned char, Dynamic, 1>;

#ifdef BQP_DEBUG
IOFormat CleanFmt(4, 0, ", ", "\n", "[", "]");
#endif

namespace {
class CSCMatrix {
private:
	csc m_csc;

public:
	CSCMatrix(SpMatrixXd &mat)
	{
		mat.makeCompressed();
		m_csc.m = mat.rows();
		m_csc.n = mat.cols();
		m_csc.p = mat.outerIndexPtr();
		m_csc.i = mat.innerIndexPtr();
		m_csc.x = mat.valuePtr();
		m_csc.nzmax = mat.nonZeros();
		m_csc.nz = -1;
	}

	operator csc *() { return &m_csc; }
};

struct QPResult {
	int status = 0;
	double objective_val = 0.0;
	VectorXd x;
};

QPResult _solve_qp(SpMatrixXd &P, VectorXd &q, SpMatrixXd &G, VectorXd &h,
                   double tol, int max_iter)
{
	// Convert the P and G matrix into sparse CSC matrices
	CSCMatrix Pcsc(P), Gcsc(G);

#ifdef BQP_DEBUG
	std::cout << "P =\n" << MatrixXd(P).format(CleanFmt) << std::endl;
	std::cout << "q =\n" << MatrixXd(q).format(CleanFmt) << std::endl;
#endif

	// Generate a lower bound matrix
	VectorXd l =
	    VectorXd::Ones(G.rows()) * std::numeric_limits<c_float>::lowest();

	// Populate data
	OSQPData data;
	data.n = P.rows();
	data.m = G.rows();
	data.P = Pcsc;
	data.q = const_cast<c_float *>(q.data());
	data.A = Gcsc;
	data.l = const_cast<c_float *>(l.data());
	data.u = const_cast<c_float *>(h.data());

	// Define solver settings as default
	OSQPSettings settings;
	osqp_set_default_settings(&settings);
	settings.scaling = 0;
	settings.scaled_termination = 0;
	settings.rho = 1e-1; // Default value
	settings.eps_rel = tol;
	settings.eps_abs = tol;
	settings.polish = true;
	settings.polish_refine_iter = 3; // Default value
	if (max_iter > 0) {
		settings.max_iter = max_iter;
	}

	// Setup workspace
	QPResult res;
	OSQPWorkspace *work = nullptr;
	res.status = osqp_setup(&work, &data, &settings);
	if (res.status != 0) {
		return res;
	}

	// Solve the problem
	res.status = osqp_solve(work);
	if (res.status == 0 && work->info->status_val < 0) {
		res.status = work->info->status_val;
	}
#ifdef BQP_DEBUG
	std::cout << "res.status = " << work->info->status << " res.status_polish = " << work->info->status_polish << std::endl;
#endif

	// Copy the results to the output arrays
	res.x = Map<VectorXd>(work->solution->x, P.rows());
	res.objective_val = work->info->obj_val;

	// Cleanup the workspace
	osqp_cleanup(work);

	return res;
}

template <typename T>
size_t sum(const T &t)
{
	size_t res = 0;
	for (size_t i = 0; i < size_t(t.size()); i++) {
		res += t[i];
	}
	return res;
}

QPResult _solve_weights_qp(const MatrixXd &A, const VectorXd &b,
                           const BoolVector &valid, double i_th, double reg,
                           double tol, int max_iter, bool nonneg)
{
	//
	// Step 1: Count stuff and setup indices used to partition the matrices
	//         into smaller parts.
	//

	// Compute the number of slack variables required to solve this problem
	const size_t n_cstr = A.rows();
	const size_t n_vars = A.cols();
	const size_t n_cstr_valid = sum(valid);
	const size_t n_cstr_invalid = n_cstr - n_cstr_valid;
	const size_t n_slack = n_cstr_invalid;

	// Variables
	const size_t v0 = 0;
	const size_t v1 = v0 + n_vars;
	const size_t v2 = v1 + n_slack;

	// Inequality constraints
	const size_t g0 = 0;
	const size_t g1 = g0 + n_cstr_invalid;
	const size_t g2 = g1 + (nonneg ? n_vars : 0);

	//
	// Step 2: Assemble the QP matrices
	//

	// Compute a dense matrix containing the valid constraints and fill the
	// target vector b
	MatrixXd Avalid(n_cstr_valid, n_vars);
	VectorXd bvalid = VectorXd::Zero(n_cstr_valid);
	size_t i_valid = 0;
	for (size_t i = 0; i < n_cstr; i++) {
		if (valid[i]) {
			for (size_t j = 0; j < n_vars; j++) {
				Avalid(i_valid, v0 + j) = A(i, j);
			}
			bvalid[i_valid] = b[i];
			i_valid++;
		}
	}

	// Compute Avalid.T @ Avalid
	MatrixXd ATAvalid;
	ATAvalid.noalias() = Avalid.transpose() * Avalid;

	// Add the square regularisation term to Avalid
	const double lambda = double(n_cstr) * reg;
	for (size_t i = v0; i < v1; i++) {
		ATAvalid(i, i) += lambda;
	}

	// Compute the sparsity pattern of Aext
	VectorXi Aspp(v2);
	for (size_t i = v0; i < v1; i++) {
		Aspp[i] = i + 1;  // Only copying the upper triangle
	}
	for (size_t i = v1; i < v2; i++) {
		Aspp[i] = 1;
	}

	// Copy ATAvalid to a sparse matrix Aext. Only copy the upper triangle.
	SpMatrixXd Aext(v2, v2);
	Aext.reserve(Aspp);
	for (size_t j = v0; j < v1; j++) {
		for (size_t i = v0; i <= j; i++) {
			Aext.insert(i, j) = ATAvalid(i, j);
		}
	}
	// Penalize slack variables
	for (size_t i = v1; i < v2; i++) {
		Aext.insert(i, i) = 1.0;
	}

	// Compute -Avalid.transpose() * bvalid and store the result in a larger
	// vector bext
	VectorXd bext = VectorXd::Zero(v2);
	bext.block(v0, 0, v1, 1) = -Avalid.transpose() * bvalid;

	// Compute the sparsity pattern of G
	VectorXi Gssp(v2);
	for (size_t i = v0; i < v1; i++) {
		Gssp[i] = n_cstr_invalid + (nonneg ? 1 : 0);
	}
	for (size_t i = v1; i < v2; i++) {
		Gssp[i] = 1;
	}

	// Form the inequality constraints
	SpMatrixXd G(g2, v2);
	G.reserve(Gssp);
	VectorXd h = VectorXd::Zero(g2);
	i_valid = 0;
	for (size_t i = 0; i < n_cstr; i++) {
		if (!valid[i]) {
			for (size_t j = 0; j < n_vars; j++) {
				G.insert(g0 + i_valid, v0 + j) = A(i, j);
			}
			h[i_valid] = i_th;
			i_valid++;
		}
	}
	// Subtract the slack variable from each inequality constraint. This allows
	// the inequality constraints to be violated at the cost of increasing the
	// error.
	for (size_t i = 0; i < n_cstr_invalid; i++) {
		G.insert(g0 + i, v1 + i) = -1.0;
	}

	// Make sure the weights are nonnegative if nonneg is set. Scale the weight
	// a little to make violation of this constraint more costly.
	if (nonneg) {
		for (size_t i = 0; i < n_vars; i++) {
			G.insert(g1 + i, v0 + i) = -100.0;
		}
	}

#ifdef BQP_DEBUG
	std::cout << "Aext = \n" << MatrixXd(Aext).format(CleanFmt) << std::endl;
	std::cout << "bext = \n" << MatrixXd(bext).format(CleanFmt) << std::endl;
	std::cout << "G    = \n" << MatrixXd(G).format(CleanFmt) << std::endl;
	std::cout << "h    = \n" << MatrixXd(h).format(CleanFmt) << std::endl;
#endif

	//
	// Step 3: Sovle the QP
	//
	QPResult res = _solve_qp(Aext, bext, G, h, tol, max_iter);

	//
	// Step 4: Post-processing
	//

	// Strictly enforce weight non-negativity
	if (nonneg) {
		for (size_t i = v0; i < v1; i++) {
			res.x[i] = std::max(0.0, res.x[i]);
		}
	}
	return res;
}

void _bioneuronqp_solve_single(BioneuronWeightProblem *problem,
                               BioneuronSolverParameters *params, size_t j)
{
	// Copy some input parameters as convenient aliases
	size_t Npre = problem->n_pre;
	size_t Nsamples = problem->n_samples;

	// Copy some relevant input matrices
	MatrixMap APre(problem->a_pre, problem->n_samples, problem->n_pre);

	MatrixMap JPost(problem->j_post, problem->n_samples, problem->n_post);

	MatrixMap Ws(problem->model_weights, problem->n_post, 6);
	Matrix<double, 6, 1> ws = Ws.row(j);

	BoolMatrixMap ConExc(problem->connection_matrix_exc, problem->n_pre,
	                     problem->n_post);
	BoolMatrixMap ConInh(problem->connection_matrix_inh, problem->n_pre,
	                     problem->n_post);

	// Fetch the output weights
	MatrixMap WExc(problem->synaptic_weights_exc, problem->n_pre,
	               problem->n_post);
	MatrixMap WInh(problem->synaptic_weights_inh, problem->n_pre,
	               problem->n_post);

	// Count the number of excitatory and inhibitory pre-neurons; also reset the
	// output weights for this post neuron
	size_t Npre_exc = 0, Npre_inh = 0;
	for (size_t i = 0; i < Npre; i++) {
		if (ConExc(i, j)) {
			Npre_exc++;
		}
		if (ConInh(i, j)) {
			Npre_inh++;
		}
		WExc(i, j) = 0.0;
		WInh(i, j) = 0.0;
	}

	// Compute the total number of pre neurons. We're done if there are no pre
	// neurons.
	const size_t Npre_tot = Npre_exc + Npre_inh;
	if (Npre_tot == 0) {
		return;
	}

	// Renormalise the target currents
	double Wscale = 1.0, LambdaScale = 1.0;
	if (params->renormalise) {
		// Need to scale the regularisation factor as well
		LambdaScale = 1.0 / (ws[1] * ws[1]);

		// Compute synaptic weights in nS
		Wscale = 1e-9;
		ws[1] *= Wscale;
		ws[2] *= Wscale;
		ws[4] *= Wscale;
		ws[5] *= Wscale;

		// Set ws[1]=1 for better numerical stability/conditioning
		ws /= ws[1];
	}

	// Demangle the weight vector
	double a0 = ws[0], a1 = ws[1], a2 = ws[2], b0 = ws[3], b1 = ws[4],
	       b2 = ws[5];

	// Warn if some weights are out of range
	if (params->warn && std::abs(b2) > 0.0 && std::abs(b1) > 0.0) {
		const double jPostMax = JPost.col(j).array().maxCoeff();
		if ((a1 / b1) < jPostMax) {
			std::stringstream ss;
			ss << "Target currents for neuron " << j << " cannot be reached! "
			   << jPostMax << " ∉ [" << (a2 / b2) << ", " << (a1 / b1) << "]";
			if (params->warn) {
				params->warn(ss.str().c_str(), j);
			}
		}
	}

	// Assemble the "A" matrix for the least squares problem
	MatrixXd A(Nsamples, Npre_tot);
	size_t i_pre_exc = 0, i_pre_inh = Npre_exc;
	for (size_t i = 0; i < Npre; i++) {
		if (ConExc(i, j)) {
			for (size_t k = 0; k < Nsamples; k++) {
				A(k, i_pre_exc) = (a1 - b1 * JPost(k, j)) * APre(k, i);
			}
			i_pre_exc++;
		}
		if (ConInh(i, j)) {
			for (size_t k = 0; k < Nsamples; k++) {
				A(k, i_pre_inh) = (a2 - b2 * JPost(k, j)) * APre(k, i);
			}
			i_pre_inh++;
		}
	}

	// Assemble the "b" matrix for the least squares problem
	VectorXd b = JPost.col(j).array() * b0 - a0;

	// Determine which target currents are valid and which target currents are
	// not
	BoolVector valid = BoolVector::Ones(Nsamples);
	if (problem->relax_subthreshold) {
		for (size_t i = 0; i < Nsamples; i++) {
			if (JPost(i, j) < problem->j_threshold) {
				valid(i) = 0;
			}
		}
	}

	// Solve the quadratic programing problem
	const double i_th = problem->j_threshold * b0 - a0;
	const double reg = problem->regularisation * LambdaScale;
	const bool nonneg = problem->non_negative;
	const double tol = params->tolerance;
	const int max_iter = params->max_iter;
	QPResult res =
	    _solve_weights_qp(A, b, valid, i_th, reg, tol, max_iter, nonneg);
#ifdef BQP_DEBUG
	std::cout << "x = \n" << res.x.format(CleanFmt) << std::endl;
	std::cout << "status = " << res.status << std::endl;
#endif
	if (res.status != 0) {
		std::stringstream ss;
		ss << "Error while computing weights for post-neuron " << j << ". ";
		switch (res.status) {
			case OSQP_DATA_VALIDATION_ERROR:
				ss << "Data valiation error.";
				break;
			case OSQP_SETTINGS_VALIDATION_ERROR:
				ss << "Settings validation error.";
				break;
			case OSQP_LINSYS_SOLVER_INIT_ERROR:
			case OSQP_LINSYS_SOLVER_LOAD_ERROR:
				ss << "Cannot load linear solver.";
				break;
			case OSQP_MEM_ALLOC_ERROR:
				ss << "Cannot allocated workspace memory.";
				break;
#ifdef OSQP_TIME_LIMIT_REACHED
			case OSQP_TIME_LIMIT_REACHED:
				ss << "Time limit reached.";
				break;
#endif /* OSQP_TIME_LIMIT_REACHED */
			case OSQP_MAX_ITER_REACHED:
				ss << "Maximum numbers of iterations reached.";
				break;
			case OSQP_PRIMAL_INFEASIBLE:
				ss << "Primal infeasible.";
				break;
			case OSQP_DUAL_INFEASIBLE:
				ss << "Dual infeasible.";
				break;
			case OSQP_SIGINT:
				ss << "Interrupted by user.";
				break;
			case OSQP_UNSOLVED:
				ss << "Unsolved.";
				break;
			case OSQP_NONCVX_ERROR:
			case OSQP_NON_CVX:
				ss << "Problem not convex.";
				break;
			default:
				ss << "Unknown/unhandled error.";
				break;
		}
		if (params->warn) {
			params->warn(ss.str().c_str(), j);
		}
	}

	// Distribute the resulting weights back to their correct locations
	if (size_t(res.x.size()) >= Npre_tot) {
		i_pre_exc = 0, i_pre_inh = Npre_exc;
		for (size_t i = 0; i < Npre; i++) {
			if (ConExc(i, j)) {
				WExc(i, j) = res.x[i_pre_exc++] * Wscale;
			}
			if (ConInh(i, j)) {
				WInh(i, j) = res.x[i_pre_inh++] * Wscale;
			}
		}
	}

	// Store the objective values
	if (problem->objective_vals) {
		problem->objective_vals[j] = res.objective_val;
	}
}

BioneuronError _bioneuronqp_solve(BioneuronWeightProblem *problem,
                                  BioneuronSolverParameters *params)
{
	// Construct the kernal that is being executed -- here, we're solving the
	// weights for a single post-neuron.
	auto kernel =
	    [&](size_t idx) { _bioneuronqp_solve_single(problem, params, idx); };

	// Construct the progress callback
	bool did_cancel = false;
	auto progress = [&](size_t cur, size_t max) {
		if (params->progress) {
			if (!params->progress(cur, max)) {
				did_cancel = true;
			}
			return !did_cancel;
		}
		return true;
	};

	// Create a threadpool and solve the weights for all neurons. Do not create
	// a threadpool if there is only one set of weights to solve for.
	if (problem->n_post > 1) {
		Threadpool pool(params->n_threads);
		pool.run(problem->n_post, kernel, progress);
	} else if (problem->n_post > 0) {
		kernel(0);
	}

	return did_cancel ? BN_ERR_CANCEL : BN_ERR_OK;
}

}  // namespace

/******************************************************************************
 * EXTERNAL C API                                                             *
 ******************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************
 * Enum BioneuronError                                                        *
 ******************************************************************************/

const char *bioneuronqp_strerr(BioneuronError err)
{
	switch (err) {
		case BN_ERR_OK:
			return "no error";
		case BN_ERR_INVALID_N_PRE:
			return "n_pre is invalid";
		case BN_ERR_INVALID_N_POST:
			return "n_post is invalid";
		case BN_ERR_INVALID_N_SAMPLES:
			return "n_samples is invalid";
		case BN_ERR_INVALID_A_PRE:
			return "a_pre is invalid";
		case BN_ERR_INVALID_J_POST:
			return "j_post is invalid";
		case BN_ERR_INVALID_MODEL_WEIGHTS:
			return "model_weights is invalid";
		case BN_ERR_INVALID_CONNECTION_MATRIX_EXC:
			return "connection_matrix_exc is invalid";
		case BN_ERR_INVALID_CONNECTION_MATRIX_INH:
			return "connection_matrix_inh is invalid";
		case BN_ERR_INVALID_REGULARISATION:
			return "regularisation is invalid";
		case BN_ERR_INVALID_SYNAPTIC_WEIGHTS_EXC:
			return "synaptic_weights_exc is invalid";
		case BN_ERR_INVALID_SYNAPTIC_WEIGHTS_INH:
			return "synaptic_weights_inh is invalid";
		case BN_ERR_INVALID_TOLERANCE:
			return "tolerance is invalid";
		case BN_ERR_CANCEL:
			return "canceled by user";
	}
	return "unknown error code";
}

/******************************************************************************
 * Struct BioneuronWeightProblem                                              *
 ******************************************************************************/

#define DEFAULT_REGULARISATION 1e-1
#define DEFAULT_TOLERANCE 1e-6;

void bioneuronqp_weight_problem_init(BioneuronWeightProblem *problem)
{
	problem->n_pre = 0;
	problem->n_post = 0;
	problem->n_samples = 0;
	problem->a_pre = nullptr;
	problem->j_post = nullptr;
	problem->model_weights = nullptr;
	problem->connection_matrix_exc = nullptr;
	problem->connection_matrix_inh = nullptr;
	problem->regularisation = DEFAULT_REGULARISATION;
	problem->j_threshold = 0.0;
	problem->relax_subthreshold = 0;
	problem->non_negative = 0;
	problem->synaptic_weights_exc = 0;
	problem->synaptic_weights_inh = 0;
}

/******************************************************************************
 * Struct BioneuronSolverParameters                                           *
 ******************************************************************************/

void bioneuronqp_solver_parameters_init(BioneuronSolverParameters *params)
{
	params->renormalise = 1;
	params->tolerance = DEFAULT_TOLERANCE;
	params->progress = nullptr;
	params->warn = nullptr;
	params->n_threads = 0;
}

/******************************************************************************
 * Actual solver code                                                         *
 ******************************************************************************/

static BioneuronError _check_problem_is_valid(BioneuronWeightProblem *problem)
{
	if (problem->n_pre <= 0) {
		return BN_ERR_INVALID_N_PRE;
	}
	if (problem->n_post <= 0) {
		return BN_ERR_INVALID_N_PRE;
	}
	if (problem->n_samples <= 0) {
		return BN_ERR_INVALID_N_SAMPLES;
	}
	if (problem->a_pre == nullptr) {
		return BN_ERR_INVALID_A_PRE;
	}
	if (problem->j_post == nullptr) {
		return BN_ERR_INVALID_J_POST;
	}
	if (problem->model_weights == nullptr) {
		return BN_ERR_INVALID_MODEL_WEIGHTS;
	}
	if (problem->connection_matrix_exc == nullptr) {
		return BN_ERR_INVALID_CONNECTION_MATRIX_EXC;
	}
	if (problem->connection_matrix_inh == nullptr) {
		return BN_ERR_INVALID_CONNECTION_MATRIX_INH;
	}
	if (problem->regularisation < 0.0) {
		return BN_ERR_INVALID_REGULARISATION;
	}
	if (problem->synaptic_weights_exc == nullptr) {
		return BN_ERR_INVALID_SYNAPTIC_WEIGHTS_EXC;
	}
	if (problem->synaptic_weights_inh == nullptr) {
		return BN_ERR_INVALID_SYNAPTIC_WEIGHTS_INH;
	}
	return BN_ERR_OK;
}

static BioneuronError _check_parameters_is_valid(
    BioneuronSolverParameters *params)
{
	if (params->tolerance <= 0.0) {
		return BN_ERR_INVALID_TOLERANCE;
	}
	return BN_ERR_OK;
}

BioneuronError bioneuronqp_solve(BioneuronWeightProblem *problem,
                                 BioneuronSolverParameters *params)
{
	// Make sure the given pointers point at valid problem and parameter
	// descriptors
	BioneuronError err;
	if ((err = _check_problem_is_valid(problem)) < 0) {
		return err;
	}
	if ((err = _check_parameters_is_valid(params)) < 0) {
		return err;
	}

	// Forward the parameters and the problem description to the internal C++
	// code.
	return _bioneuronqp_solve(problem, params);
}

#ifdef __cplusplus
}
#endif
