"""SPICE subcircuits read by the simulator (src/io/Spice.cpp) against ngspice:
one .subckt text with transistors, a diode, a current source, POLY(2) and
TABLE voltage sources, linear G and F, a POLY(1) H source, a B source and a
nested subcircuit with PARAMS:, run through both programs. Compared are the
node voltages inside the subcircuit over a DC sweep of the input.

Run with:  python3 tests/spice_subckt_ngspice.py
(needs ngspice, found through NGSPICE or on the PATH)
"""

import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "python"))
import esim  # noqa: E402

NGSPICE = os.environ.get("NGSPICE") or shutil.which("ngspice")

SUBCKT = """\
* a made-up amplifier that uses many kinds of elements
.subckt stage a b params: k=2
e1 b 0 a 0 {k}
.ends stage

.subckt AMP in out vcc vee params: gain=10
* differential pair with a tail current
q1 c1 in e1 qn
q2 c2 ref e1 qn
r1 vcc c1 10k
r2 vcc c2 10k
ie e1 vee dc 1m
rref ref 0 1meg
* polynomial of two controlling voltages
e1 x 0 poly(2) c1 c2 c2 0 0 {gain*0.5} 0.01 0.002 0.001 0.0005
r3 x 0 1k
g1 0 y x 0 1m
r4 y 0 2k
vsense y z 0
r5 z 0 1k
f1 0 w vsense 2
r6 w 0 500
h1 u 0 poly(1) vsense 0.1 100 1e3
r7 u 0 1k
et t 0 table {v(u)} = (-1, -1) (0, 0) (2, 1.5)
rt t 0 1k
x1 w ww stage params: k=3
rw ww 0 1k
b1 out 0 v=min(max(v(u)*0.5, -5), 5) + 0.1*v(t)
d1 out dd dmod
rd dd 0 10k
.model qn npn(is=1e-15 bf=150 vaf=80)
.model dmod d(is=1e-14 n=1.5)
.ends AMP
"""

NODES = ["out", "xa.c1", "xa.c2", "xa.x", "xa.y", "xa.w", "xa.u", "xa.t", "xa.ww", "xa.dd"]
SWEEP = (-0.004, 0.004, 0.0005)


def ngspice_rows():
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out.txt")
        deck = ["subcircuit comparison", SUBCKT,
                "vcc vcc 0 12", "vee vee 0 -12", "vin in 0 0",
                "xa in out vcc vee amp params: gain=20",
                ".options gmin=1e-15 abstol=1e-15 reltol=1e-9 vntol=1e-12",
                ".control", "set wr_singlescale", "option numdgt=15",
                f"dc vin {SWEEP[0]} {SWEEP[1]} {SWEEP[2]}",
                f"wrdata {out} " + " ".join(f"v({n})" for n in NODES),
                ".endc", ".end"]
        path = os.path.join(tmp, "deck.cir")
        with open(path, "w") as f:
            f.write("\n".join(deck) + "\n")
        run = subprocess.run([NGSPICE, "-b", path], capture_output=True, text=True, cwd=tmp)
        if run.returncode != 0 or not os.path.exists(out):
            sys.exit(f"ngspice failed:\n{run.stdout}\n{run.stderr}")
        with open(out) as f:
            return [list(map(float, line.split())) for line in f if line.strip()]


def esim_rows():
    netlist = {
        "options": {"gmin": 1e-15},
        "subcircuits": {"AMP": SUBCKT.splitlines()},
        "components": [
            {"type": "V", "name": "vcc", "nodes": ["vcc", "0"], "dc": 12},
            {"type": "V", "name": "vee", "nodes": ["vee", "0"], "dc": -12},
            {"type": "V", "name": "vin", "nodes": ["in", "0"], "dc": 0},
            {"type": "X", "name": "xa", "subcircuit": "AMP", "nodes": ["in", "out", "vcc", "vee"],
             "parameters": {"gain": 20}},
        ],
        "analyses": [{"type": "dc", "source": "vin", "start": SWEEP[0], "stop": SWEEP[1], "step": SWEEP[2]}],
        "outputs": [f"v({n})" for n in NODES],
    }
    res = esim.run(netlist)
    data = res.dc.data
    x = list(data.values())[0]
    return [[x[k]] + [data[f"v({n})"][k] for n in NODES] for k in range(len(x))]


if not NGSPICE:
    print("spice_subckt_ngspice: ngspice not found, skipped")
    sys.exit(0)

ng, es = ngspice_rows(), esim_rows()
if len(ng) != len(es):
    sys.exit(f"different number of points: ngspice {len(ng)}, esim {len(es)}")
worst, where = 0.0, ""
for a, b in zip(ng, es):
    # wr_singlescale: the sweep value once, then the vectors
    values = a[: 1 + len(NODES)]
    if abs(values[0] - b[0]) > 1e-12:
        sys.exit(f"sweep points differ: {values[0]} / {b[0]}")
    for k, n in enumerate(NODES):
        ref, got = values[k + 1], b[k + 1]
        err = abs(got - ref) / max(abs(ref), 1e-3)
        if err > worst:
            worst, where = err, f"v({n}) at vin = {b[0]:g}: ngspice {ref:.9g}, esim {got:.9g}"
print(f"largest deviation {worst:.2e} ({where})")
if worst > 1e-6:
    sys.exit("FAIL: subcircuit differs from ngspice")
print("ok")
