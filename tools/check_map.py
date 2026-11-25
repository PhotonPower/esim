#!/usr/bin/env python3
"""Check a field map (flux linkage and force over current and position) before it
goes into an esim table.

    python3 tools/check_map.py examples/data/hubmagnet_map.csv
    python3 tools/check_map.py map.csv --x i --y g --psi psi --force F --tol 5

The file has one point per row and covers the whole grid of x (current) and y
(position or air gap). The checks:

    grid         every combination of x and y exactly once, both axes increasing
    zero         psi = 0 and force = 0 at x = 0
    symmetry     with negative currents: psi odd, force even in x
    monotone     psi increases with x (for x >= 0)
    energy       the force follows from the flux linkage: W'(x, y) = integral of
                 psi dx from 0, F = -dW'/dy (y is an air gap; the derivative
                 comes from a spline in log-log axes). Compared with the force
                 column; this is the consistency that Newton's method and the
                 energy balance of the coupled circuit rely on

Needs numpy and scipy. The exit status is 1 if a check fails.
"""

import argparse
import csv
import sys

import numpy as np
from scipy.interpolate import CubicSpline, PchipInterpolator


def read(path, names):
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    head = [h.strip() for h in rows[0]]
    for n in names:
        if n not in head:
            sys.exit(f"{path}: no column '{n}' (columns: {', '.join(head)})")
    col = {n: head.index(n) for n in names}
    data = {n: [] for n in names}
    for r in rows[1:]:
        if not r or not r[0].strip():
            continue
        for n in names:
            data[n].append(float(r[col[n]]))
    return {n: np.array(v) for n, v in data.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--x", default="i", help="column of the current (default i)")
    ap.add_argument("--y", default="g", help="column of the position / air gap (default g)")
    ap.add_argument("--psi", default="psi", help="column of the flux linkage (default psi)")
    ap.add_argument("--force", default="F", help="column of the force (default F)")
    ap.add_argument("--tol", type=float, default=5.0, help="allowed deviation of the force in percent (default 5)")
    ap.add_argument("--xmin", type=float, default=None,
                    help="smallest current of the force comparison (default: 10%% of the largest)")
    a = ap.parse_args()

    d = read(a.file, [a.x, a.y, a.psi, a.force])
    x, y, psi, force = d[a.x], d[a.y], d[a.psi], d[a.force]
    failed = False

    def report(ok, text):
        nonlocal failed
        print(("ok    " if ok else "FAIL  ") + text)
        failed = failed or not ok

    xs, ys = np.unique(x), np.unique(y)
    pairs = set(zip(x.tolist(), y.tolist()))
    report(len(pairs) == len(x), f"no point appears twice ({len(x)} rows)")
    report(len(pairs) == len(xs) * len(ys),
           f"grid is complete: {len(xs)} x {len(ys)} = {len(xs) * len(ys)} points, {len(pairs)} given")
    if failed:
        sys.exit(1)
    # z[i, j] at (xs[i], ys[j])
    ix = np.searchsorted(xs, x)
    iy = np.searchsorted(ys, y)
    P = np.zeros((len(xs), len(ys)))
    F = np.zeros_like(P)
    P[ix, iy] = psi
    F[ix, iy] = force

    scale = max(np.abs(P).max(), 1e-300)
    fscale = max(np.abs(F).max(), 1e-300)
    zero = np.where(xs == 0)[0]
    if len(zero):
        z = zero[0]
        report(np.abs(P[z]).max() <= 1e-9 * scale and np.abs(F[z]).max() <= 1e-9 * fscale,
               f"psi and force vanish at {a.x} = 0")
    else:
        report(False, f"no row with {a.x} = 0 (the map should contain it: psi(0) = 0)")

    if xs[0] < 0:
        if np.allclose(xs, -xs[::-1]):
            report(np.abs(P + P[::-1]).max() <= 1e-3 * scale, "psi is odd in the current")
            report(np.abs(F - F[::-1]).max() <= 1e-3 * fscale, "force is even in the current")
        else:
            report(False, "negative currents are not mirrored by positive ones: no symmetry check")

    pos = np.where(xs >= 0)[0]
    steps = np.diff(P[pos], axis=0)
    report(bool((steps > 0).all()), "psi increases with the current for every position")

    # energy consistency (needs the point at 0 and at least four positions)
    if len(zero) and len(pos) >= 3 and len(ys) >= 4:
        xp = xs[pos]
        W = np.zeros((len(pos), len(ys)))
        for j in range(len(ys)):
            f = PchipInterpolator(xp, P[pos, j])
            W[:, j] = [f.antiderivative()(v) for v in xp]
        # W' falls roughly like a power of the gap: a spline in log W' over log y
        # follows it far better at the outer positions than one in W' over y
        FW = np.zeros_like(W)
        with np.errstate(divide="ignore", invalid="ignore"):
            for k in range(1, len(pos)):
                lw = CubicSpline(np.log(ys), np.log(W[k]))
                FW[k] = -(W[k] / ys) * lw(np.log(ys), 1)
        xmin = a.xmin if a.xmin is not None else 0.1 * xp.max()
        sel = [k for k in range(len(pos)) if xp[k] >= xmin]
        print(f"\nforce from the energy against the force column, {a.x} >= {xmin:g} (percent):")
        print("  " + a.y + " " * 8 + "min      max")
        worst = 0.0
        for j in range(len(ys)):
            dev = 100.0 * (FW[sel, j] - F[pos[sel], j]) / F[pos[sel], j]
            worst = max(worst, np.abs(dev).max())
            print(f"  {ys[j]:<10.4g}{dev.min():+7.2f}  {dev.max():+7.2f}")
        print("  (the derivative at the outer positions is the least accurate: coarse grid in "
              + a.y + ")")
        report(worst <= a.tol, f"force from psi agrees with the force column within {a.tol:g} %"
                               f" (worst {worst:.2f} %)")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
