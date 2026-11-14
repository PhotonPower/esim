"""Tests of the Python interface. Run with:  python3 python/test_esim.py"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esim  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("  FAIL", what)


def near(a, b, tol):
    return abs(a - b) <= tol


RC = {
    "parameters": {"R": "1k", "C": "1u"},
    "components": [
        {"type": "V", "name": "V1", "nodes": ["in", "0"], "dc": 1, "ac": 1},
        {"type": "R", "name": "R1", "nodes": ["in", "out"], "value": "R"},
        {"type": "C", "name": "C1", "nodes": ["out", "0"], "value": "C", "ic": 0},
    ],
    "analyses": [
        {"type": "op"},
        {"type": "ac", "sweep": "dec", "points": 50, "fstart": 1, "fstop": "100k",
         "measure": {"fc": {"f3db": "v(out)"}}},
        {"type": "tran", "tstop": "5m", "uic": True, "measure": {"vend": {"final": "v(out)"}}},
    ],
    "outputs": ["v(out)"],
}

# --- netlist as a dict ---
res = esim.run(RC)
check(len(res.analyses) == 3, "three analyses")
check(near(res.op.values["v(out)"], 1.0, 1e-9), "operating point value")
check(near(res.ac.measures["fc"], 1 / (2 * math.pi * 1e-3), 0.5), "measured corner frequency")
check(res.ac.columns == ["freq", "mag(v(out))", "phase(v(out))"], "columns of the ac analysis")
t, v = res.tran["time"], res.tran["v(out)"]
check(len(t) == len(v) and len(t) > 10, "transient columns")
check(max(abs(vi - (1 - math.exp(-ti / 1e-3))) for ti, vi in zip(t, v)) < 1e-3, "transient values")
check(res.tran.stats["steps"] > 0, "statistics of the transient analysis")

# --- parameters and analyses can be replaced ---
res = esim.run(RC, parameters={"C": 100e-9},
               analyses=[{"type": "ac", "fstart": 10, "fstop": 1e6, "points": 50,
                          "measure": {"fc": {"f3db": "v(out)"}}}])
check(near(res.ac.measures["fc"], 1 / (2 * math.pi * 1e-4), 5), "overridden parameter")

# --- parameter sweep: one table, runs() splits it ---
res = esim.run(RC, analyses=[{"type": "tran", "tstop": "5m", "uic": True,
                              "param_sweep": {"param": "C", "values": [1e-6, 2e-6]},
                              "measure": {"t50": {"cross": "v(out)", "level": 0.5}}}])
sweep = res.tran
check(sweep.sweep["param"] == "C", "sweep parameter")
check(len(sweep.measures["t50"]) == 2, "one measurement per run")
check(near(sweep.measures["t50"][1] / sweep.measures["t50"][0], 2.0, 1e-2), "t50 scales with C")
runs = list(sweep.runs())
check([value for value, _ in runs] == [1e-6, 2e-6] and "time" in runs[0][1], "runs of the sweep")

# --- a netlist file with comments, tables relative to it ---
res = esim.run(os.path.join(ROOT, "examples", "solar_cell.json"))
check(near(res.dc.measures["p_max"], 1.2388, 1e-3), "example file: maximum power")
check("v(p)*i(Vload)" in res.dc.columns, "example file: expression output")

# --- errors arrive as exceptions with the simulator's message ---
try:
    esim.run({"components": [{"type": "R", "nodes": ["a", "b"], "value": 1}]})
    check(False, "missing ground raises")
except esim.EsimError as e:
    check("ground" in str(e), "error message of the simulator")

# --- a measurement without result is None ---
res = esim.run(RC, analyses=[{"type": "tran", "tstop": "1m", "uic": True,
                              "measure": {"never": {"cross": "v(out)", "level": 5}}}])
check(res.tran.measures["never"] is None, "undefined measurement is None")

print("python interface:", "ok" if failures == 0 else f"{failures} failures")
sys.exit(1 if failures else 0)
