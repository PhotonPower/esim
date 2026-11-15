"""BSIM3 drain current of the simulator against the Python implementation
(tests/bsim3_reference.py) at 720 bias points for each of 16 parameter sets.

Run with:  python3 tests/test_bsim3.py
"""

import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import bsim3_reference as ref  # noqa: E402
import esim  # noqa: E402

# Typical 0.18 um transistors (parameter sets of the kind foundries publish).
N18 = dict(w=2e-6, l=0.18e-6, tox=4.1e-9, xj=1e-7, nch=2.3549e17, vth0=0.3694303, k1=0.5789116,
           k2=1.110723e-3, k3=1e-3, k3b=0.0297124, w0=1e-7, nlx=2.037748e-7, dvt0w=0, dvt1w=0, dvt2w=0,
           dvt0=1.2953626, dvt1=0.3421545, dvt2=0.0395588, u0=293.1687573, ua=-1.21942e-9,
           ub=2.325738e-18, uc=7.061289e-11, vsat=1.676164e5, a0=2, ags=0.4764546, b0=1.617101e-7,
           b1=5e-6, keta=-0.0138552, a1=1.09168e-3, a2=0.3303025, rdsw=105.6133217, prwg=0.5,
           prwb=-0.2, wr=1, wint=2.885735e-9, lint=1.715622e-8, xl=0, xw=-1e-8, dwg=2.754317e-9,
           dwb=-3.690793e-9, voff=-0.0948017, nfactor=2.1860065, cit=0, cdsc=2.4e-4, cdscd=0, cdscb=0,
           eta0=2.665034e-3, etab=6.028975e-5, dsub=0.0442223, pclm=1.746064, pdiblc1=0.3258185,
           pdiblc2=2.701992e-3, pdiblcb=-0.1, drout=0.9787232, pscbe1=4.494778e10, pscbe2=3.672074e-8,
           pvag=0.0122755, delta=0.01, mobmod=1, prt=0, ute=-1.5, kt1=-0.11, kt1l=0, kt2=0.022,
           ua1=4.31e-9, ub1=-7.61e-18, uc1=-5.6e-11, at=3.3e4, pvth0=-5.105777e-3, prdsw=-1.1011726,
           pk2=2.247806e-3, wketa=-5.071892e-3, lketa=5.324922e-4, pu0=-4.0206081, pua=-4.48232e-11,
           pub=5.018589e-24, pvsat=2e3, peta0=1e-4, pketa=-2.090695e-3)
P18 = dict(w=4e-6, l=0.18e-6, tox=4.1e-9, xj=1e-7, nch=4.1589e17, vth0=-0.3823437, k1=0.5722049,
           k2=0.0219717, k3=0.1576753, k3b=4.2763642, w0=1e-6, nlx=1.104212e-7, dvt0w=0, dvt1w=0,
           dvt2w=0, dvt0=0.6234839, dvt1=0.2479255, dvt2=0.1, u0=109.4682454, ua=1.31646e-9, ub=1e-21,
           uc=-1e-10, vsat=1.054892e5, a0=1.5796859, ags=0.3115024, b0=4.729297e-7, b1=1.446715e-6,
           keta=0.0298609, a1=0.3886886, a2=0.4010376, rdsw=199.1594405, prwg=0.5, prwb=-0.4947034,
           wr=1, wint=0, lint=2.93948e-8, xl=0, xw=-1e-8, dwg=-1.998034e-8, dwb=-2.481453e-9,
           voff=-0.0935653, nfactor=2, cit=0, cdsc=2.4e-4, cdscd=0, cdscb=0, eta0=3.515392e-4,
           etab=-4.804338e-4, dsub=1.215087e-5, pclm=0.96422, pdiblc1=3.026627e-3, pdiblc2=-1e-5,
           pdiblcb=-1e-3, drout=1.117016e-4, pscbe1=7.999986e10, pscbe2=8.271897e-10, pvag=0.0190118,
           delta=0.01, mobmod=1, pvth0=3.360313e-3, prdsw=10.5, pk2=2.17e-3, wketa=0.0354115,
           lketa=-2.403727e-3, pu0=-1.5089586, pua=-5.94214e-11, pub=1e-21, pvsat=50, peta0=1e-4,
           pketa=-6.740436e-3)

CASES = [
    ("defaults, L = 0.5 um", "nmos", dict(w=10e-6, l=0.5e-6), 27),
    ("defaults, L = 5 um", "nmos", dict(w=10e-6, l=5e-6), 27),
    ("0.18 um nmos", "nmos", N18, 27),
    ("0.18 um nmos, long channel", "nmos", dict(N18, l=2e-6), 27),
    ("0.18 um nmos, 125 C", "nmos", N18, 125),
    ("0.18 um nmos, -40 C", "nmos", N18, -40),
    ("0.18 um pmos", "pmos", P18, 27),
    ("0.18 um pmos, narrow", "pmos", dict(P18, w=0.3e-6), 27),
    ("mobmod 2", "nmos", dict(N18, mobmod=2), 27),
    ("mobmod 3", "nmos", dict(N18, mobmod=3, uc=-0.03), 27),
    ("poly depletion", "nmos", dict(N18, ngate=5e19), 27),
    ("impact ionization", "nmos", dict(N18, alpha0=1e-7, alpha1=0.5, beta0=18), 27),
    ("a1 < 0", "nmos", dict(N18, a1=-0.2, a2=0.6), 27),
    ("no rdsw, a2 = 1", "nmos", dict(N18, rdsw=0, prdsw=0, a1=0, a2=1), 27),
    ("k1, k2 from the doping", "nmos", dict(w=5e-6, l=0.35e-6, tox=7e-9, nch=2e17, nsub=5e16, vth0=0.55, rdsw=300), 27),
    ("narrow width effect", "nmos", dict(w=0.4e-6, l=0.35e-6, tox=7e-9, dvt0w=0.5, dvt1w=2e6, k3=20, eta0=0.3, etab=-0.5), 27),
]

VG = [round(-0.3 + 0.15 * k, 4) for k in range(15)]
VD = [0.0, 0.003, 0.02, 0.05, 0.1, 0.2, 0.35, 0.5, 0.8, 1.2, 1.8, 2.5]
VB = [0.0, -0.5, -1.8, 0.3]

failures = 0
for label, kind, params, temp in CASES:
    sign = 1.0 if kind == "nmos" else -1.0
    device = {"type": kind, "name": "M1", "nodes": ["d", "g", "0", "b"], "level": "bsim3",
              # junctions without leakage, so that the drain current is that of the channel
              "ad": 1e-12, "as": 1e-12, "js": 1e-22}
    device.update(params)
    netlist = {
        "options": {"temp": temp, "gmin": 1e-30},
        "parameters": {"vg": 0, "vd": 0, "vb": 0},
        "components": [
            {"type": "V", "name": "VG", "nodes": ["g", "0"], "dc": f"{sign}*vg"},
            {"type": "V", "name": "VD", "nodes": ["d", "0"], "dc": f"{sign}*vd"},
            {"type": "V", "name": "VB", "nodes": ["b", "0"], "dc": f"{sign}*vb"},
            device,
        ],
        "analyses": [{"type": "op", "outputs": ["-i(VD)"],
                      "param_sweep": [{"param": "vb", "values": VB}, {"param": "vg", "values": VG},
                                      {"param": "vd", "values": VD}]}],
    }
    data = esim.run(netlist).analyses[0].data
    prepared = ref.Prepared(params, kind, temp)
    worst, where = 0.0, None
    for vb, vg, vd, current in zip(data["vb"], data["vg"], data["vd"], data["-i(VD)"]):
        ids, isub = ref.drain_current(params, kind, vg, vd, vb, prepared=prepared)
        expected = ids + isub
        # 1 fA floor: the junction diodes of the simulator carry up to 1e-24 A
        error = abs(sign * current - expected) / (abs(expected) + 1e-15)
        if error > worst:
            worst, where = error, (vg, vd, vb, sign * current, expected)
    ok = worst < 1e-7 and len(data["vd"]) == len(VG) * len(VD) * len(VB)
    failures += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {label:28s} largest relative difference {worst:.1e}"
          + ("" if ok else f" at vgs, vds, vbs = {where[:3]}: simulator {where[3]:.9g}, python {where[4]:.9g}"))

print("bsim3:", "ok" if failures == 0 else f"{failures} failures")
sys.exit(1 if failures else 0)
