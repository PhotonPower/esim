#!/usr/bin/env python3
"""Plot the CSV files written by esim.

    python3 tools/plot.py results/buck_converter.tran.csv
    python3 tools/plot.py results/bjt_amplifier.ac.csv -o bode.png
    python3 tools/plot.py results/a.tran.csv results/b.tran.csv --cols "v\\(out\\)"

The kind of plot follows from the header of the file:

    time, ...                 transient: one panel each for voltages, currents and the rest
    freq, mag(..), phase(..)  frequency response: magnitude in dB and phase over log f
    <source>, ...             DC sweep over the first column
    <parameter>, time|freq    parameter sweep: one curve per parameter value

Needs matplotlib (pip install matplotlib).
"""

import argparse
import csv
import math
import os
import re
import sys
from collections import OrderedDict


def read_csv(path):
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    if len(rows) < 2:
        sys.exit(f"{path}: no data")
    header = rows[0]
    columns = [[float(v) for v in col] for col in zip(*rows[1:])]
    return header, columns


def split_groups(header, columns, force):
    """Parameter sweeps have the parameter value in front of time or freq."""
    grouped = force or (len(header) > 2 and header[1] in ("time", "freq"))
    if not grouped:
        return header, [("", columns)]
    groups = OrderedDict()
    for i, value in enumerate(columns[0]):
        groups.setdefault(value, []).append(i)
    out = []
    for value, idx in groups.items():
        out.append((f"{header[0]} = {value:g}", [[col[i] for i in idx] for col in columns[1:]]))
    return header[1:], out


def unwrap(phase):
    """Remove the jumps of a phase curve that is limited to -180 .. 180 degrees."""
    out, offset = [], 0.0
    for k, p in enumerate(phase):
        if k and abs(p - phase[k - 1]) > 180.0:
            offset -= 360.0 * round((p - phase[k - 1]) / 360.0)
        out.append(p + offset)
    return out


def panel_of(name):
    if name.startswith("v("):
        return "Spannung [V]"
    if name.startswith("i(") or name.startswith("-i("):
        return "Strom [A]"
    return "Größen"


def main():
    ap = argparse.ArgumentParser(description="Plot esim results.")
    ap.add_argument("files", nargs="+", help="CSV files written by esim")
    ap.add_argument("-o", "--output", help="write an image (png, pdf, svg) instead of opening a window")
    ap.add_argument("--cols", help="regular expression: only columns whose name matches")
    ap.add_argument("--separate", action="store_true", help="one panel per quantity")
    ap.add_argument("--group", action="store_true", help="treat the first column as sweep parameter")
    ap.add_argument("--logx", action="store_true", help="logarithmic x axis")
    ap.add_argument("--logy", action="store_true", help="logarithmic y axis")
    args = ap.parse_args()

    try:
        import matplotlib
        if args.output:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib is missing: pip install matplotlib")

    # Collect curves: panel -> list of (x, y, label)
    panels = OrderedDict()
    xlabel, ac = None, False
    for path in args.files:
        header, columns = read_csv(path)
        header, groups = split_groups(header, columns, args.group)
        file_tag = os.path.basename(path) if len(args.files) > 1 else ""
        is_ac = header[0] == "freq"
        ac = ac or is_ac
        xlabel = {"time": "Zeit [s]", "freq": "Frequenz [Hz]"}.get(header[0], header[0])
        for group_tag, cols in groups:
            x = cols[0]
            for name, y in zip(header[1:], cols[1:]):
                inner = name
                kind = None
                m = re.fullmatch(r"(mag|phase)\((.*)\)", name) if is_ac else None
                if m:
                    kind, inner = m.group(1), m.group(2)
                if args.cols and not re.search(args.cols, inner):
                    continue
                if kind == "mag":
                    panel = "Betrag [dB]"
                    y = [20.0 * math.log10(v) if v > 0 else float("nan") for v in y]
                elif kind == "phase":
                    panel = "Phase [°]"
                    y = unwrap(y)
                else:
                    panel = inner if args.separate else panel_of(inner)
                if args.separate and kind:
                    panel = f"{inner}: {panel}"
                label = ", ".join(t for t in (inner, group_tag, file_tag) if t)
                panels.setdefault(panel, []).append((x, y, label))

    if not panels:
        sys.exit("nothing to plot (check --cols)")

    fig, axes = plt.subplots(len(panels), 1, sharex=True, squeeze=False,
                             figsize=(9, 2.2 + 2.3 * len(panels)))
    for ax, (title, curves) in zip(axes[:, 0], panels.items()):
        for x, y, label in curves:
            ax.plot(x, y, label=label, linewidth=1.2)
        ax.set_ylabel(title)
        ax.grid(True, which="both", alpha=0.3)
        if ac or args.logx:
            ax.set_xscale("log")
        if args.logy:
            ax.set_yscale("log")
        if len(curves) <= 12:
            ax.legend(fontsize="small", loc="best")
    axes[-1, 0].set_xlabel(xlabel)
    fig.tight_layout()

    if args.output:
        fig.savefig(args.output, dpi=130)
        print(f"wrote {args.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
