#pragma once

#include <Eigen/SparseLU>
#include <vector>

#include "core/Types.hpp"

namespace esim {

// Sparse LU with partial pivoting. The symbolic analysis is reused as long as
// the sparsity pattern stays the same, and the factorization itself as long as
// the matrix is identical (a linear system stepped with a constant step size).
class LinearSolver {
public:
    // Returns false if the matrix is singular.
    bool factorize(const SparseMatrix& A);
    Vector solve(const Vector& rhs) const { return lu_.solve(rhs); }

private:
    bool samePattern(const SparseMatrix& A) const;

    Eigen::SparseLU<SparseMatrix, Eigen::COLAMDOrdering<int>> lu_;
    std::vector<int> outer_, inner_;
    std::vector<double> values_;  // of the matrix that is currently factorized
    bool factorized_ = false;
};

}  // namespace esim
