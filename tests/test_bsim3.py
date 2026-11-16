"""BSIM3 drain current of the simulator against the Python implementation
(tests/bsim3_reference.py) at 720 bias points for each of 16 parameter sets,
and the terminal charges (capMod 0 to 3, all partitions, overlaps) at 630
points for each of 25 parameter sets.

Run with:  python3 tests/test_bsim3.py
(the charges need the helper program bsim3_charges of the build, found
through BSIM3_CHARGES_BIN or in build/)
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "python"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import bsim3_reference as ref  # noqa: E402
import esim  # noqa: E402
from bsim3_cases import BIAS, CASES, CHARGE_CASES, VB, VD, VG  # noqa: E402

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

# --- Charges: intrinsic and overlap, at given terminal voltages -------------
CHARGES_BIN = os.environ.get("BSIM3_CHARGES_BIN", os.path.join(ROOT, "build", "bsim3_charges"))

for label, kind, params, temp in CHARGE_CASES:
    sign = 1.0 if kind == "nmos" else -1.0
    device = {"type": kind, "name": "M1", "nodes": ["d", "g", "s", "b"], "level": "bsim3"}
    device.update(params)
    bias = [[sign * v for v in point] for point in BIAS]
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        # the resistor only gives the circuit its ground node; it holds no charge
        ground = {"type": "R", "name": "R0", "nodes": ["s", "0"], "value": 1e12}
        json.dump({"netlist": {"options": {"temp": temp}, "components": [device, ground]}, "bias": bias}, f)
    try:
        run = subprocess.run([CHARGES_BIN, f.name], capture_output=True, text=True)
    finally:
        os.unlink(f.name)
    if run.returncode != 0:
        sys.exit(f"{CHARGES_BIN}: {run.stderr.strip()}")
    out = run.stdout
    simulated = [tuple(map(float, line.split())) for line in out.splitlines()]
    prepared = ref.Prepared(params, kind, temp)
    scale = prepared.cox_wl * prepared.mult  # charge of the channel at 1 V
    worst, where = 0.0, None
    for point, sim in zip(bias, simulated):
        expected = ref.terminal_charges(params, kind, *point, prepared=prepared)
        for k in range(4):
            error = abs(sim[k] - expected[k]) / (abs(expected[k]) + 1e-6 * scale)
            if error > worst:
                worst, where = error, (point, "dgsb"[k], sim[k], expected[k])
        if abs(sum(sim)) > 1e-12 * scale:
            worst, where = float("inf"), (point, "sum", sum(sim), 0.0)
    # near-zero charges are differences of larger ones (overlaps against the channel)
    ok = worst < 1e-7 and len(simulated) == len(BIAS)
    failures += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {label:32s} charges, largest relative difference {worst:.1e}"
          + ("" if ok else f" at vd, vg, vs, vb = {where[0]}, q{where[1]}: simulator {where[2]:.9g}, python {where[3]:.9g}"))

print("bsim3:", "ok" if failures == 0 else f"{failures} failures")
sys.exit(1 if failures else 0)
