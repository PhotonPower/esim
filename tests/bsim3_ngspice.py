"""BSIM3 of the simulator against the BSIM3v3.3 of ngspice (level 8,
version 3.3.0): drain current and terminal charges for the parameter sets and
bias points of tests/bsim3_cases.py.

Run with:  python3 tests/bsim3_ngspice.py
(needs ngspice, found through NGSPICE or on the PATH, and the helper program
bsim3_charges of the build, found through BSIM3_CHARGES_BIN or in build/)
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import bsim3_reference as ref  # noqa: E402
import esim  # noqa: E402
from bsim3_cases import BIAS, CASES, CHARGE_CASES, VB, VD, VG  # noqa: E402

NGSPICE = os.environ.get("NGSPICE") or shutil.which("ngspice")
CHARGES_BIN = os.environ.get("BSIM3_CHARGES_BIN", os.path.join(ROOT, "build", "bsim3_charges"))
INSTANCE = {"w", "l", "m", "ad", "as", "pd", "ps", "nrd", "nrs"}


def key(*values):
    return tuple(round(v, 9) for v in values)


def ngspice(kind, params, temp, nodes, sweeps, saves):
    """Runs one deck: transistor M1 between the sources VD VG VS VB (VS only
    if the source node is not ground). sweeps: list of ({source: value},
    (start, stop, step) of VG). Returns the rows (vg, *saves) of all sweeps."""
    model = " ".join(f"{k}={v!r}" for k, v in params.items() if k not in INSTANCE)
    instance = " ".join(f"{k}={v!r}" for k, v in params.items() if k in INSTANCE)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out.txt")
        deck = [
            "bsim3 comparison",
            f".model mod {kind} level=8 version=3.3.0 {model}",
            f"m1 {' '.join(nodes)} mod {instance}",
            "vd d 0 0", "vg g 0 0", "vb b 0 0",
        ]
        if nodes[2] != "0":
            deck.append("vs s 0 0")
        # Tolerances far below the default abstol of 1 pA, at which the DC
        # sweep would stop iterating on small currents; no gmin across the
        # junctions, which would add gmin * vbd to the drain current.
        deck += [f".options temp={temp} gmin=1e-30 abstol=1e-24 reltol=1e-10 nobypass", ".control",
                 "set appendwrite", "set wr_singlescale", "option numdgt=15", f"save {' '.join(saves)}"]
        for values, (start, stop, step) in sweeps:
            deck += [f"alter v{name} = {v!r}" for name, v in values.items()]
            deck += [f"dc vg {start!r} {stop!r} {step!r}", f"wrdata {out} {' '.join(saves)}", "destroy all"]
        deck += [".endc", ".end"]
        path = os.path.join(tmp, "deck.cir")
        with open(path, "w") as f:
            f.write("\n".join(deck) + "\n")
        run = subprocess.run([NGSPICE, "-b", path], capture_output=True, text=True, cwd=tmp)
        if run.returncode != 0 or not os.path.exists(out):
            sys.exit(f"ngspice failed:\n{run.stdout}\n{run.stderr}")
        with open(out) as f:
            return [tuple(map(float, line.split())) for line in f if line.strip()]


if not NGSPICE:
    print("bsim3_ngspice: ngspice not found, skipped")
    sys.exit(0)

failures = 0
print("drain current")
for label, kind, params, temp in CASES:
    sign = 1.0 if kind == "nmos" else -1.0
    # simulator, as in tests/test_bsim3.py
    device = {"type": kind, "name": "M1", "nodes": ["d", "g", "0", "b"], "level": "bsim3",
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
    sim = {key(vb, vg, vd): sign * i for vb, vg, vd, i in zip(data["vb"], data["vg"], data["vd"], data["-i(VD)"])}
    # ngspice: drain current = -i(vd), with the same junctions (without area
    # ngspice would give them a leakage current of its own)
    sweeps = [({"b": sign * vb, "d": sign * vd}, (sign * VG[0], sign * VG[-1], sign * (VG[1] - VG[0])))
              for vb in VB for vd in VD]
    rows = ngspice(kind, dict(params, ad=1e-12, js=1e-22, **{"as": 1e-12}), temp, ["d", "g", "0", "b"],
                   sweeps, ["i(vd)"])
    if len(rows) != len(sweeps) * len(VG):
        sys.exit(f"{label}: ngspice gave {len(rows)} points instead of {len(sweeps) * len(VG)}")
    worst, where = 0.0, None
    for n, row in enumerate(rows):
        vb, vd = VB[n // (len(VD) * len(VG))], VD[n // len(VG) % len(VD)]
        vg = sign * row[0]
        expected = -sign * row[1]
        got = sim[key(vb, vg, vd)]
        error = abs(got - expected) / (abs(expected) + 1e-12)
        if error > worst:
            worst, where = error, (vg, vd, vb, got, expected)
    ok = worst < 1e-8
    failures += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {label:32s} largest relative difference {worst:.1e}"
          f" at vgs, vds, vbs = {where[:3]}: esim {where[3]:.9g}, ngspice {where[4]:.9g}")

print("charges")
for label, kind, params, temp in CHARGE_CASES:
    sign = 1.0 if kind == "nmos" else -1.0
    device = {"type": kind, "name": "M1", "nodes": ["d", "g", "s", "b"], "level": "bsim3"}
    device.update(params)
    bias = [[sign * v for v in point] for point in BIAS]
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        ground = {"type": "R", "name": "R0", "nodes": ["s", "0"], "value": 1e12}
        json.dump({"netlist": {"options": {"temp": temp}, "components": [device, ground]}, "bias": bias}, f)
    try:
        run = subprocess.run([CHARGES_BIN, f.name], capture_output=True, text=True)
    finally:
        os.unlink(f.name)
    if run.returncode != 0:
        sys.exit(f"{CHARGES_BIN}: {run.stderr.strip()}")
    sim = {key(*point): tuple(map(float, line.split())) for point, line in zip(bias, run.stdout.splitlines())}

    vgs = sorted({p[1] for p in BIAS})
    outer = [(vs, vd, vb) for vs in sorted({p[2] for p in BIAS}) for vd in sorted({p[0] for p in BIAS})
             for vb in sorted({p[3] for p in BIAS})]
    sweeps = [({"s": sign * vs, "d": sign * vd, "b": sign * vb},
               (sign * vgs[0], sign * vgs[-1], sign * (vgs[1] - vgs[0]))) for vs, vd, vb in outer]
    rows = ngspice(kind, params, temp, ["d", "g", "s", "b"], sweeps, ["@m1[qd]", "@m1[qg]", "@m1[qb]"])
    if len(rows) != len(sweeps) * len(vgs):
        sys.exit(f"{label}: ngspice gave {len(rows)} points instead of {len(sweeps) * len(vgs)}")
    scale = ref.Prepared(params, kind, temp).cox_wl * params.get("m", 1)
    worst, where = 0.0, None
    for n, row in enumerate(rows):
        vs, vd, vb = outer[n // len(vgs)]
        point = (sign * vd, row[0], sign * vs, sign * vb)
        # ngspice keeps the charges in the n-channel frame
        qd, qg, qb = (sign * q for q in row[1:])
        expected = (qd, qg, -(qd + qg + qb), qb)
        got = sim[key(*point)]
        for k in range(4):
            error = abs(got[k] - expected[k]) / (abs(expected[k]) + 1e-6 * scale)
            if error > worst:
                worst, where = error, (point, "dgsb"[k], got[k], expected[k])
    ok = worst < 1e-8
    failures += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {label:32s} largest relative difference {worst:.1e}"
          f" at vd, vg, vs, vb = {where[0]}, q{where[1]}: esim {where[2]:.9g}, ngspice {where[3]:.9g}")

print("bsim3 against ngspice:", "ok" if failures == 0 else f"{failures} failures")
sys.exit(1 if failures else 0)
