#!/usr/bin/env python3
"""Translate a SPICE netlist into the JSON input of esim.

    python3 tools/spice2json.py amplifier.cir -o amplifier.json
    ./build/esim amplifier.json

Understood: R C L K V I E G F H D Q M J X S T O, .model, .subckt/.ends, .param,
.op .dc .ac .tran .noise .four, .ic .nodeset .temp .options .print/.plot.
Sources: DC, AC, SIN, PULSE, PWL. Everything else is reported on stderr and
left out. Names and nodes are converted to lower case (SPICE ignores case).
"""

import argparse
import json
import re
import sys

SUFFIX = {"t": 1e12, "g": 1e9, "meg": 1e6, "k": 1e3, "m": 1e-3, "mil": 25.4e-6,
          "u": 1e-6, "n": 1e-9, "p": 1e-12, "f": 1e-15}
NUMBER = re.compile(r"^([+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?)([a-z]*)$")

warnings = []


def warn(msg):
    warnings.append(msg)


def value(token):
    """A SPICE number as float; a {parameter expression} or a name as string."""
    t = token.strip()
    if (t.startswith("{") and t.endswith("}")) or (t.startswith("'") and t.endswith("'")):
        return t[1:-1].strip()
    m = NUMBER.match(t.lower())
    if not m:
        return t  # the name of a parameter
    number, suffix = float(m.group(1)), m.group(2)
    if suffix.startswith("meg"):
        return number * 1e6
    if suffix.startswith("mil"):
        return number * 25.4e-6
    return number * SUFFIX.get(suffix[:1], 1.0) if suffix else number


def logical_lines(text):
    """Title, then the lines with comments removed and continuations joined."""
    raw = text.splitlines()
    title = raw[0].strip() if raw else ""
    lines = []
    for line in raw[1:]:
        line = re.split(r";|\s\$\s", line)[0].rstrip()
        if not line.strip() or line.lstrip().startswith("*"):
            continue
        if line.lstrip().startswith("+") and lines:
            lines[-1] += " " + line.lstrip()[1:]
        else:
            lines.append(line.strip())
    return title, lines


def tokens(line):
    """Split a line; parentheses and commas separate, 'a = b' becomes one token a=b."""
    line = re.sub(r"\s*=\s*", "=", line)
    keep = []  # protect {expressions}
    line = re.sub(r"\{[^}]*\}|'[^']*'", lambda m: keep.append(m.group(0)) or f"\0{len(keep) - 1}\0", line)
    parts = re.sub(r"[(),]", " ", line).split()
    return [re.sub(r"\0(\d+)\0", lambda m: keep[int(m.group(1))], p) for p in parts]


def split_params(parts):
    """Separate key=value tokens from positional ones."""
    positional, named = [], {}
    for p in parts:
        if "=" in p:
            k, v = p.split("=", 1)
            named[k.lower()] = value(v)
        else:
            positional.append(p)
    return positional, named


def source(name, parts, kind):
    """V and I sources: [DC] value, AC mag [phase], SIN(...), PULSE(...), PWL(...)."""
    comp = {"type": kind, "name": name, "nodes": [parts[0].lower(), parts[1].lower()]}
    rest = [p.lower() for p in parts[2:]]
    i = 0
    functions = {"sin", "pulse", "pwl", "exp", "sffm", "am"}

    def numbers(start):
        out, j = [], start
        while j < len(rest) and rest[j] not in functions and rest[j] not in ("dc", "ac"):
            out.append(value(rest[j]))
            j += 1
        return out, j

    while i < len(rest):
        t = rest[i]
        if t == "dc":
            vals, i = numbers(i + 1)
            if vals:
                comp["dc"] = vals[0]
        elif t == "ac":
            vals, i = numbers(i + 1)
            comp["ac"] = vals[0] if vals else 1.0
            if len(vals) > 1:
                comp["acphase"] = vals[1]
        elif t == "sin":
            v, i = numbers(i + 1)
            v += [0.0] * (6 - len(v))
            comp["wave"] = {"type": "sin", "offset": v[0], "amplitude": v[1], "freq": v[2],
                            "delay": v[3], "damping": v[4], "phase": v[5]}
        elif t == "pulse":
            v, i = numbers(i + 1)
            wave = {"type": "pulse", "v1": v[0], "v2": v[1]}
            for key, k in (("delay", 2), ("rise", 3), ("fall", 4), ("width", 5), ("period", 6)):
                # a rise or fall time of 0 means "default" here as in SPICE
                if len(v) > k and not (key in ("rise", "fall") and v[k] == 0):
                    wave[key] = v[k]
            comp["wave"] = wave
        elif t == "pwl":
            v, i = numbers(i + 1)
            comp["wave"] = {"type": "pwl", "points": [[v[k], v[k + 1]] for k in range(0, len(v) - 1, 2)]}
        elif t in functions:
            warn(f"{name}: source function {t.upper()} is not supported, ignored")
            _, i = numbers(i + 1)
        else:  # a bare value is the DC value
            comp["dc"] = value(t)
            i += 1
    return comp


def element(line, models, switch_models, line_models):
    parts = tokens(line)
    name = parts[0].lower()
    kind = name[0].upper()
    args = parts[1:]
    pos, named = split_params(args)
    node = lambda k: pos[k].lower()  # noqa: E731

    if kind in "RCL":
        comp = {"type": kind, "name": name, "nodes": [node(0), node(1)], "value": value(pos[2])}
        if "ic" in named and kind in "CL":
            comp["ic"] = named["ic"]
        return comp
    if kind == "K":
        return {"type": "K", "name": name, "inductors": [node(0), node(1)], "k": value(pos[2])}
    if kind in "VI":
        return source(name, args, kind)
    if kind in "EG":
        key = "gain" if kind == "E" else "gm"
        return {"type": kind, "name": name, "nodes": [node(0), node(1), node(2), node(3)],
                key: value(pos[4])}
    if kind in "FH":
        key = "gain" if kind == "F" else "r"
        return {"type": kind, "name": name, "nodes": [node(0), node(1)], "control": node(2),
                key: value(pos[3])}
    if kind == "D":
        return {"type": "D", "name": name, "nodes": [node(0), node(1)], "model": node(2)}
    if kind == "Q":
        # collector base emitter [substrate] model
        model = node(4) if len(pos) > 4 and node(4) in models else node(3)
        return {"type": "Q", "name": name, "nodes": [node(0), node(1), node(2)], "model": model}
    if kind == "J":
        return {"type": "J", "name": name, "nodes": [node(0), node(1), node(2)], "model": node(3)}
    if kind == "M":
        comp = {"type": "M", "name": name, "nodes": [node(0), node(1), node(2), node(3)],
                "model": node(4)}
        for key in ("w", "l"):
            if key in named:
                comp[key] = named[key]
        return comp
    if kind == "S":
        comp = {"type": "S", "name": name, "nodes": [node(0), node(1), node(2), node(3)]}
        comp.update(switch_models.get(node(4), {}))
        if node(4) not in switch_models:
            warn(f"{name}: switch model {node(4)} not found, defaults are used")
        return comp
    if kind == "T":  # lossless line: Z0 and TD, or F and NL
        comp = {"type": "T", "name": name, "nodes": [node(0), node(1), node(2), node(3)]}
        for key in ("z0", "td", "f", "nl"):
            if key in named:
                comp[key] = named[key]
        return comp
    if kind == "O":  # lossy line with an LTRA model: R, L, G, C per unit length and LEN
        model = line_models.get(node(4))
        if model is None:
            warn(f"{name}: line model {node(4)} not found, ignored")
            return None
        comp = {"type": "T", "name": name, "nodes": [node(0), node(1), node(2), node(3)]}
        comp.update(model)
        return comp
    if kind == "X":
        comp = {"type": "X", "name": name, "subcircuit": pos[-1].lower(),
                "nodes": [p.lower() for p in pos[:-1]]}
        named.pop("params:", None)
        if named:
            comp["parameters"] = named
        return comp
    warn(f"{name}: components of type {kind} are not supported, ignored")
    return None


def probes(line):
    """Outputs named in a line: v(a), v(a,b), i(v1); vm(), vdb() ... count as v()."""
    text = line.lower()
    out = []
    for m in re.finditer(r"\b([vi])[a-z]*\s*\(\s*([^)]*?)\s*\)", text):
        args = re.sub(r"\s+", "", m.group(2))
        out.append(f"{m.group(1)}({args})")
    return out


def convert(text):
    title, lines = logical_lines(text)
    result = {"title": title}
    parameters, models, switch_models, subcircuits = {}, {}, {}, {}
    line_models = {}
    options, ic, nodeset, outputs, analyses = {}, {}, {}, [], []
    components = []
    current = components  # where components are added (top level or a subcircuit)
    four = None

    # Models first: elements need to know them (Q with substrate node, switches).
    for line in lines:
        if line.lower().startswith(".model"):
            parts = tokens(line)
            name, kind = parts[1].lower(), parts[2].lower()
            if kind in ("sw", "vswitch"):
                _, named = split_params(parts[3:])
                sw = {}
                for src, dst in (("ron", "ron"), ("roff", "roff"), ("vt", "vt"), ("vh", "vh")):
                    if src in named:
                        sw[dst] = named[src]
                switch_models[name] = sw
            elif kind == "ltra":
                _, named = split_params(parts[3:])
                line_models[name] = {k: named[k] for k in ("r", "l", "g", "c", "len") if k in named}
            else:
                models[name] = re.sub(r"^\.model\s+\S+", f".model {name}", line, flags=re.I)

    for line in lines:
        low = line.lower()
        if not low.startswith("."):
            comp = element(line, models, switch_models, line_models)
            if comp:
                current.append(comp)
            continue
        parts = tokens(line)
        cmd = parts[0].lower()
        pos, named = split_params(parts[1:])
        if cmd == ".model":
            pass
        elif cmd == ".subckt":
            ports = [p.lower() for p in pos[1:] if p.lower() != "params:"]
            sub = {"ports": ports, "components": []}
            if named:
                sub["parameters"] = named
            subcircuits[pos[0].lower()] = sub
            current = sub["components"]
        elif cmd == ".ends":
            current = components
        elif cmd == ".param":
            parameters.update(named)
        elif cmd == ".op":
            analyses.append({"type": "op"})
        elif cmd == ".dc":
            analyses.append({"type": "dc", "source": pos[0].lower(), "start": value(pos[1]),
                             "stop": value(pos[2]), "step": value(pos[3])})
            if len(pos) > 4:
                warn(".dc: only the first sweep is translated")
        elif cmd == ".ac":
            analyses.append({"type": "ac", "sweep": pos[0].lower(), "points": int(value(pos[1])),
                             "fstart": value(pos[2]), "fstop": value(pos[3])})
        elif cmd == ".tran":
            numbers = [p for p in pos if p.lower() != "uic"]
            tran = {"type": "tran", "tstop": value(numbers[1]), "output_step": value(numbers[0])}
            if len(numbers) > 3:
                tran["hmax"] = value(numbers[3])
            if len(numbers) > 2 and value(numbers[2]) != 0:
                warn(".tran: a start time of the output is not supported, output starts at 0")
            if any(p.lower() == "uic" for p in pos):
                tran["uic"] = True
            analyses.append(tran)
        elif cmd == ".noise":
            out = probes(low)
            analyses.append({"type": "noise", "output": out[0] if out else pos[0].lower(),
                             "input": pos[-5].lower(), "sweep": pos[-4].lower(),
                             "points": int(value(pos[-3])), "fstart": value(pos[-2]),
                             "fstop": value(pos[-1])})
        elif cmd in (".four", ".fourier"):
            four = {"freq": value(pos[0]), "outputs": probes(low)}
        elif cmd in (".ic", ".nodeset"):
            target = ic if cmd == ".ic" else nodeset
            for k, v in named.items():
                m = re.match(r"v\((.*)\)", k)
                if m:
                    target[m.group(1)] = v
        elif cmd == ".temp":
            options["temp"] = value(pos[0])
        elif cmd in (".options", ".option"):
            for k, v in named.items():
                if k in ("reltol", "abstol", "vntol", "gmin", "tnom", "temp", "trtol"):
                    options[k] = v
                elif k == "itl1":
                    options["maxiter_dc"] = v
                elif k == "itl4":
                    options["maxiter_tran"] = v
        elif cmd in (".print", ".plot", ".probe", ".save"):
            for p in probes(low):
                if p not in outputs:
                    outputs.append(p)
        elif cmd in (".end", ".control", ".endc", ".global", ".lib", ".include", ".inc"):
            if cmd in (".lib", ".include", ".inc"):
                warn(f"{cmd}: files are not read, copy the models into the netlist")
        else:
            warn(f"{cmd}: not supported, ignored")

    if four:
        trans = [a for a in analyses if a["type"] == "tran"]
        if trans:
            trans[-1]["fourier"] = four
        else:
            warn(".four without .tran, ignored")
    for key, section in (("parameters", parameters), ("options", options), ("models", models),
                         ("subcircuits", subcircuits)):
        if section:
            result[key] = section
    result["components"] = components
    if nodeset:
        result["nodeset"] = nodeset
    if ic:
        result["ic"] = ic
    result["analyses"] = analyses
    if outputs:
        result["outputs"] = outputs
    return result


def main():
    ap = argparse.ArgumentParser(description="Translate a SPICE netlist into esim's JSON format.")
    ap.add_argument("netlist", help="SPICE netlist (.cir, .sp, .net)")
    ap.add_argument("-o", "--output", help="JSON file to write (default: standard output)")
    args = ap.parse_args()
    with open(args.netlist) as f:
        result = convert(f.read())
    text = json.dumps(result, indent=2)
    if args.output:
        with open(args.output, "w") as f:
            f.write(text + "\n")
    else:
        print(text)
    for w in warnings:
        print("warning:", w, file=sys.stderr)


if __name__ == "__main__":
    main()
