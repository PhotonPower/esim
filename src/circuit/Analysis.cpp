#include "circuit/Analysis.hpp"

#include <Eigen/SparseLU>
#include <cmath>
#include <sstream>
#include <stdexcept>

#include "circuit/Device.hpp"

namespace esim {

Vector solveOperatingPoint(Circuit& ckt, const SimOptions& opt, const Vector* guess,
                           DcResult* info) {
    Vector x = guess ? *guess : ckt.initialGuess();
    DcOptions dc;
    dc.newton.maxIter = opt.maxIterDc;
    dc.newton.reltol = opt.reltol;
    const DcResult r = solveDc(ckt, 0.0, x, dc);
    if (info) *info = r;
    if (!r.converged)
        throw std::runtime_error(
            "DC operating point did not converge (check for floating nodes, loops of voltage "
            "sources/inductors, or try a nodeset)");
    return x;
}

std::vector<double> frequencySweep(const std::string& kind, int points, double fstart,
                                   double fstop) {
    if (!(fstart > 0.0) || !(fstop >= fstart))
        throw std::runtime_error("ac: need 0 < fstart <= fstop");
    if (points < 1) throw std::runtime_error("ac: points must be >= 1");

    std::vector<double> f;
    if (kind == "lin") {
        if (points == 1) return {fstart};
        for (int k = 0; k < points; ++k) f.push_back(fstart + k * (fstop - fstart) / (points - 1));
        return f;
    }
    double base;
    if (kind == "dec") base = 10.0;
    else if (kind == "oct") base = 2.0;
    else throw std::runtime_error("ac: sweep must be \"dec\", \"oct\" or \"lin\"");

    const int total =
        static_cast<int>(std::floor(std::log(fstop / fstart) / std::log(base) * points + 1e-9)) + 1;
    for (int k = 0; k < total; ++k) f.push_back(fstart * std::pow(base, double(k) / points));
    return f;
}

void runAc(Circuit& ckt, const Vector& xop, const std::vector<double>& freqs,
           const AcCallback& onPoint) {
    using cd = std::complex<double>;
    const int n = ckt.size();

    Evaluation ev;
    ckt.evaluate(0.0, xop, LimitMode::Off, ev);

    Eigen::VectorXcd rhs = Eigen::VectorXcd::Zero(n);
    for (const auto& d : ckt.devices()) d->stampAc(rhs);

    std::vector<Eigen::Triplet<cd>> trip;
    trip.reserve(ev.G.size() + ev.C.size());
    Eigen::SparseMatrix<cd> A(n, n);
    Eigen::SparseLU<Eigen::SparseMatrix<cd>, Eigen::COLAMDOrdering<int>> lu;

    bool analyzed = false;
    for (double f : freqs) {
        const double w = 2.0 * kPi * f;
        trip.clear();
        for (const Triplet& g : ev.G) trip.emplace_back(g.row(), g.col(), cd(g.value(), 0.0));
        for (const Triplet& c : ev.C) trip.emplace_back(c.row(), c.col(), cd(0.0, w * c.value()));
        A.setFromTriplets(trip.begin(), trip.end());
        if (!analyzed) {
            lu.analyzePattern(A);
            analyzed = true;
        }
        lu.factorize(A);
        if (lu.info() != Eigen::Success) {
            std::ostringstream os;
            os << "ac: singular matrix at f = " << f << " Hz";
            throw std::runtime_error(os.str());
        }
        const Eigen::VectorXcd x = lu.solve(rhs);
        onPoint(f, x);
    }
}

}  // namespace esim
