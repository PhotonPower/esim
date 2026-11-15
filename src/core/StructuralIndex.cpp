#include "core/StructuralIndex.hpp"

#include <algorithm>
#include <cmath>

namespace esim {

namespace {

// Bipartite graph of equations and unknowns that grows as equations are
// differentiated (Pantelides 1988, "The consistent initialization of
// differential-algebraic systems").
struct Graph {
    std::vector<std::vector<int>> vars;  // per equation node: its variable nodes
    std::vector<int> derVar;             // variable node -> node of its derivative, -1: none
    std::vector<int> derEq;              // equation node -> node of its derivative, -1: none
    std::vector<int> assign;             // variable node -> equation node, -1: free
    std::vector<char> colourVar, colourEq;

    int newVar() {
        derVar.push_back(-1);
        assign.push_back(-1);
        colourVar.push_back(0);
        return static_cast<int>(derVar.size()) - 1;
    }
    int newEq() {
        vars.emplace_back();
        derEq.push_back(-1);
        colourEq.push_back(0);
        return static_cast<int>(vars.size()) - 1;
    }

    // Look for an alternating path from equation i to a free variable. Only
    // the highest derivative of every variable takes part (derVar == -1).
    // Iterative depth-first search, because paths can be as long as the system.
    bool augment(int start) {
        struct Frame {
            int eq;
            size_t next;
            int via;  // variable through which this equation was reached
        };
        std::vector<Frame> stack{{start, 0, -1}};
        colourEq[start] = 1;
        while (!stack.empty()) {
            const int i = stack.back().eq;
            // A free variable ends the search.
            int freeVar = -1;
            if (stack.back().next == 0)
                for (int j : vars[i])
                    if (derVar[j] < 0 && assign[j] < 0) {
                        freeVar = j;
                        break;
                    }
            if (freeVar >= 0) {
                // Re-assign along the path.
                int j = freeVar;
                for (size_t k = stack.size(); k-- > 0;) {
                    assign[j] = stack[k].eq;
                    j = stack[k].via;
                }
                return true;
            }
            bool descended = false;
            while (stack.back().next < vars[i].size()) {
                const int j = vars[i][stack.back().next++];
                if (derVar[j] >= 0 || colourVar[j]) continue;
                colourVar[j] = 1;
                const int other = assign[j];
                if (other < 0 || colourEq[other]) continue;
                colourEq[other] = 1;
                stack.push_back({other, 0, j});
                descended = true;
                break;
            }
            if (!descended) stack.pop_back();
        }
        return false;
    }
};

}  // namespace

IndexAnalysis structuralIndex(const std::vector<std::vector<int>>& occurs,
                              const std::vector<std::vector<int>>& differentiated) {
    IndexAnalysis result;
    const int n = static_cast<int>(occurs.size());
    result.differentiations.assign(n, 0);
    result.analyzed = true;

    Graph g;
    for (int j = 0; j < n; ++j) g.newVar();
    for (int i = 0; i < n; ++i)
        for (int j : differentiated[i])
            if (g.derVar[j] < 0) g.derVar[j] = g.newVar();
    for (int i = 0; i < n; ++i) {
        const int e = g.newEq();
        for (int j : occurs[i]) g.vars[e].push_back(j);
        for (int j : differentiated[i]) g.vars[e].push_back(g.derVar[j]);
    }

    // More differentiations than unknowns cannot be needed: the system is singular.
    const size_t limit = static_cast<size_t>(n) * 4 + 16;
    for (int k = 0; k < n && !result.singular; ++k) {
        int i = k;
        for (;;) {
            std::fill(g.colourVar.begin(), g.colourVar.end(), 0);
            std::fill(g.colourEq.begin(), g.colourEq.end(), 0);
            if (g.augment(i)) break;
            if (g.vars.size() > limit) {
                result.singular = true;
                break;
            }
            // Differentiate the equations reached, and the variables they are assigned to.
            const int nv = static_cast<int>(g.derVar.size()), ne = static_cast<int>(g.vars.size());
            for (int j = 0; j < nv; ++j)
                if (g.colourVar[j]) g.derVar[j] = g.newVar();
            for (int l = 0; l < ne; ++l) {
                if (!g.colourEq[l]) continue;
                const int d = g.newEq();
                g.derEq[l] = d;
                std::vector<int> list;
                for (int j : g.vars[l]) {
                    list.push_back(j);
                    if (g.derVar[j] >= 0) list.push_back(g.derVar[j]);
                }
                g.vars[d] = std::move(list);
            }
            for (int j = 0; j < nv; ++j)
                if (g.colourVar[j] && g.assign[j] >= 0) g.assign[g.derVar[j]] = g.derEq[g.assign[j]];
            i = g.derEq[i];
        }
    }

    int most = 0;
    bool algebraic = false;
    for (int i = 0; i < n; ++i) {
        for (int e = i; g.derEq[e] >= 0; e = g.derEq[e]) ++result.differentiations[i];
        most = std::max(most, result.differentiations[i]);
        if (differentiated[i].empty()) algebraic = true;
    }
    result.index = most > 0 ? most + 1 : (algebraic ? 1 : 0);
    return result;
}

IndexAnalysis structuralIndex(DaeSystem& sys, const Vector& x, double t) {
    const int n = sys.size();
    if (n > 20000) return {};
    // Move away from special points where a derivative vanishes by chance
    // (x * y at x = 0), with fixed pseudo-random offsets.
    Vector xp = x;
    unsigned seed = 2463534242u;
    for (int i = 0; i < n; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        const double r = 0.5 + (seed >> 8) / 33554432.0;  // 0.5 .. 1
        xp[i] += r * 1e-3 * (std::abs(x[i]) + 1.0);
    }
    Evaluation ev;
    sys.evaluate(t, xp, LimitMode::Off, ev);
    std::vector<std::vector<int>> occurs(n), differentiated(n);
    auto collect = [&](const std::vector<Triplet>& entries, std::vector<std::vector<int>>& lists) {
        for (const Triplet& e : entries)
            if (e.value() != 0.0) lists[e.row()].push_back(e.col());
        for (auto& l : lists) {
            std::sort(l.begin(), l.end());
            l.erase(std::unique(l.begin(), l.end()), l.end());
        }
    };
    collect(ev.G, occurs);
    collect(ev.C, differentiated);
    return structuralIndex(occurs, differentiated);
}

}  // namespace esim
