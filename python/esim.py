"""Run the esim simulator from Python.

    import esim

    res = esim.run("examples/opamp_filter.json")
    res.ac.measures["f3db"]            # 999.97
    f, mag = res.ac["freq"], res.ac["mag(v(out))"]

    # A netlist can also be a dict, and parameters can be overridden:
    res = esim.run(netlist, parameters={"C": 220e-9})
    t, v = res.tran["time"], res.tran["v(out)"]

The module starts the esim program, lets it write its results to a temporary
directory and reads them back. Columns are numpy arrays if numpy is installed,
otherwise lists of floats. It needs nothing beyond the standard library.

The program is searched in this order: the argument `esim`, the environment
variable ESIM_BIN, build/esim next to this package, the PATH.
"""

import csv
import json
import os
import re
import shutil
import subprocess
import tempfile

try:
    import numpy as _np
except ImportError:  # the standard library alone is enough
    _np = None

__all__ = ["run", "load_netlist", "EsimError", "Result", "Analysis"]


class EsimError(RuntimeError):
    """The simulator reported an error (the message is its error text)."""


class Analysis:
    """Result of one analysis.

    type      "op", "dc", "ac" or "tran"
    name      its name in the netlist (default: the type)
    data      {column name: values}; indexing the analysis looks up a column
    values    operating point: {output: value}
    measures  {name: value}; None for a measurement without result.
              For a parameter sweep each value is a list, one entry per run.
    stats     transient: steps, rejected steps, Newton iterations, events, ...
    sweep     parameter sweep: {"param": name, "values": [...]}, else None
    warnings  not per analysis, see Result.log
    """

    def __init__(self, entry, data, measure_table):
        self.type = entry["type"]
        self.name = entry.get("name", self.type)
        self.data = data
        self.values = entry.get("values", {})
        self.stats = entry.get("stats", {})
        self.sweep = entry.get("sweep")
        self.measures = entry.get("measures", {})
        if measure_table:  # sweep: one row of measurements per parameter value
            self.measures = {k: v for k, v in measure_table.items() if k != self.sweep["param"]}

    def __getitem__(self, column):
        try:
            return self.data[column]
        except KeyError:
            raise KeyError(f"no column {column!r}; available: {', '.join(self.data)}") from None

    @property
    def columns(self):
        return list(self.data)

    def runs(self):
        """Parameter sweep: yield (parameter value, {column: values}) for every run."""
        if not self.sweep:
            raise ValueError("not a parameter sweep")
        key = self.data[self.sweep["param"]]
        for value in self.sweep["values"]:
            rows = [i for i, k in enumerate(key) if k == value]
            yield value, {c: _array([col[i] for i in rows])
                          for c, col in self.data.items() if c != self.sweep["param"]}

    def __repr__(self):
        parts = [f"{self.type} {self.name!r}"]
        if self.data:
            n = len(next(iter(self.data.values())))
            parts.append(f"{n} rows x {len(self.data)} columns")
        if self.measures:
            parts.append(f"measures: {', '.join(self.measures)}")
        return f"<Analysis {', '.join(parts)}>"


class Result:
    """All analyses of one run, in the order of the netlist.

    res.analyses           list of Analysis
    res.op / dc / ac / tran  the first analysis of that type (AttributeError if none)
    res["name"]            the analysis with that name
    res.log                what the simulator printed, including warnings
    """

    def __init__(self, title, analyses, log):
        self.title = title
        self.analyses = analyses
        self.log = log

    def _first(self, kind):
        for a in self.analyses:
            if a.type == kind:
                return a
        raise AttributeError(f"the netlist has no {kind!r} analysis")

    op = property(lambda self: self._first("op"))
    dc = property(lambda self: self._first("dc"))
    ac = property(lambda self: self._first("ac"))
    tran = property(lambda self: self._first("tran"))

    def __getitem__(self, name):
        for a in self.analyses:
            if a.name == name:
                return a
        raise KeyError(f"no analysis named {name!r}")

    def __repr__(self):
        return f"<Result {self.title!r}: {len(self.analyses)} analyses>"


def _array(values):
    return _np.asarray(values, dtype=float) if _np is not None else list(values)


def _read_csv(path):
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    header, body = rows[0], rows[1:]
    columns = list(zip(*body)) if body else [[] for _ in header]
    return {name: _array([float(v) for v in col]) for name, col in zip(header, columns)}


def _strip_comments(text):
    """Remove // and /* */ comments outside of strings (esim accepts them, json does not)."""
    pattern = r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/'
    return re.sub(pattern, lambda m: m.group(0) if m.group(0).startswith('"') else " ",
                  text, flags=re.S)


def load_netlist(path):
    """Read a netlist file into a dict (comments are allowed in the file)."""
    with open(path) as f:
        return json.loads(_strip_comments(f.read()))


def _find_program(esim):
    candidates = [esim, os.environ.get("ESIM_BIN"),
                  os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "esim"),
                  shutil.which("esim")]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    raise EsimError("esim program not found: build it (cmake --build build), "
                    "set ESIM_BIN or pass esim=<path>")


def run(netlist, parameters=None, analyses=None, outputs=None, esim=None, keep=None):
    """Simulate a netlist and return a Result.

    netlist     path of a netlist file, or the netlist as a dict
    parameters  {name: value} overriding entries of the netlist's "parameters"
    analyses    replaces the "analyses" of the netlist (a list of dicts)
    outputs     replaces the "outputs" of the netlist (a list of expressions)
    esim        path of the esim program (see the module text for the search order)
    keep        directory in which the result files are kept; default: a
                temporary directory that is removed again

    Raises EsimError with the simulator's message if the run fails.
    """
    program = _find_program(esim)
    if isinstance(netlist, (str, os.PathLike)):
        base_dir = os.path.dirname(os.path.abspath(netlist))
        spec = load_netlist(netlist)
    else:
        base_dir = os.getcwd()
        spec = json.loads(json.dumps(netlist))  # deep copy, and a check that it is plain JSON
    if parameters:
        spec.setdefault("parameters", {}).update(parameters)
    if analyses is not None:
        spec["analyses"] = analyses
    if outputs is not None:
        spec["outputs"] = outputs
    # Data files of tables are given relative to the netlist: make them absolute,
    # because the netlist is run from another directory.
    for table in spec.get("tables", {}).values():
        if isinstance(table, dict) and "file" in table and not os.path.isabs(table["file"]):
            table["file"] = os.path.join(base_dir, table["file"])

    workdir = keep or tempfile.mkdtemp(prefix="esim_")
    os.makedirs(workdir, exist_ok=True)
    try:
        netlist_path = os.path.join(workdir, "netlist.json")
        summary_path = os.path.join(workdir, "summary.json")
        with open(netlist_path, "w") as f:
            json.dump(spec, f)
        proc = subprocess.run([program, netlist_path, "-o", workdir, "--summary", summary_path],
                              capture_output=True, text=True)
        log = proc.stdout + proc.stderr
        if proc.returncode != 0:
            message = [l for l in proc.stderr.splitlines() if l.startswith("error:")]
            raise EsimError(message[-1][len("error: "):] if message else log.strip())
        with open(summary_path) as f:
            summary = json.load(f)
        result = []
        for entry in summary["analyses"]:
            data = _read_csv(entry["file"]) if entry.get("file") else {}
            table = _read_csv(entry["measure_file"]) if entry.get("measure_file") else None
            result.append(Analysis(entry, data, table))
        return Result(summary.get("title", ""), result, log)
    finally:
        if not keep:
            shutil.rmtree(workdir, ignore_errors=True)
