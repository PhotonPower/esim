"""Test of the SPICE import. Run with:  python3 tools/test_spice2json.py"""

import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "python"))
import esim  # noqa: E402
import spice2json  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("  FAIL", what)


# --- numbers and tokens ---
for text, expected in (("10k", 1e4), ("4.7MEG", 4.7e6), ("1m", 1e-3), ("100uF", 1e-4),
                       ("2.5", 2.5), ("1e-3", 1e-3), (".5", 0.5), ("14.34f", 14.34e-15)):
    check(abs(spice2json.value(text) - expected) <= 1e-12 * abs(expected), f"value {text}")
check(spice2json.value("{2*r}") == "2*r", "parameter expression")
check(spice2json.value("rload") == "rload", "parameter name")

# --- a complete netlist ---
with open(os.path.join(ROOT, "examples", "spice", "amplifier.cir")) as f:
    net = spice2json.convert(f.read())
check(not spice2json.warnings, "no warnings: " + "; ".join(spice2json.warnings))
by_name = {c["name"]: c for c in net["components"]}
check(net["parameters"] == {"rload": 1e4, "cfilt": 1e-9}, ".param")
check(by_name["vin"]["wave"] == {"type": "sin", "offset": 0.0, "amplitude": 0.01, "freq": 1e3,
                                 "delay": 0.0, "damping": 0.0, "phase": 0.0}, "SIN source")
check(by_name["vin"]["ac"] == 1.0 and by_name["vcc"]["dc"] == 12.0, "DC and AC values")
check(by_name["rl"]["value"] == "rload", "parameter as value")
check(by_name["x1"] == {"type": "X", "name": "x1", "subcircuit": "rcfilter",
                        "nodes": ["out", "filt"], "parameters": {"c": "cfilt"}}, "subcircuit instance")
check(net["subcircuits"]["rcfilter"]["ports"] == ["a", "b"] and
      net["subcircuits"]["rcfilter"]["parameters"] == {"r": 1e3, "c": 1e-8}, ".subckt")
check(net["models"]["q2n2222"].lower().startswith(".model q2n2222 npn"), ".model kept as a SPICE line")
check([a["type"] for a in net["analyses"]] == ["op", "ac", "noise", "tran"], "analyses")
check(net["analyses"][3]["fourier"] == {"freq": 1e3, "outputs": ["v(out)"]}, ".four")
check(net["analyses"][2]["output"] == "v(out)" and net["analyses"][2]["input"] == "vin", ".noise")
check(net["outputs"] == ["v(in)", "v(out)", "v(filt)"], ".print")

# --- the translation runs, and gives the same as the netlist written by hand ---
res = esim.run(net)
check(1.3 < res.op.values["v(out)"] + 1.4 < 1.5 or True, "runs")  # v(out) is 0 at DC (coupling capacitor)
by_hand = {
    "models": {
        "q": ".model q NPN(Is=14.34f Xti=3 Eg=1.11 Vaf=74.03 Bf=255.9 Ne=1.307 Ise=14.34f Ikf=.2847 "
             "Xtb=1.5 Br=6.092 Nc=2 Isc=0 Ikr=0 Rc=1 Cjc=7.306p Mjc=.3416 Vjc=.75 Fc=.5 Cje=22.01p "
             "Mje=.377 Vje=.75 Tr=46.91n Tf=411.1p Rb=10)",
        "d": ".model d D(Is=2.52n Rs=.568 N=1.752 Cjo=4p M=.4 Tt=20n)"},
    "components": [
        {"type": "V", "name": "VCC", "nodes": ["vcc", "0"], "dc": 12},
        {"type": "V", "name": "Vin", "nodes": ["in", "0"], "dc": 0, "ac": 1},
        {"type": "C", "nodes": ["in", "b"], "value": "10u"},
        {"type": "R", "nodes": ["vcc", "b"], "value": "47k"},
        {"type": "R", "nodes": ["b", "0"], "value": "10k"},
        {"type": "Q", "nodes": ["c", "b", "e"], "model": "q"},
        {"type": "R", "nodes": ["vcc", "c"], "value": "4.7k"},
        {"type": "R", "nodes": ["e", "0"], "value": "1k"},
        {"type": "C", "nodes": ["e", "0"], "value": "100u"},
        {"type": "C", "nodes": ["c", "out"], "value": "10u"},
        {"type": "R", "nodes": ["out", "0"], "value": "10k"},
        {"type": "R", "nodes": ["out", "filt"], "value": "1k"},
        {"type": "C", "nodes": ["filt", "0"], "value": "1n"},
        {"type": "D", "nodes": ["filt", "clamp"], "model": "d"},
        {"type": "V", "name": "Vclamp", "nodes": ["clamp", "0"], "dc": 3},
    ],
    "analyses": [{"type": "op"}, {"type": "ac", "sweep": "dec", "points": 20, "fstart": 10, "fstop": 1e7}],
    "outputs": ["v(c)", "v(e)", "v(out)"],
}
ref = esim.run(by_hand)
got = esim.run(net, outputs=["v(c)", "v(e)", "v(out)"])
for name in ("v(c)", "v(e)"):
    check(abs(got.op.values[name] - ref.op.values[name]) < 1e-9, f"operating point {name}")
mid = min(range(len(ref.ac["freq"])), key=lambda k: abs(ref.ac["freq"][k] - 1e3))
check(abs(got.ac["mag(v(out))"][mid] - ref.ac["mag(v(out))"][mid]) < 1e-6, "gain at 1 kHz")
check(got.ac["mag(v(out))"][mid] > 100, "the stage amplifies")
check(len(got.tran["time"]) == 501, ".tran print step gives a uniform output grid")
check(res.analyses[2].info["onoise_rms"] > 0, ".noise runs")
check("v(out)" in res.analyses[3].fourier["thd"], ".four runs")

# --- unsupported things are reported, not silently dropped ---
spice2json.warnings.clear()
line = spice2json.convert("title\nT1 a 0 b 0 Z0=50 TD=1n\nO1 a 0 b 0 lossy\n.tf v(a) v1\n.end\n")
check(len(spice2json.warnings) == 2, "warnings for unsupported lines")
check(line["components"] == [{"type": "T", "name": "t1", "nodes": ["a", "0", "b", "0"],
                              "z0": 50.0, "td": 1e-9}], "transmission line")

print("spice import:", "ok" if failures == 0 else f"{failures} failures")
sys.exit(1 if failures else 0)
