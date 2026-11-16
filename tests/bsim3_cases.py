"""Parameter sets and bias points of the BSIM3 tests (tests/test_bsim3.py
against the Python reference, tests/bsim3_ngspice.py against ngspice)."""

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

# Drain current: (label, kind, parameters, temperature), with vgs, vds, vbs from VG, VD, VB.
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

# Charges: the same kind of tuples, at the terminal voltages (vd, vg, vs, vb) of BIAS.
OVERLAP = dict(cgsl=3e-10, cgdl=3e-10, ckappa=0.8)
CHARGE_CASES = [(f"capmod {c}, xpart {x}", "nmos", dict(N18, capmod=c, xpart=x, **OVERLAP), 27)
                for c in range(4) for x in (0, 0.5, 1)]
CHARGE_CASES += [
    ("capmod 3, defaults", "nmos", dict(w=10e-6, l=0.5e-6), 27),
    ("capmod 2, defaults", "nmos", dict(w=10e-6, l=0.5e-6, capmod=2), 27),
    ("capmod 3, pmos", "pmos", dict(P18, **OVERLAP), 27),
    ("capmod 2, pmos, xpart 1", "pmos", dict(P18, capmod=2, xpart=1), 27),
    ("capmod 0, pmos", "pmos", dict(P18, capmod=0, vfbcv=-0.9), 27),
    ("capmod 3, 125 C", "nmos", N18, 125),
    ("capmod 3, noff voffcv acde moin", "nmos", dict(N18, noff=1.5, voffcv=-0.05, acde=0.6, moin=20), 27),
    ("capmod 1, noff voffcv", "nmos", dict(N18, capmod=1, noff=0.8, voffcv=0.03), 27),
    ("capmod 2, clc cle dlc dwc", "nmos", dict(N18, capmod=2, clc=2e-7, cle=0.4, dlc=3e-8, dwc=1e-8, llc=-1e-16), 27),
    ("capmod 3, poly depletion", "nmos", dict(N18, ngate=5e19), 27),
    ("capmod 3, k1 from the doping", "nmos", dict(w=5e-6, l=0.35e-6, tox=7e-9, nch=2e17, nsub=5e16, vth0=0.55), 27),
    ("capmod 3, m = 3, binned cgsl", "nmos", dict(N18, m=3, cgsl=2e-10, lcgsl=1e-11, cgdl=1e-10, wckappa=0.1), 27),
    ("xpart < 0: overlaps only", "nmos", dict(N18, xpart=-1, **OVERLAP), 27),
]
BIAS = [(vd, vg, vs, vb) for vs in (0.0, 0.4) for vd in (-0.3, 0.0, 0.02, 0.1, 0.4, 0.8, 1.8)
        for vg in [round(-1.5 + 0.25 * k, 4) for k in range(15)] for vb in (0.0, -0.6, 0.3)]
