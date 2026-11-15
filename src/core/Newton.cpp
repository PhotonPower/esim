#include "core/Newton.hpp"

#include <algorithm>
#include <cmath>

namespace esim {

void NewtonSolver::assemble(double a, int n) {
    const size_t ng = ev_.G.size(), total = ng + ev_.C.size();
    auto entry = [&](size_t k) -> const Triplet& { return k < ng ? ev_.G[k] : ev_.C[k - ng]; };

    // Same sequence of entries as last time?
    bool known = key_.size() == total && J_.rows() == n;
    for (size_t k = 0; known && k < total; ++k)
        known = key_[k].first == entry(k).row() && key_[k].second == entry(k).col();

    if (!known) {
        // Build the pattern once (sorting and merging duplicates), then note
        // where every triplet ends up.
        jt_.assign(ev_.G.begin(), ev_.G.end());
        jt_.insert(jt_.end(), ev_.C.begin(), ev_.C.end());
        J_.resize(n, n);
        J_.setFromTriplets(jt_.begin(), jt_.end());
        key_.resize(total);
        slot_.resize(total);
        const int* outer = J_.outerIndexPtr();
        const int* inner = J_.innerIndexPtr();
        for (size_t k = 0; k < total; ++k) {
            const Triplet& e = entry(k);
            key_[k] = {e.row(), e.col()};
            const int* first = inner + outer[e.col()];
            const int* last = inner + outer[e.col() + 1];
            slot_[k] = static_cast<int>(std::lower_bound(first, last, e.row()) - inner);
        }
    }

    double* values = J_.valuePtr();
    std::fill(values, values + J_.nonZeros(), 0.0);
    for (size_t k = 0; k < ng; ++k) values[slot_[k]] += ev_.G[k].value();
    for (size_t k = ng; k < total; ++k) values[slot_[k]] += a * ev_.C[k - ng].value();
}

NewtonResult NewtonSolver::solve(DaeSystem& sys, double t, double a, const Vector* b, Vector& x,
                                 const NewtonOptions& opt) {
    const int n = sys.size();
    NewtonResult res;

    abstol_.resize(n);
    for (int i = 0; i < n; ++i) abstol_[i] = sys.absTol(i);

    bool haveLu = false;   // lin_ holds a Jacobian of this solve
    bool refresh = false;  // it has become too inaccurate
    double previous = 0.0; // size of the last correction, in units of the tolerance

    for (int it = 0; it < opt.maxIter; ++it) {
        const bool limit = it > 0 || opt.limitFromMemory;
        sys.evaluate(t, x, limit ? LimitMode::On : LimitMode::Init, ev_);
        res.iterations = it + 1;

        // Residual r = f + a*q + b and Jacobian J = G + a*C. The C entries are
        // kept even for a == 0 so that the sparsity pattern never changes.
        r_ = ev_.f + a * ev_.q;
        if (b) r_ += *b;

        // Modified Newton: the Jacobian of an earlier iterate is good enough
        // while the corrections keep shrinking fast.
        const bool reuse = opt.reuseJacobian && haveLu && !refresh && !ev_.limited;
        if (!reuse) assemble(a, n);

        if (!r_.allFinite()) {
            res.singular = true;
            return res;
        }

        // Solved to rounding accuracy: every residual is negligible against the
        // terms it is the sum of. A further correction would only be noise. This
        // matters for unknowns that are poorly determined (a section of a circuit
        // that floats against ground): there the noise of the correction can
        // exceed its tolerance, and the test on dx below would never pass.
        if (!ev_.limited) {
            scale_ = (a * ev_.q).cwiseAbs();
            if (b) scale_ += b->cwiseAbs();
            for (int col = 0; col < n; ++col) {
                const double xc = x[col];
                for (SparseMatrix::InnerIterator e(J_, col); e; ++e)
                    scale_[e.row()] += std::abs(e.value() * xc);
            }
            bool solved = true;
            for (int i = 0; i < n && solved; ++i) solved = std::abs(r_[i]) <= 1e-12 * scale_[i];
            if (solved) {
                res.converged = true;
                res.evaluatedAtSolution = true;  // ev_ belongs to the returned x
                return res;
            }
        }

        if (!reuse) {
            if (!lin_.factorize(J_)) {
                res.singular = true;
                return res;
            }
            ++res.factorizations;
            haveLu = true;
            refresh = false;
        }
        dx_ = lin_.solve(-r_);
        if (!dx_.allFinite()) {
            res.singular = true;
            return res;
        }
        x += dx_;

        if (ev_.limited) {  // residual was evaluated at a damped point
            refresh = true;
            continue;
        }

        // Largest correction relative to its tolerance.
        double size = 0.0;
        for (int i = 0; i < n; ++i) {
            const double tol =
                opt.reltol * std::max(std::abs(x[i]), std::abs(x[i] - dx_[i])) + abstol_[i];
            size = std::max(size, std::abs(dx_[i]) / tol);
        }
        // With an old Jacobian the correction says how far the solution still
        // is only if the iteration contracts well.
        const bool slow = reuse && size > 0.5 * previous;
        previous = size;
        if (slow) {
            refresh = true;
            continue;
        }
        if (size <= 1.0) {
            res.converged = true;
            return res;
        }
    }
    return res;
}

}  // namespace esim
