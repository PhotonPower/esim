#include "core/LinearSolver.hpp"

#include <algorithm>

namespace esim {

bool LinearSolver::samePattern(const SparseMatrix& A) const {
    const auto nOuter = static_cast<size_t>(A.outerSize()) + 1;
    const auto nInner = static_cast<size_t>(A.nonZeros());
    return outer_.size() == nOuter && inner_.size() == nInner &&
           std::equal(outer_.begin(), outer_.end(), A.outerIndexPtr()) &&
           std::equal(inner_.begin(), inner_.end(), A.innerIndexPtr());
}

bool LinearSolver::factorize(const SparseMatrix& A) {
    const double* values = A.valuePtr();
    const auto nnz = static_cast<size_t>(A.nonZeros());
    if (!samePattern(A)) {
        outer_.assign(A.outerIndexPtr(), A.outerIndexPtr() + A.outerSize() + 1);
        inner_.assign(A.innerIndexPtr(), A.innerIndexPtr() + A.nonZeros());
        lu_.analyzePattern(A);
    } else if (factorized_ && std::equal(values_.begin(), values_.end(), values)) {
        return true;  // the same matrix as last time
    }
    lu_.factorize(A);
    factorized_ = lu_.info() == Eigen::Success;
    if (factorized_) values_.assign(values, values + nnz);
    return factorized_;
}

}  // namespace esim
