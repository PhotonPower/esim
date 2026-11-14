#pragma once

#include <Eigen/Core>
#include <Eigen/SparseCore>
#include <vector>

namespace esim {

using Vector = Eigen::VectorXd;
using SparseMatrix = Eigen::SparseMatrix<double>;
using Triplet = Eigen::Triplet<double>;
using Triplets = std::vector<Triplet>;

constexpr double kPi = 3.14159265358979323846;

}  // namespace esim
