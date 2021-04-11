/**
 * @file balance_controller.cpp
 * @date 2021-03-04
 * @author Boston Cleek
 * @brief Force balance controller
 */

#include <ros/console.h>
#include <quadruped_controller/balance_controller.hpp>

/*
References:
  [R1] M. Focchi, A. del Prete, I. Havoutis, R. Featherstone, D. G. Caldwell,
      and C. Semini. High-slope terrain locomotion for torque-controlled quadruped
      robots. Autonomous Robots, 2016.
*/

namespace quadruped_controller
{
static const std::string LOGNAME = "Balance Controller";

void copy_to_real_t(const vec& source, real_t* target)
{
  for (unsigned int i = 0; i < source.size(); i++)
  {
    target[i] = source(i);
  }
}

void copy_to_real_t(const mat& source, real_t* target)
{
  unsigned int count = 0;
  for (unsigned int i = 0; i < source.n_rows; i++)
  {
    for (unsigned int j = 0; j < source.n_cols; j++)
    {
      target[count] = source(i, j);
      count++;
    }
  }
}

vec copy_from_real_t(const real_t* const source, unsigned int n_rows)
{
  vec target(n_rows);
  for (unsigned int i = 0; i < n_rows; i++)
  {
    target(i) = source[i];
  }

  return target;
}

void print_real_t(const real_t* const array, unsigned int n_rows, unsigned int n_cols,
                  const std::string& msg)
{
  std::cout << msg << "\n";
  unsigned int count = 0;
  for (unsigned int i = 0; i < n_rows; i++)
  {
    for (unsigned int j = 0; j < n_cols; j++)
    {
      std::cout << array[count] << " ";
      count++;
    }
    std::cout << std::endl;
  }
}

BalanceController::BalanceController(double mu, double mass, double fzmin, double fzmax,
                                     const mat& Ib, const mat& S, const mat& W,
                                     const vec& kff, const vec& kp_p, const vec& kd_p,
                                     const vec& kp_w, const vec& kd_w, 
                                     const std::vector<std::string>& leg_names)
  : mu_(mu)
  , mass_(mass)
  , Ib_(Ib)
  , g_({ 0.0, 0.0, -9.81 })
  , kff_(kff)
  , kp_p_(kp_p)
  , kd_p_(kd_p)
  , kp_w_(kp_w)
  , kd_w_(kd_w)
  , QPSolver_(num_variables_qp_, num_constraints_qp_)
  , nWSR_(200)
  , fzmin_(fzmin)
  , fzmax_(fzmax)
  , S_(S)
  , W_(W)
  , C_(frictionConeConstraint())
  , cpu_time_(0.001)
  , leg_names_(leg_names)
{
  // Disable printing
  QPSolver_.setPrintLevel(qpOASES::PL_NONE);
}

vec BalanceController::control(const mat& ft_p, const mat& Rwb, const mat& Rwb_d,
                               const vec& x, const vec& xdot, const vec& w,
                               const vec& x_d, const vec& xdot_d, const vec& w_d, const GaitMap& gait_map) const
{
  // TODO: return previouse solution if there is a failure

  frictionConeBounds(gait_map);

  // IMPORTANT: Ground reaction forces from QP solver are in world frame
  vec fw(num_variables_qp_, arma::fill::zeros);

  // PD control on COM position and orientation
  // [R1] Eq(3)
  vec xddot_d = kp_p_ % (x_d - x) + kd_p_ % (xdot_d - xdot);
  xddot_d(0) += kff_(0) * xdot_d(0);
  xddot_d(1) += kff_(1) * xdot_d(1);
  xddot_d(2) += kff_(2) * mass_ * 9.81;
  // xddot_d.print("xddot_d");

  // [R1] Eq(4)
  const Rotation3d R_error(Rwb_d * Rwb.t());

  // TODO: verify that angleAxisTotal() should be used here
  vec wdot_d = kp_w_ % R_error.angleAxisTotal() + kd_w_ % (w_d - w);
  wdot_d += kff_.rows(3, 5) % w_d;
  // wdot_d.print("wdot_d");

  // [R1] Eq(5) Linear Newton-Euler single rigid body dynamics
  const auto srb_dyn = dynamics(ft_p, Rwb, x, xddot_d, wdot_d);
  const mat A_dyn = std::get<mat>(srb_dyn);
  const vec b_dyn = std::get<vec>(srb_dyn);

  // TODO: Add regularization term Eq(6)
  // [R1] Convert Eq(6) to QP standard form 1/2*x.T*Q*x + x.T*c
  // Q = 2*(A.T*S*A + W) (12x12)
  // c = -2*A.T*S*b (12x1)
  const mat Q = 2.0 * (A_dyn.t() * S_ * A_dyn + W_);
  const vec c = -2.0 * A_dyn.t() * S_ * b_dyn;

  copy_to_real_t(Q, qp_Q_);
  copy_to_real_t(c, qp_c_);

  // No lower/upper bound constraints on GRFs because
  // the constraint matrix, C, takes care of this.
  real_t* qp_lb = nullptr;
  real_t* qp_ub = nullptr;

  // Primal solution
  real_t qp_xOpt[num_variables_qp_];

  // Will update based on actual
  int nWSR_acutal = nWSR_;
  real_t cpu_time_actual = cpu_time_;

  // TODO: See Init Homotopy pg 31
  // TODO: Add initial guess to hotstart
  if (!QPSolver_.isInitialised())
  {
    const returnValue ret_val = QPSolver_.init(qp_Q_, qp_c_, qp_C_, qp_lb, qp_ub, qp_lbC_,
                                               qp_ubC_, nWSR_acutal, &cpu_time_actual);

    if (ret_val != qpOASES::SUCCESSFUL_RETURN)
    {
      ROS_ERROR_STREAM_NAMED(LOGNAME,
                             "Failed to initialize Balance Controller QP Solver");
      return fw;
    }
  }

  else
  {
    const returnValue ret_val =
        QPSolver_.hotstart(qp_Q_, qp_c_, qp_C_, qp_lb, qp_ub, qp_lbC_, qp_ubC_,
                           nWSR_acutal, &cpu_time_actual);

    if (ret_val != qpOASES::SUCCESSFUL_RETURN)
    {
      ROS_ERROR_STREAM_NAMED(LOGNAME, "Failed to hotstart Balance Controller QP Solver");
      return fw;
    }
  }

  // std::cout << "CPU time: " << cpu_time_actual << " (s)" << std::endl;

  if (QPSolver_.isSolved())
  {
    QPSolver_.getPrimalSolution(qp_xOpt);
    fw = copy_from_real_t(qp_xOpt, num_variables_qp_);
  }

  else
  {
    ROS_ERROR_STREAM_NAMED(LOGNAME, "Balance Controller QP Solver Failed");
    return fw;
  }

  // Negate force directions and transform into body frame
  const mat Rbw = Rwb.t();
  vec fb(num_variables_qp_);

  fb.rows(0, 2) = Rbw * fw.rows(0, 2);
  fb.rows(3, 5) = Rbw * fw.rows(3, 5);
  fb.rows(6, 8) = Rbw * fw.rows(6, 8);
  fb.rows(9, 11) = Rbw * fw.rows(9, 11);

  return -1.0 * fb;
}

tuple<mat, vec> BalanceController::dynamics(const mat& ft_p, const mat& Rwb, const vec& x,
                                            const vec& xddot_d, const vec& wdot_d) const
{
  // TODO: verify this is correct the feet are already in the body frame
  // Vector from COM to each foot position in world frame
  // pcom,i = ft_p - x_com = (Rwb * ft_p + x_com) - x_com = Rwb * ft_p
  mat com_ft_p(3, 4);
  com_ft_p.col(0) = Rwb * ft_p.col(0);
  com_ft_p.col(1) = Rwb * ft_p.col(1);
  com_ft_p.col(2) = Rwb * ft_p.col(2);
  com_ft_p.col(3) = Rwb * ft_p.col(3);

  // Moment of Interia in world frame
  const mat Iw = Rwb * Ib_ * Rwb.t();

  mat A(num_equations_qp_, num_variables_qp_, arma::fill::zeros);
  A.submat(0, 0, 2, 2) = eye(3, 3);
  A.submat(0, 3, 2, 5) = eye(3, 3);
  A.submat(0, 6, 2, 8) = eye(3, 3);
  A.submat(0, 9, 2, 11) = eye(3, 3);

  A.submat(3, 0, 5, 2) = skew_symmetric(com_ft_p.col(0));
  A.submat(3, 3, 5, 5) = skew_symmetric(com_ft_p.col(1));
  A.submat(3, 6, 5, 8) = skew_symmetric(com_ft_p.col(2));
  A.submat(3, 9, 5, 11) = skew_symmetric(com_ft_p.col(3));

  vec b(num_equations_qp_, arma::fill::zeros);
  b.rows(0, 2) = mass_ * (xddot_d + g_);
  b.rows(3, 5) = Iw * wdot_d;

  return std::make_tuple(A, b);
}


mat BalanceController::frictionConeConstraint() const 
{
  // [R1] Eq(7) and Eq(8)
  // Friction cone constraint per foot
  const mat Cf = { { 1.0, 0.0, -mu_ },
                   { 0.0, 1.0, -mu_ },
                   { 0.0, 1.0, mu_ },
                   { 1.0, 0.0, mu_ },
                   { 0.0, 0.0, 1.0 } };

  // Constraint matrix
  mat C(num_constraints_qp_, num_variables_qp_, arma::fill::zeros);
  C.submat(0, 0, 4, 2) = Cf;
  C.submat(5, 3, 9, 5) = Cf;
  C.submat(10, 6, 14, 8) = Cf;
  C.submat(15, 9, 19, 11) = Cf;

  return C;
}

void BalanceController::frictionConeBounds(const GaitMap& gait_map) const
{
  const auto upper = 1000000.0;
  const auto lower = -1000000.0;

  // Friction cone lower and upper limits per foot
  const vec lbf = { lower, lower, 0.0, 0.0, fzmin_ };
  const vec ubf = { 0.0, 0.0, upper, upper, fzmax_ };

  // Lower and upper bounds on constraint matrix
  vec lbC(num_constraints_qp_);
  vec ubC(num_constraints_qp_);

  unsigned int row_start = 0;
  unsigned int row_end = 4;

  for (const auto& leg_name : leg_names_)
  {
    if (gait_map.at(leg_name).first == LegState::swing)
    {
      lbC.rows(row_start, row_end).zeros();
      ubC.rows(row_start, row_end).zeros();
    }
    else 
    {
      lbC.rows(row_start, row_end) = lbf;
      ubC.rows(row_start, row_end) = ubf;
    }

    row_start += 5;
    row_end += 5;
  }

  // lbC.rows(0, 4) = lbf;
  // lbC.rows(5, 9) = lbf;
  // lbC.rows(10, 14) = lbf;
  // lbC.rows(15, 19) = lbf;

  // ubC.rows(0, 4) = ubf;
  // ubC.rows(5, 9) = ubf;
  // ubC.rows(10, 14) = ubf;
  // ubC.rows(15, 19) = ubf;

  copy_to_real_t(C_, qp_C_);
  copy_to_real_t(lbC, qp_lbC_);
  copy_to_real_t(ubC, qp_ubC_);
}
}  // namespace quadruped_controller
