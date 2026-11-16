"""MOSFET level 3 of the simulator against ngspice's MOS3: drain current
over gate and drain sweeps, for parameter sets that switch on one effect
after the other (velocity saturation, weak inversion, static feedback,
mobility reduction, channel length modulation, short and narrow channel),
n- and p-channel, with body bias and at a second temperature.

The bulk junctions get a tiny saturation current: ngspice approximates their
reverse current below -3 vt by -is (1 + (3 vt / (e v))^3), the simulator
keeps the exponential, which differs by some 1e-5 of the leakage; the
comparison is about the channel.

Run with:  python3 tests/mos3_ngspice.py
(needs ngspice, found through NGSPICE or on the PATH)
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "python"))
import esim  # noqa: E402

NGSPICE = os.environ.get("NGSPICE") or shutil.which("ngspice")

# name, type, model parameters, instance (w, l), temperature
CASES = [
    ("plain", "nmos", {"vto": 0.7, "kp": 1e-4, "gamma": 0.4, "phi": 0.7, "tox": 2e-8}, (10e-6, 2e-6), 27),
    ("vmax", "nmos", {"vto": 0.7, "kp": 1e-4, "gamma": 0.4, "tox": 2e-8, "vmax": 1.5e5, "u0": 500}, (10e-6, 1e-6), 27),
    ("nfs eta theta", "nmos", {"vto": 0.6, "kp": 8e-5, "gamma": 0.5, "tox": 2e-8, "nfs": 1e11, "eta": 0.3,
                               "theta": 0.1, "vmax": 2e5}, (10e-6, 1e-6), 27),
    ("nsub kappa", "nmos", {"vto": 0.7, "tox": 2.5e-8, "nsub": 1e16, "kappa": 0.4, "u0": 550}, (20e-6, 2e-6), 27),
    ("nsub vmax kappa", "nmos", {"vto": 0.7, "tox": 2.5e-8, "nsub": 3e16, "kappa": 0.3, "vmax": 1.2e5,
                                 "u0": 550}, (20e-6, 1e-6), 27),
    ("xj ld delta", "nmos", {"vto": 0.8, "tox": 2e-8, "nsub": 2e16, "xj": 2e-7, "ld": 1e-7, "delta": 0.5,
                             "vmax": 1.5e5, "nfs": 5e10}, (5e-6, 1.2e-6), 27),
    ("pmos", "pmos", {"vto": -0.87, "kp": 1.12, "phi": 0.6, "tox": 1e-7, "nfs": 4.2e10, "eta": 12000,
                      "u0": 600, "vmax": 800}, (1e-4, 1e-4), 27),
    ("hot", "nmos", {"vto": 0.6, "kp": 8e-5, "gamma": 0.5, "tox": 2e-8, "nfs": 1e11, "eta": 0.3,
                     "theta": 0.1, "vmax": 2e5}, (10e-6, 1e-6), 85),
]
VGS = (0.0, 3.0, 0.1)          # gate sweep (sign follows the type)
VDS_FOR_VGS = (0.05, 2.0)      # drain voltages of the gate sweeps
VDS = (0.0, 3.0, 0.1)          # drain sweep
VGS_FOR_VDS = (1.2, 2.5)
VBS = -1.0                     # body bias of all sweeps


def ngspice(kind, params, wl, temp, sweep, fixed):
    """sweep: ("vg" or "vd", start, stop, step); fixed: value of the other source."""
    s = 1 if kind == "nmos" else -1
    model = " ".join(f"{k}={v!r}" for k, v in params.items()) + " is=1e-24"
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out.txt")
        name, start, stop, step = sweep
        other = "vd" if name == "vg" else "vg"
        deck = ["mos3 comparison", f".model mod {kind} level=3 {model}",
                f"m1 d g 0 b mod w={wl[0]!r} l={wl[1]!r}",
                f"{name} {name[1]} 0 0", f"{other} {other[1]} 0 {s * fixed!r}", f"vb b 0 {s * VBS!r}",
                f".options temp={temp} gmin=1e-30 abstol=1e-24 reltol=1e-12 vntol=1e-15 nobypass",
                ".control", "set wr_singlescale", "option numdgt=15",
                f"dc {name} {s * start!r} {s * stop!r} {s * step!r}", f"wrdata {out} -i(vd)", ".endc", ".end"]
        path = os.path.join(tmp, "deck.cir")
        with open(path, "w") as f:
            f.write("\n".join(deck) + "\n")
        run = subprocess.run([NGSPICE, "-b", path], capture_output=True, text=True, cwd=tmp)
        if run.returncode != 0 or not os.path.exists(out):
            sys.exit(f"ngspice failed:\n{run.stdout}\n{run.stderr}")
        with open(out) as f:
            return [s * float(line.split()[1]) for line in f if line.strip()]


def simulated(kind, params, wl, temp, sweep, fixed):
    s = 1 if kind == "nmos" else -1
    name, start, stop, step = sweep
    other = "VD" if name == "vg" else "VG"
    model = {"type": kind, "level": 3, "is": 1e-24}
    model.update(params)
    netlist = {
        "options": {"temp": temp, "gmin": 1e-30},
        "models": {"mod": model},
        "components": [
            {"type": "V", "name": "VG", "nodes": ["g", "0"], "dc": 0},
            {"type": "V", "name": "VD", "nodes": ["d", "0"], "dc": 0},
            {"type": "V", "name": "VB", "nodes": ["b", "0"], "dc": s * VBS},
            {"type": "M", "name": "M1", "nodes": ["d", "g", "0", "b"], "model": "mod", "w": wl[0], "l": wl[1]},
        ],
        "analyses": [{"type": "dc", "source": name.upper(), "start": s * start, "stop": s * stop, "step": s * step}],
        "outputs": ["-i(VD)"],
    }
    netlist["components"][0 if other == "VG" else 1]["dc"] = s * fixed
    res = esim.run(netlist)
    return [s * v for v in res.dc.data["-i(VD)"]]


if not NGSPICE:
    print("mos3_ngspice: ngspice not found, skipped")
    sys.exit(0)

failures = 0
for label, kind, params, wl, temp in CASES:
    sweeps = [(("vg",) + VGS, vd) for vd in VDS_FOR_VGS] + [(("vd",) + VDS, vg) for vg in VGS_FOR_VDS]
    worst, where = 0.0, ""
    for sweep, fixed in sweeps:
        ref = ngspice(kind, params, wl, temp, sweep, fixed)
        got = simulated(kind, params, wl, temp, sweep, fixed)
        if len(ref) != len(got):
            sys.exit(f"{label}: {len(ref)} points from ngspice, {len(got)} from esim")
        top = max(abs(v) for v in ref)
        for k, (a, b) in enumerate(zip(ref, got)):
            # relative; currents below a millionth of the largest of the sweep (leakage of the junctions) count against that
            err = abs(b - a) / max(abs(a), 1e-6 * top)
            if err > worst:
                x = sweep[1] + k * sweep[3]
                worst, where = err, f"{sweep[0]} = {x:.2f} V ({'vd' if sweep[0] == 'vg' else 'vg'} = {fixed}): ngspice {a:.9g}, esim {b:.9g}"
    ok = worst < 1e-6
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {label:16s} largest deviation {worst:.1e}  {where if not ok else ''}")
if failures:
    sys.exit(f"{failures} parameter sets differ from ngspice")
