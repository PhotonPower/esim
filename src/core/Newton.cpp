#include "core/Newton.hpp"

#include <algorithm>
#include <atomic>
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
        // for every value of the matrix which triplets it is the sum of.
        jt_.assign(ev_.G.begin(), ev_.G.end());
        jt_.insert(jt_.end(), ev_.C.begin(), ev_.C.end());
        J_.resize(n, n);
        J_.setFromTriplets(jt_.begin(), jt_.end());
        key_.resize(total);
        std::vector<int> slot(total);
        const int* outer = J_.outerIndexPtr();
        const int* inner = J_.innerIndexPtr();
        const auto nnz = static_cast<size_t>(J_.nonZeros());
        start_.assign(nnz + 1, 0);
        for (size_t k = 0; k < total; ++k) {
            const Triplet& e = entry(k);
            key_[k] = {e.row(), e.col()};
            const int* first = inner + outer[e.col()];
            const int* last = inner + outer[e.col() + 1];
            slot[k] = static_cast<int>(std::lower_bound(first, last, e.row()) - inner);
            ++start_[static_cast<size_t>(slot[k]) + 1];
        }
        for (size_t s = 0; s < nnz; ++s) start_[s + 1] += start_[s];
        from_.resize(total);
        std::vector<int> fill(start_.begin(), start_.end() - 1);
        for (size_t k = 0; k < total; ++k) from_[static_cast<size_t>(fill[static_cast<size_t>(slot[k])]++)] = static_cast<int>(k);
    }

    // Every value is the sum of its triplets, taken in the order of stamping.
    // The values are independent of each other, so ranges of them can be
    // filled on different threads with the same result.
    double* values = J_.valuePtr();
    const auto nnz = static_cast<size_t>(J_.nonZeros());
    const Triplet* g = ev_.G.data();
    const Triplet* c = ev_.C.data();
    const auto fill = [&](size_t first, size_t last) {
        for (size_t s = first; s < last; ++s) {
            double sum = 0.0;
            for (int p = start_[s]; p < start_[s + 1]; ++p) {
                const auto k = static_cast<size_t>(from_[static_cast<size_t>(p)]);
                sum += k < ng ? g[k].value() : a * c[k - ng].value();
            }
            values[s] = sum;
        }
    };
    if (pool_ && pool_->parts() > 1 && nnz >= 20000) {
        const size_t chunk = 4096;
        std::atomic<size_t> next{0};
        pool_->run([&](int) {
            for (size_t first; (first = next.fetch_add(chunk, std::memory_order_relaxed)) < nnz;)
                fill(first, std::min(nnz, first + chunk));
        });
    } else {
        fill(0, nnz);
    }
}

NewtonResult NewtonSolver::solve(DaeSystem& sys, double t, double a, const Vector* b, Vector& x,
                                 const NewtonOptions& opt) {
    const int n = sys.size();
    NewtonResult res;

    abstol_.resize(n);
    for (int i = 0; i < n; ++i) abstol_[i] = sys.absTol(i);
    pool_ = sys.workers();

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
